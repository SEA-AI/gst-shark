#!/usr/bin/env python3

"""
GstShark Report Generator

Parses babeltrace output from a GstShark CTF trace directory and produces
an HTML report with:
  1. Pipeline diagram (optional, from a GStreamer DOT file)
  2. Test overview — detection count statistics (detector & tracker)
  3. Per-element processing time (proctime) statistics
  4. Per-element range time (rangetime) statistics

Usage:
    gstshark-report <CTF_DIR> [--dot FILE] [--filter PATTERN] [--output FILE]
"""

import argparse
import os
import re
import subprocess
import sys
from collections import defaultdict

# Allow importing sibling module regardless of how the script is invoked
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from report_html import build_report, build_summary


# ---------------------------------------------------------------------------
# DOT file pipeline extractor
# ---------------------------------------------------------------------------

def extract_pipeline_subgraph(dot_content):
    """Extract the pipeline portion between nvstreammux and nvstreamdemux.

    Builds an element-level graph from the GStreamer DOT file via BFS both
    forward from nvstreammux and backward from nvstreamdemux, then emits a
    simplified DOT containing only those elements.  Returns None if either
    element is absent.
    """
    # --- 1. Collect outer element clusters (skip _sink / _src sub-clusters) ---
    cluster_re = re.compile(r'subgraph\s+cluster_(\w+)\s*\{')
    elements = {}  # unique_id -> display_name
    for m in cluster_re.finditer(dot_content):
        uid = m.group(1)
        if uid.endswith('_sink') or uid.endswith('_src'):
            continue
        name = re.sub(r'_0x[0-9a-fA-F]+$', '', uid)
        elements[uid] = name

    # --- 2. Locate nvstreammux and nvstreamdemux ---
    mux_id = next(
        (u for u in elements if 'nvstreammux' in elements[u] and 'demux' not in elements[u]),
        None,
    )
    demux_id = next(
        (u for u in elements if 'nvstreamdemux' in elements[u]),
        None,
    )
    if not mux_id or not demux_id:
        return None

    # --- 3. Parse directed edges (skip invis internal ones) ---
    edge_re = re.compile(r'^\s*(\w+)\s*->\s*(\w+)\s*(?:\[([^\]]*)\])?', re.MULTILINE)
    uid_sorted = sorted(elements, key=len, reverse=True)

    def find_element(pad_id):
        for uid in uid_sorted:
            if pad_id.startswith(uid + '_'):
                return uid
        return None

    successors = {}  # element_uid -> set of element_uids
    for m in edge_re.finditer(dot_content):
        attrs = m.group(3) or ''
        if 'invis' in attrs:
            continue
        src_elem = find_element(m.group(1))
        dst_elem = find_element(m.group(2))
        if src_elem and dst_elem and src_elem != dst_elem:
            successors.setdefault(src_elem, set()).add(dst_elem)

    # --- 4. BFS forward from mux ---
    reachable_from_mux = set()
    queue = [mux_id]
    while queue:
        node = queue.pop(0)
        if node in reachable_from_mux:
            continue
        reachable_from_mux.add(node)
        for succ in successors.get(node, []):
            queue.append(succ)

    # --- 5. BFS backward from demux ---
    predecessors = {}
    for src, dsts in successors.items():
        for dst in dsts:
            predecessors.setdefault(dst, set()).add(src)

    reachable_to_demux = set()
    queue = [demux_id]
    while queue:
        node = queue.pop(0)
        if node in reachable_to_demux:
            continue
        reachable_to_demux.add(node)
        for pred in predecessors.get(node, []):
            queue.append(pred)

    # --- 6. Build simplified DOT for elements on both paths ---
    on_path = reachable_from_mux & reachable_to_demux
    on_path_succ = {
        src: [d for d in dsts if d in on_path]
        for src, dsts in successors.items()
        if src in on_path
    }
    lines = [
        'digraph pipeline {',
        '  rankdir=LR;',
        '  fontname="sans";',
        '  fontsize="10";',
        '  node [style="filled,rounded", shape=box, fontsize="9", fontname="sans", fillcolor="#aaffaa"];',
        '  edge [fontsize="7", fontname="monospace"];',
    ]
    for uid in on_path:
        lines.append(f'  {uid} [label="{elements[uid]}"];')
    for src, dsts in on_path_succ.items():
        for dst in dsts:
            lines.append(f'  {src} -> {dst};')
    lines.append('}')

    # --- 7. Topological order (Kahn's algorithm, starting from mux) ---
    in_deg = {u: 0 for u in on_path}
    for dsts in on_path_succ.values():
        for dst in dsts:
            in_deg[dst] += 1

    from collections import deque
    # Only seed with zero-in-degree nodes; mux_id goes first
    starts = sorted((u for u in on_path if in_deg[u] == 0),
                    key=lambda u: 0 if u == mux_id else 1)
    topo_q = deque(starts)
    topo_order = []
    processed = set()
    while topo_q:
        node = topo_q.popleft()
        if node in processed:
            continue
        processed.add(node)
        topo_order.append(elements[node])
        for succ in on_path_succ.get(node, []):
            in_deg[succ] -= 1
            if in_deg[succ] == 0:
                topo_q.append(succ)

    # --- 8. Identify GstQueue elements on path from DOT labels ---
    queue_name_re = re.compile(r'label\s*=\s*"GstQueue[^"]*\\n([^"]+)"')
    queue_names = {m.group(1).strip() for m in queue_name_re.finditer(dot_content)}
    # Keep only those that are actually on the path
    path_names = set(elements[u] for u in on_path)
    queue_names &= path_names

    return '\n'.join(lines), topo_order, queue_names


