#!/usr/bin/env python3

"""
GstShark Interlatency Analyzer

Parses babeltrace output from a GstShark CTF trace directory and prints
interlatency statistics. With --output it also writes an HTML report.

By default there is one graph per to_pad, with one line per from_pad.
Use --by-from to invert this: one graph per from_pad, one line per to_pad.

Pad names have the form <element>_<pad> (e.g. "queue0_src"). The
--exclude / --include patterns are regexes searched in those names.

Usage:
    gstshark-interlatency <CTF_DIR> [--by-from] [--exclude PATTERN ...]
                          [--include PATTERN ...] [--output FILE]
"""

import argparse
import importlib.util
import json
import os
import re
import sys
from collections import defaultdict

# Allow importing sibling module regardless of how the script is invoked
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from report_html import (_esc, _fmt_ms, _load_template, _slug, _stats,
                         _time_series_chart, _vals)

# gstshark-report.py is not importable by name because of the dash
_spec = importlib.util.spec_from_file_location(
    "gstshark_report",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "gstshark-report.py"))
_report = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_report)
run_babeltrace = _report.run_babeltrace
_parse_ts = _report._parse_ts


RE_INTERLATENCY = re.compile(
    r"\[(?P<ts>[^\]]+)\].*?\binterlatency\b.*?"
    r'from_pad\s*=\s*"(?P<from_pad>[^"]+)".*?'
    r'to_pad\s*=\s*"(?P<to_pad>[^"]+)".*?'
    r"\b_?time\s*=\s*(?P<time>\d+)"
)


# ---------------------------------------------------------------------------
# Parser
# ---------------------------------------------------------------------------

def _matches_any(name, patterns):
    return any(p.search(name) for p in patterns)


def parse_interlatency(text, by_from=False, exclude=(), include=()):
    """Parse babeltrace text into {graph_pad: {line_pad: [(ts, ns), ...]}}.

    Graphs are keyed by to_pad (or from_pad when by_from is set).  An entry
    is dropped when either pad matches an exclude pattern, or when include
    patterns are given and neither pad matches one.
    """
    data = defaultdict(lambda: defaultdict(list))

    for line in text.splitlines():
        m = RE_INTERLATENCY.search(line)
        if not m:
            continue

        from_pad = m.group("from_pad")
        to_pad = m.group("to_pad")
        if exclude and (_matches_any(from_pad, exclude) or
                        _matches_any(to_pad, exclude)):
            continue
        if include and not (_matches_any(from_pad, include) or
                            _matches_any(to_pad, include)):
            continue

        graph, series = (from_pad, to_pad) if by_from else (to_pad, from_pad)
        data[graph][series].append((_parse_ts(m.group("ts")), int(m.group("time"))))

    return {g: dict(s) for g, s in data.items()}


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------

def _percentile(values, pct):
    """Compute the p-th percentile (0-100) of an unsorted list."""
    if not values:
        return 0.0
    s = sorted(values)
    k = (len(s) - 1) * pct / 100.0
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def _line_stats(ts_vals):
    vals = _vals(ts_vals)
    st = _stats(vals)
    st["p95"] = _percentile(vals, 95)
    st["max"] = max(vals)
    return st


def _section_graph(graph, lines, graph_role, line_role, plot_data):
    h = []
    h.append(f'<h2>{graph_role}: {_esc(graph)}</h2>')
    h.append('<div class="card">')

    ranked = sorted(lines.items(),
                    key=lambda kv: _stats(_vals(kv[1]))["mean"],
                    reverse=True)

    h.append('<table><thead><tr>'
             f'<th>{line_role}</th><th class="num">Samples</th>'
             '<th class="num">Mean (ms)</th><th class="num">Std (ms)</th>'
             '<th class="num">P95 (ms)</th><th class="num">Max (ms)</th>'
             '</tr></thead><tbody>')
    for name, ts_vals in ranked:
        st = _line_stats(ts_vals)
        slug = _slug(f"il_{graph}__{name}")
        plot_data[slug] = {"t": f"{name} → {graph}" if graph_role == "to_pad"
                           else f"{graph} → {name}",
                           "y": "ms",
                           "s": [{"n": name, "d": [[t, v / 1e6] for t, v in ts_vals]}]}
        h.append(f'<tr class="clickable" onclick="showPlot(\'{slug}\')">'
                 f'<td>{_esc(name)}</td>'
                 f'<td class="num">{st["count"]}</td>'
                 f'<td class="num">{_fmt_ms(st["mean"])}</td>'
                 f'<td class="num">{_fmt_ms(st["std"])}</td>'
                 f'<td class="num">{_fmt_ms(st["p95"])}</td>'
                 f'<td class="num">{_fmt_ms(st["max"])}</td></tr>')
    h.append('</tbody></table>')

    series = [{"n": name, "d": [[t, v / 1e6] for t, v in ts_vals]}
              for name, ts_vals in ranked]
    h.append(_time_series_chart(series, ylabel="latency (ms)"))
    h.append('</div>')
    return h


