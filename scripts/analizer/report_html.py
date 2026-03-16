"""
HTML report builder for GstShark tracer data.

Takes parsed data dicts (with timestamped samples) and produces a
self-contained HTML page with summary tables and interactive
time-series plots.

Static HTML/CSS/JS lives in the templates/ directory; this module
reads those files at build time and injects dynamic content.
"""

import json
import os
import re
from math import sqrt

# ---------------------------------------------------------------------------
# Template loading
# ---------------------------------------------------------------------------

_TEMPLATE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                             "templates")


def _load_template(name):
    """Read a template file from the templates/ directory."""
    path = os.path.join(_TEMPLATE_DIR, name)
    with open(path, "r") as fh:
        return fh.read()


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _fmt_ms(ns):
    """Format nanoseconds as milliseconds string."""
    return f"{ns / 1e6:.3f}"


def _esc(text):
    """HTML-escape a string."""
    return (str(text)
            .replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace(">", "&gt;")
            .replace('"', "&quot;"))


def _slug(text):
    """Create a URL-safe anchor id from text."""
    return re.sub(r'[^a-zA-Z0-9_-]', '_', text)


def _vals(ts_val_list):
    """Extract just the values from a list of (timestamp, value) tuples."""
    return [v for _, v in ts_val_list]


def _stats(values):
    """Return dict with count, mean, std.  Accepts plain values list."""
    if not values:
        return {"count": 0, "mean": 0.0, "std": 0.0}
    n = len(values)
    mean = sum(values) / n
    variance = sum((x - mean) ** 2 for x in values) / n if n > 1 else 0.0
    return {"count": n, "mean": mean, "std": sqrt(variance)}


def _time_series_chart(series_data, ylabel="value", div_id="", rangeslider=True):
    """Generate a <div> element with embedded data for a Plotly time-series plot."""
    MAX_PTS = 2000
    compact = []
    for s in series_data:
        pts = s["d"]
        if len(pts) > MAX_PTS:
            step = len(pts) // MAX_PTS
            pts = pts[::step]
        compact.append({"n": s["n"], "d": pts})

    id_attr = f' id="{_esc(div_id)}"' if div_id else ""
    payload = {"s": compact, "y": ylabel, "rs": rangeslider}
    data_json = json.dumps(payload, separators=(',', ':'))
    return (f'<div{id_attr} data-plotly=\'{data_json}\' '
            f'class="plot-container"></div>')


# ---------------------------------------------------------------------------
# Section builders
# ---------------------------------------------------------------------------

def _section_pipeline(svg):
    h = []
    h.append('<h2>Pipeline Diagram</h2>')
    h.append('<div class="card pipeline-diagram">')
    if svg:
        h.append(svg)
    else:
        h.append('<p class="muted">Pipeline diagram unavailable.</p>')
    h.append('</div>')
    return h


def _section_detection(det):
    h = []
    h.append('<h2>1. Test Conditions &mdash; Detection Count</h2>')
    h.append('<div class="card">')
    if det:
        h.append('<table><thead><tr>'
                 '<th>Pad</th><th class="num">Samples</th>'
                 '<th class="num">Det Mean</th><th class="num">Det Std</th>'
                 '<th class="num">Trk Mean</th><th class="num">Trk Std</th>'
                 '<th class="num">Det/Trk Ratio</th>'
                 '</tr></thead><tbody>')
        for pad in sorted(det):
            ds = _stats(_vals(det[pad]["det"]))
            ts = _stats(_vals(det[pad]["trk"]))
            ratio = f"{ds['mean'] / ts['mean']:.2f}" if ts["mean"] > 0 else "&mdash;"
            h.append(f'<tr>'
                     f'<td>{_esc(pad)}</td>'
                     f'<td class="num">{ds["count"]}</td>'
                     f'<td class="num">{ds["mean"]:.2f}</td>'
                     f'<td class="num">{ds["std"]:.2f}</td>'
                     f'<td class="num">{ts["mean"]:.2f}</td>'
                     f'<td class="num">{ts["std"]:.2f}</td>'
                     f'<td class="num">{ratio}</td></tr>')
        h.append('</tbody></table>')
        for pad in sorted(det):
            series = [
                {"n": "detector", "d": [[t, v] for t, v in det[pad]["det"]]},
                {"n": "tracker",  "d": [[t, v] for t, v in det[pad]["trk"]]},
            ]
            h.append(f'<h3 style="margin-top:1rem">{_esc(pad)}</h3>')
            h.append(_time_series_chart(series, ylabel="count", rangeslider=False))
    else:
        h.append('<p class="muted">No detection count data found.</p>')
    h.append('</div>')
    return h