def dot_to_svg(dot_content):
    """Render DOT source to an inline SVG string via graphviz.  Returns None on failure."""
    try:
        result = subprocess.run(
            ['dot', '-Tsvg'],
            input=dot_content,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            check=True,
        )
        svg = result.stdout
        start = svg.find('<svg')
        return svg[start:] if start >= 0 else svg
    except (FileNotFoundError, subprocess.CalledProcessError):
        return None


# ---------------------------------------------------------------------------
# Babeltrace runner
# ---------------------------------------------------------------------------

def run_babeltrace(ctf_dir):
    """Run babeltrace and return the full text output."""
    try:
        result = subprocess.run(
            ["babeltrace", ctf_dir],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            check=True,
        )
        return result.stdout
    except FileNotFoundError:
        sys.exit("Error: 'babeltrace' not found. Please install it.")
    except subprocess.CalledProcessError as exc:
        sys.exit(f"Error: babeltrace failed:\n{exc.stderr}")


# ---------------------------------------------------------------------------
# Parsers
# ---------------------------------------------------------------------------

RE_PROCTIME = re.compile(
    r"\[(?P<ts>[^\]]+)\].*?\bproctime\b.*?"
    r'element\s*=\s*"(?P<element>[^"]+)".*?'
    r'\btime\s*=\s*(?P<time>\d+)'
)

RE_RANGETIME = re.compile(
    r"\[(?P<ts>[^\]]+)\].*?\brangetime\b.*?"
    r'element\s*=\s*"(?P<element>[^"]+)".*?'
    r'\btime\s*=\s*(?P<time>\d+)'
)

RE_DETECTION = re.compile(
    r"\[(?P<ts>[^\]]+)\].*?\bdetectioncount\b.*?"
    r'pad\s*=\s*"(?P<pad>[^"]+)".*?'
    r"detector_count\s*=\s*(?P<det>\d+).*?"
    r"tracker_count\s*=\s*(?P<trk>\d+)"
)

RE_FRAMERATE = re.compile(
    r"\[(?P<ts>[^\]]+)\].*?\bframerate\b.*?"
    r'pad\s*=\s*"(?P<pad>[^"]+)".*?'
    r"\bfps\s*=\s*(?P<fps>\d+)"
)

RE_QUEUELEVEL = re.compile(
    r"\[(?P<ts>[^\]]+)\].*?\bqueuelevel\b.*?"
    r'queue\s*=\s*"(?P<queue>[^"]+)".*?'
    r'size_buffers\s*=\s*(?P<size_buffers>\d+).*?'
    r'max_size_buffers\s*=\s*(?P<max_buffers>\d+)'
)


def _parse_ts(ts_str):
    """Parse babeltrace timestamp 'HH:MM:SS.NNNNNNNNN' to seconds (float)."""
    m = re.match(r"(\d+):(\d{2}):(\d{2})\.(\d+)", ts_str)
    if not m:
        return 0.0
    h, mi, s, frac = m.groups()
    return int(h) * 3600 + int(mi) * 60 + int(s) + int(frac) / (10 ** len(frac))