def build_sections(data, by_from=False):
    """Return (section HTML lines, modal PLOT_DATA) for every graph.

    gstshark-report.py embeds these in the processing report (--interlatency).
    """
    graph_role, line_role = ("from_pad", "to_pad") if by_from else ("to_pad", "from_pad")

    plot_data = {}
    sections = []
    for graph in sorted(data):
        sections.extend(_section_graph(graph, data[graph], graph_role,
                                       line_role, plot_data))

    # Modal charts are not downsampled by chart.js, so cap them here
    MAX_PTS = 2000
    for entry in plot_data.values():
        for s in entry["s"]:
            if len(s["d"]) > MAX_PTS:
                s["d"] = s["d"][::len(s["d"]) // MAX_PTS]
    return sections, plot_data


def build_report(data, by_from=False):
    sections, plot_data = build_sections(data, by_from=by_from)

    scripts_html = ('<script>var PLOT_DATA='
                    + json.dumps(plot_data, separators=(',', ':'))
                    + ';</script>\n<script>\n'
                    + _load_template("chart.js") + '\n</script>')

    page = _load_template("base.html")
    page = page.replace("GstShark Processing Report", "GstShark Interlatency Report")
    page = page.replace("<!-- SECTIONS -->", "\n".join(sections))
    page = page.replace("<!-- MODAL -->", _load_template("modal.html"))
    page = page.replace("<!-- SCRIPTS -->", scripts_html)
    return page


def build_summary(data, by_from=False):
    graph_role, line_role = ("from_pad", "to_pad") if by_from else ("to_pad", "from_pad")
    lines = []
    for graph in sorted(data):
        lines.append(f"\n{graph_role}: {graph}")
        lines.append(f"  {line_role:<40} {'samples':>8} {'mean ms':>10} "
                     f"{'p95 ms':>10} {'max ms':>10}")
        for name, ts_vals in sorted(data[graph].items()):
            st = _line_stats(ts_vals)
            lines.append(f"  {name:<40} {st['count']:>8} {_fmt_ms(st['mean']):>10} "
                         f"{_fmt_ms(st['p95']):>10} {_fmt_ms(st['max']):>10}")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def _compile(patterns):
    try:
        return [re.compile(p) for p in patterns]
    except re.error as exc:
        sys.exit(f"Error: invalid pattern: {exc}")


def main():
    parser = argparse.ArgumentParser(
        description="Plot GstShark interlatency data: one graph per to_pad, "
                    "one line per from_pad (or the inverse with --by-from)."
    )
    parser.add_argument(
        "ctf_dir",
        help="Path to the GstShark CTF trace directory",
    )
    parser.add_argument(
        "-r", "--by-from",
        action="store_true",
        help="One graph per from_pad with one line per to_pad",
    )
    parser.add_argument(
        "-x", "--exclude",
        action="append",
        default=[],
        metavar="PATTERN",
        help="Drop entries whose from_pad or to_pad matches this regex (repeatable)",
    )
    parser.add_argument(
        "-i", "--include",
        action="append",
        default=[],
        metavar="PATTERN",
        help="Keep only entries whose from_pad or to_pad matches this regex (repeatable)",
    )
    parser.add_argument(
        "-o", "--output",
        default=None,
        help="Write the HTML report to FILE (no report is written otherwise)",
    )
    args = parser.parse_args()

    if not os.path.isdir(args.ctf_dir):
        sys.exit(f"Error: '{args.ctf_dir}' is not a directory")

    raw = run_babeltrace(args.ctf_dir)
    if not raw.strip():
        sys.exit("Error: babeltrace produced no output. Is the directory a valid CTF trace?")

    data = parse_interlatency(raw, by_from=args.by_from,
                              exclude=_compile(args.exclude),
                              include=_compile(args.include))
    if not data:
        sys.exit("Error: no interlatency data found (check the tracer and filters).")

    print(build_summary(data, by_from=args.by_from))

    if args.output:
        with open(args.output, "w") as fh:
            fh.write(build_report(data, by_from=args.by_from) + "\n")
        print(f"\nReport written to {args.output}")


if __name__ == "__main__":
    main()