def _section_framerate(fr):
    if not fr:
        return []
    h = []
    h.append('<h2>Framerate Summary</h2>')
    h.append('<div class="card">')
    h.append('<table><thead><tr>'
             '<th>Pad</th><th class="num">Mean (fps)</th><th class="num">Std</th><th class="num">Samples</th>'
             '</tr></thead><tbody>')
    for pad in sorted(fr):
        st = _stats(_vals(fr[pad]))
        h.append(f'<tr>'
                 f'<td>{_esc(pad)}</td>'
                 f'<td class="num">{st["mean"]:.1f}</td>'
                 f'<td class="num">{st["std"]:.1f}</td>'
                 f'<td class="num">{st["count"]}</td></tr>')
    h.append('</tbody></table>')
    for pad in sorted(fr):
        series = [{"n": pad, "d": [[t, v] for t, v in fr[pad]]}]
        h.append(f'<h3 style="margin-top:1rem">{_esc(pad)}</h3>')
        h.append(_time_series_chart(series, ylabel="fps"))
    h.append('</div>')
    return h


def _section_proctime(pt, pipeline_order=None, queue_names=None):
    h = []
    h.append('<h2>2. Processing Time (proctime)</h2>')
    h.append('<div class="card">')
    if pt:
        # --- Compute segments first so total/bottleneck can be scoped to them ---
        segments = []
        if pipeline_order:
            _queues = queue_names or set()
            current = []
            for name in pipeline_order:
                if name in _queues:
                    if current:
                        segments.append(current)
                    current = []
                else:
                    if name in pt:
                        current.append(name)
            if current:
                segments.append(current)

        # Sort by pipeline position when available; unknowns go at end sorted by mean
        if pipeline_order:
            pos = {name: i for i, name in enumerate(pipeline_order)}
            ranked = sorted(
                pt.items(),
                key=lambda kv: (pos.get(kv[0], len(pipeline_order)),
                                -(sum(_vals(kv[1])) / len(kv[1]))),
            )
        else:
            ranked = sorted(pt.items(),
                            key=lambda kv: sum(_vals(kv[1])) / len(kv[1]),
                            reverse=True)

        h.append('<table><thead><tr>'
                 '<th>Element</th><th class="num">Mean (ms)</th><th class="num">Std (ms)</th>'
                 '<th class="num">CV (%)</th><th class="num">Samples</th>'
                 '</tr></thead><tbody>')
        for elem, ts_vals in ranked:
            st = _stats(_vals(ts_vals))
            cv = (st["std"] / st["mean"] * 100) if st["mean"] else 0
            slug = _slug("pt_" + elem)
            h.append(f'<tr class="clickable" onclick="showPlot(\'{slug}\')">'
                     f'<td>{_esc(elem)}</td>'
                     f'<td class="num">{_fmt_ms(st["mean"])}</td>'
                     f'<td class="num">{_fmt_ms(st["std"])}</td>'
                     f'<td class="num">{cv:.1f}</td>'
                     f'<td class="num">{st["count"]}</td></tr>')
        h.append('</tbody></table>')

        # --- Inter-queue segment sums (only when pipeline order is known) ---
        if segments:
            # Compute per-segment sums and identify the bottleneck segment
            seg_sums = [sum(sum(_vals(pt[e])) / len(pt[e]) for e in seg)
                        for seg in segments]
            total_mean = sum(seg_sums)
            bn_idx = seg_sums.index(max(seg_sums))
            bn_sum = seg_sums[bn_idx]
            bn_pct = (bn_sum / total_mean * 100) if total_mean else 0
            bn_seg = segments[bn_idx]
            if len(bn_seg) <= 2:
                bn_label = ' → '.join(bn_seg)
            else:
                bn_label = f'{bn_seg[0]} → … → {bn_seg[-1]}'

            h.append('<h3 style="margin-top:1.25rem">Processing Time by Segment (between queues)</h3>')
            h.append('<table><thead><tr>'
                     '<th>#</th><th>Elements</th><th class="num">Count</th>'
                     '<th class="num">Sum of Means (ms)</th>'
                     '</tr></thead><tbody>')
            for i, (seg, seg_sum) in enumerate(zip(segments, seg_sums), 1):
                cls_hl = ' class="highlight"' if i - 1 == bn_idx else ''
                if len(seg) <= 2:
                    elem_label = ' → '.join(_esc(e) for e in seg)
                else:
                    elem_label = f'{_esc(seg[0])} → … → {_esc(seg[-1])}'
                h.append(f'<tr{cls_hl}>'
                         f'<td>{i}</td>'
                         f'<td style="font-size:.85rem">{elem_label}</td>'
                         f'<td class="num">{len(seg)}</td>'
                         f'<td class="num">{_fmt_ms(seg_sum)}</td></tr>')
            h.append('</tbody></table>')

            h.append('<div class="summary">')
            h.append(f'<div class="kv"><div class="label">Total (sum of means)</div>'
                     f'<div class="value">{_fmt_ms(total_mean)} ms</div></div>')
            h.append(f'<div class="kv"><div class="label">Bottleneck segment</div>'
                     f'<div class="value">{_esc(bn_label)} '
                     f'({_fmt_ms(bn_sum)} ms, {bn_pct:.1f}%)</div></div>')
            h.append('</div>')
        else:
            # No segment info — fall back to element-level total/bottleneck
            total_mean = sum(sum(_vals(v)) / len(v) for v in pt.values())
            bottleneck_elem = max(pt, key=lambda e: sum(_vals(pt[e])) / len(pt[e]))
            bn_mean = sum(_vals(pt[bottleneck_elem])) / len(pt[bottleneck_elem])
            bn_pct = (bn_mean / total_mean * 100) if total_mean else 0
            h.append('<div class="summary">')
            h.append(f'<div class="kv"><div class="label">Total (sum of means)</div>'
                     f'<div class="value">{_fmt_ms(total_mean)} ms</div></div>')
            h.append(f'<div class="kv"><div class="label">Bottleneck</div>'
                     f'<div class="value">{_esc(bottleneck_elem)} '
                     f'({_fmt_ms(bn_mean)} ms, {bn_pct:.1f}%)</div></div>')
            h.append('</div>')
    else:
        h.append('<p class="muted">No proctime data found.</p>')
    h.append('</div>')
    return h