def parse_log(text, filter_pattern=None):
    """Parse babeltrace text, return dicts of collected data.

    Each value list stores (timestamp_s, value) tuples so time-series
    plots can be generated.
    """
    proctime = defaultdict(list)
    rangetime = defaultdict(list)
    detection = defaultdict(lambda: {"det": [], "trk": []})
    framerate = defaultdict(list)
    ql_buffers = defaultdict(list)   # queue -> [(ts, size_buffers)]
    ql_capacity: dict[str, int] = {}  # queue -> max_size_buffers

    for line in text.splitlines():
        if filter_pattern and not filter_pattern.search(line):
            continue

        m = RE_PROCTIME.search(line)
        if m:
            ts = _parse_ts(m.group("ts"))
            proctime[m.group("element")].append((ts, int(m.group("time"))))
            continue

        m = RE_RANGETIME.search(line)
        if m:
            ts = _parse_ts(m.group("ts"))
            rangetime[m.group("element")].append((ts, int(m.group("time"))))
            continue

        m = RE_DETECTION.search(line)
        if m:
            pad = m.group("pad")
            ts = _parse_ts(m.group("ts"))
            detection[pad]["det"].append((ts, int(m.group("det"))))
            detection[pad]["trk"].append((ts, int(m.group("trk"))))
            continue

        m = RE_FRAMERATE.search(line)
        if m:
            ts = _parse_ts(m.group("ts"))
            framerate[m.group("pad")].append((ts, int(m.group("fps"))))
            continue

        m = RE_QUEUELEVEL.search(line)
        if m:
            ts = _parse_ts(m.group("ts"))
            q = m.group("queue")
            ql_buffers[q].append((ts, int(m.group("size_buffers"))))
            ql_capacity[q] = int(m.group("max_buffers"))

    queuelevel = {q: {"size_buffers": ql_buffers[q], "capacity": ql_capacity.get(q, 0)}
                  for q in ql_buffers}

    return {
        "proctime": dict(proctime),
        "rangetime": dict(rangetime),
        "detection": dict(detection),
        "framerate": dict(framerate),
        "queuelevel": queuelevel,
    }


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Generate a GstShark processing report from a CTF trace directory."
    )
    parser.add_argument(
        "ctf_dir",
        help="Path to the GstShark CTF trace directory",
    )
    parser.add_argument(
        "-f", "--filter",
        default=None,
        help="Regex filter — only lines matching this pattern are analysed",
    )
    parser.add_argument(
        "-d", "--dot",
        default=None,
        metavar="FILE",
        help="GStreamer DOT file; the nvstreammux→nvstreamdemux sub-pipeline will be included in the report",
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Write the report to FILE instead of auto-generated path",
    )
    args = parser.parse_args()

    if not os.path.isdir(args.ctf_dir):
        sys.exit(f"Error: '{args.ctf_dir}' is not a directory")

    # --- Optional pipeline diagram and order ---
    pipeline_svg = None
    pipeline_order = None
    queue_names = None
    if args.dot:
        if not os.path.isfile(args.dot):
            sys.exit(f"Error: DOT file '{args.dot}' not found")
        with open(args.dot, 'r') as fh:
            dot_raw = fh.read()
        result = extract_pipeline_subgraph(dot_raw)
        if result is None:
            print("Warning: nvstreammux or nvstreamdemux not found in DOT file; skipping diagram.")
        else:
            subgraph_dot, pipeline_order, queue_names = result
            pipeline_svg = dot_to_svg(subgraph_dot)
            if pipeline_svg is None:
                print("Warning: 'dot' (graphviz) not found or failed; skipping diagram.")

    raw = run_babeltrace(args.ctf_dir)

    if not raw.strip():
        sys.exit("Error: babeltrace produced no output. Is the directory a valid CTF trace?")

    filt = re.compile(args.filter) if args.filter else None
    data = parse_log(raw, filter_pattern=filt)
    report = build_report(data, pipeline_svg=pipeline_svg, pipeline_order=pipeline_order,
                          queue_names=queue_names)

    output = args.output
    if not output:
        output = os.path.join(os.path.dirname(os.path.abspath(args.ctf_dir)),
                              "gstshark_report.html")
    with open(output, "w") as fh:
        fh.write(report + "\n")

    summary = build_summary(data, pipeline_order=pipeline_order, queue_names=queue_names)
    print(summary)


if __name__ == "__main__":
    main()