def _section_rangetime(rt):
    h = []
    h.append('<h2>3. Range Time (rangetime)</h2>')
    h.append('<div class="card">')
    if rt:
        ranked = sorted(rt.items(),
                        key=lambda kv: sum(_vals(kv[1])) / len(kv[1]),
                        reverse=True)

        h.append('<table><thead><tr>'
                 '<th>Segment</th><th class="num">Mean (ms)</th><th class="num">Std (ms)</th>'
                 '<th class="num">CV (%)</th><th class="num">Samples</th>'
                 '</tr></thead><tbody>')
        for label, ts_vals in ranked:
            st = _stats(_vals(ts_vals))
            cv = (st["std"] / st["mean"] * 100) if st["mean"] else 0
            cls_hl = " highlight" if label == ranked[0][0] else ""
            slug = _slug("rt_" + label)
            h.append(f'<tr class="clickable{cls_hl}" onclick="showPlot(\'{slug}\')">'
                     f'<td>{_esc(label)}</td>'
                     f'<td class="num">{_fmt_ms(st["mean"])}</td>'
                     f'<td class="num">{_fmt_ms(st["std"])}</td>'
                     f'<td class="num">{cv:.1f}</td>'
                     f'<td class="num">{st["count"]}</td></tr>')
        h.append('</tbody></table>')

        ln_mean = sum(_vals(ranked[0][1])) / len(ranked[0][1])
        h.append(f'<div class="summary"><div class="kv">'
                 f'<div class="label">Slowest segment</div>'
                 f'<div class="value">{_esc(ranked[0][0])} '
                 f'({_fmt_ms(ln_mean)} ms)</div></div></div>')
    else:
        h.append('<p class="muted">No rangetime data found.</p>')
    h.append('</div>')
    return h


def _section_insights(det, fr, pt, rt):
    h = []
    h.append('<h2>4. Insights</h2>')
    h.append('<div class="card">')
    insights = []

    if pt:
        for elem, ts_vals in pt.items():
            st = _stats(_vals(ts_vals))
            cv = (st["std"] / st["mean"] * 100) if st["mean"] else 0
            if cv > 50:
                insights.append(
                    f'<span class="badge badge-warn">WARN</span> '
                    f'<strong>{_esc(elem)}</strong> proctime has high variability '
                    f'(CV={cv:.0f}%). Possible jitter source.')

    if rt:
        for label, ts_vals in rt.items():
            st = _stats(_vals(ts_vals))
            cv = (st["std"] / st["mean"] * 100) if st["mean"] else 0
            if cv > 50:
                insights.append(
                    f'<span class="badge badge-warn">WARN</span> '
                    f'Rangetime <strong>{_esc(label)}</strong> has high variability '
                    f'(CV={cv:.0f}%).')

    if det and fr:
        avg_det = sum(
            sum(_vals(d["det"])) / len(d["det"]) for d in det.values()
        ) / len(det)
        avg_fps = sum(sum(_vals(v)) / len(v) for v in fr.values()) / len(fr)
        if avg_fps > 0:
            det_per_frame = avg_det / avg_fps
            insights.append(
                f'<span class="badge badge-info">INFO</span> '
                f'Average detections per frame: {det_per_frame:.1f}')

    if insights:
        h.append('<ul>')
        for item in insights:
            h.append(f'<li style="margin:.4rem 0">{item}</li>')
        h.append('</ul>')
    else:
        h.append('<p class="muted">No anomalies detected.</p>')
    h.append('</div>')
    return h


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def build_report(data, pipeline_svg=None, pipeline_order=None, queue_names=None):
    """Build a complete HTML report from parsed tracer data.

    Loads templates from the templates/ directory, generates dynamic
    section HTML, and assembles the final page.
    """
    det = data["detection"]
    fr = data["framerate"]
    pt = data["proctime"]
    rt = data["rangetime"]

    # Build dynamic section content
    sections = []
    if pipeline_svg is not None:
        sections.extend(_section_pipeline(pipeline_svg))
    sections.extend(_section_detection(det))
    sections.extend(_section_framerate(fr))
    sections.extend(_section_proctime(pt, pipeline_order=pipeline_order, queue_names=queue_names))
    sections.extend(_section_rangetime(rt))
    sections.extend(_section_insights(det, fr, pt, rt))
    sections_html = "\n".join(sections)

    # Load templates
    base_html = _load_template("base.html")
    modal_html = _load_template("modal.html")
    chart_js = _load_template("chart.js")

    # Build the PLOT_DATA script for modal charts
    MAX_PTS = 2000
    plot_data = {}
    for elem, ts_vals in pt.items():
        slug = _slug("pt_" + elem)
        pts = [[t, v / 1e6] for t, v in ts_vals]
        if len(pts) > MAX_PTS:
            pts = pts[::len(pts) // MAX_PTS]
        plot_data[slug] = {"t": elem + " \u2014 proctime",
                           "y": "ms", "s": [{"n": elem, "d": pts}]}
    for label, ts_vals in rt.items():
        slug = _slug("rt_" + label)
        pts = [[t, v / 1e6] for t, v in ts_vals]
        if len(pts) > MAX_PTS:
            pts = pts[::len(pts) // MAX_PTS]
        plot_data[slug] = {"t": label + " \u2014 rangetime",
                           "y": "ms", "s": [{"n": label, "d": pts}]}
    plot_data_script = ('<script>var PLOT_DATA='
                        + json.dumps(plot_data, separators=(',', ':'))
                        + ';</script>')

    scripts_html = plot_data_script + "\n<script>\n" + chart_js + "\n</script>"

    # Assemble final page using placeholder comments in base.html
    page = base_html
    page = page.replace("<!-- SECTIONS -->", sections_html)
    page = page.replace("<!-- MODAL -->", modal_html)
    page = page.replace("<!-- SCRIPTS -->", scripts_html)

    return page
