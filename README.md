# GstShark

> A Front End for GstTracer

GstShark is an open-source benchmarking and profiling tool for GStreamer 1.8.1
and later. It leverages GStreamer's tracing subsystem by installing a set of
custom hooks at trace points, extracting performance data and presenting it in a
graphical, friendly way.

## Table of Contents

- [Getting Started](#getting-started)
  - [Building and Installing](#building-and-installing)
- [Generating Trace Files](#generating-trace-files)
  - [Enable debug output](#enable-debug-output)
  - [Select tracers](#select-tracers)
  - [Output location](#output-location)
  - [Environment variables reference](#environment-variables-reference)
- [Available Tracers](#available-tracers)
  - [interlatency](#interlatency)
  - [proctime](#proctime)
  - [framerate](#framerate)
  - [scheduletime](#scheduletime)
  - [cpuusage](#cpuusage)
  - [graphic](#graphic)
  - [bitrate](#bitrate)
  - [queuelevel](#queuelevel)
  - [buffer](#buffer)
- [Custom Tracers](#custom-tracers)
  - [detectioncount](#detectioncount)
  - [rangetime](#rangetime)
- [Common Parameter: infer-only](#common-parameter-infer-only)
- [Visualising Results with gstshark-plot](#visualising-results-with-gstshark-plot)

---

## Getting Started

### Building and Installing

```bash
# Clone
git clone https://github.com/SEA-AI/gst-shark.git
cd gst-shark

# Configure (meson)
meson builddir --prefix /usr/

# Build
ninja -C builddir

# Install
sudo ninja install -C builddir
```

## Generating Trace Files

### Enable debug output

GstShark tracers log at GStreamer debug level 7 (`TRACE`) under the
`GST_TRACER` category. Use the following environment variables when launching
your pipeline:

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="<tracer-list>" gst-launch-1.0 ...
```

### Select tracers

List one or more tracers separated by `;`. Parameters are passed in parentheses:

```bash
# Single tracer
GST_TRACERS="framerate"

# Multiple tracers
GST_TRACERS="proctime;framerate;interlatency"

# Tracer with parameters
GST_TRACERS="proctime(filter=nvinfer);framerate(filter=nvinfer|fakesink)"
```

### Output location

CTF binary trace files are written to a timestamped directory in the current
working directory by default (`gstshark_YYYY-MM-DD_HH:MM:SS/`).

```bash
# Custom location
export GST_SHARK_LOCATION=/tmp/my_trace

# Disable file output (log to terminal only)
export GST_SHARK_CTF_DISABLE=TRUE

# Reset to default
unset GST_SHARK_LOCATION
unset GST_SHARK_CTF_DISABLE
```

### Environment variables reference

| Variable | Description |
|---|---|
| `GST_TRACERS` | Semicolon-separated list of tracers to enable |
| `GST_DEBUG` | GStreamer debug level; use `GST_TRACER:7` for tracer output |
| `GST_SHARK_LOCATION` | Output directory for CTF trace files |
| `GST_SHARK_CTF_DISABLE` | Set to any value to suppress file output |
| `GST_SHARK_FILE_BUFFERING` | `0` = no buffering; positive integer = full buffering with that byte size |

---

## Available Tracers

All tracers accept an optional `filter` parameter that limits measurement to
elements whose names contain the given substring (or `|`-separated list of
substrings):

```bash
GST_TRACERS="proctime(filter=nvinfer|nvtracker)"
```

### interlatency

Measures the time a buffer takes to travel from the source pad of one element
to each subsequent element in the pipeline. Useful for identifying which
pipeline segment contributes the most to overall latency.

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="interlatency" \
    gst-launch-1.0 videotestsrc ! queue ! fakesink sync=true
```

Log fields: `from_pad`, `to_pad`, `time`.

### proctime

Measures the time each filter or filter-like element takes to process a single
buffer (from sink pad arrival to source pad push). Source and sink elements are
not measured. Useful for identifying which element is the processing bottleneck.

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="proctime" \
    gst-launch-1.0 videotestsrc ! identity sleep-time=50000 ! fakesink
```

Log fields: `element`, `time`.

Supports the [`infer-only`](#common-parameter-infer-only) parameter when built
with DeepStream.

### framerate

Reports the number of frames passing through every source pad per second.
Updated once per second. Useful for verifying frame rate requirements and
detecting frame drops.

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="framerate" \
    gst-launch-1.0 videotestsrc ! videorate max-rate=15 ! fakesink sync=true
```

Log fields: `pad`, `fps`.

### scheduletime

Measures the elapsed time between consecutive buffer arrivals at each sink pad.
Under normal operation the values are constant. Spikes indicate buffer drops,
bottlenecks, or clock issues.

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="scheduletime" \
    gst-launch-1.0 videotestsrc ! videoconvert ! fakesink sync=true
```

Log fields: `pad`, `time`.

### cpuusage

Reports the CPU load (%) for each logical core, sampled once per second.
Available on Linux only.

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="cpuusage" \
    gst-launch-1.0 videotestsrc ! fakesink
```

Log fields: `cpu<N>` (one per core).

### graphic

Generates a DOT file of the pipeline graph at the moment of the first buffer
push. Requires Graphviz to render.

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="graphic" \
    gst-launch-1.0 videotestsrc ! fakesink
# Render:
dot -Tpng <output_dir>/pipeline.dot -o pipeline.png
```

### bitrate

Reports the instantaneous bit rate (bits per second) on every source pad, updated once per second.

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="bitrate" \
    gst-launch-1.0 videotestsrc ! x264enc ! fakesink
```

Log fields: `pad`, `bps`.

### queuelevel

Logs the current fill level of every `queue` element (bytes, buffers, and time)
whenever a buffer passes through. Useful for detecting buffer starvation or
overflow.

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="queuelevel" \
    gst-launch-1.0 videotestsrc ! queue ! fakesink
```

Log fields: `queue`, `bytes`, `max_bytes`, `buffers`, `max_buffers`, `time`,
`max_time`.

### buffer

Logs detailed metadata for every buffer that passes through any pad: PTS, DTS,
duration, size, offset, flags, and reference count.

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="buffer" \
    gst-launch-1.0 videotestsrc ! fakesink
```

Log fields: `pad`, `pts`, `dts`, `duration`, `offset`, `offset_end`, `size`,
`flags`, `refcount`.

---

## Custom Tracers

The following tracers are specific to this fork and require NVIDIA DeepStream
(`GST_NVDS_ENABLE`). Without DeepStream they still compile and register, but
counting is always 0 (for `detectioncount`) or timing runs unconditionally (for
`rangetime`).

### detectioncount

Counts the number of confirmed `TrackerObject` detections (objects carrying
`NVDS_OBJ_TRACKER_META` user metadata) found in the `NvDsBatchMeta` of every
buffer that passes through a pad. The count is summed across all camera streams
in the batch.

**Activate:**

```bash
GST_DEBUG="GST_TRACER:7" GST_TRACERS="detectioncount" \
    python3 worker.py
```

**With element filter:**

```bash
GST_TRACERS="detectioncount(filter=nvtracker|alarmmanager)"
```

**With infer-only filter** (skip buffers where inference did not run):

```bash
GST_TRACERS="detectioncount(filter=nvtracker,infer-only=true)"
```

Log fields: `pad`, `pts`, `count`.

### rangetime

Measures the wall-clock elapsed time from when a named element begins receiving
a buffer to when another named element finishes pushing that same buffer
downstream. Element names are matched by substring, so `nvinfer` matches
`nvinfer0`. The result is logged under the label `from->to`.

This tracer reuses the `proctime` wire format and Octave plots.

**Activate:**

```bash
GST_DEBUG="GST_TRACER:7" \
    GST_TRACERS="rangetime(from=nvinfer,to=nvtracker)" \
    python3 worker.py
```

**With infer-only filter** (only time buffers on which inference actually ran):

```bash
GST_TRACERS="rangetime(from=nvinfer,to=nvtracker,infer-only=true)"
```

Log fields: `element` (label `from->to`), `time`.

**Parameters:**

| Parameter | Type | Required | Description |
|---|---|---|---|
| `from` | string | yes | Substring matched against the element name that starts the timer |
| `to` | string | yes | Substring matched against the element name that stops the timer |
| `infer-only` | bool | no | Only time buffers where `bInferDone=TRUE` (requires DeepStream) |

---

## Common Parameter: infer-only

Several tracers accept an `infer-only` boolean parameter. When set to `true`,
only buffers on which the inference plugin **actually ran** (`bInferDone=TRUE`
on the first `NvDsFrameMeta` of the batch) are measured or counted. Buffers
that were passed through without inference are silently skipped.

This is useful in pipelines where inference does not run on every frame (e.g.
due to interval-based or ROI-based inference), and you only want to profile
frames that went through the full inference + tracking path.

| Tracer | infer-only support |
|---|---|
| `proctime` | ✓ (requires DeepStream) |
| `detectioncount` | ✓ (requires DeepStream) |
| `rangetime` | ✓ (requires DeepStream) |

**Usage:**

```bash
GST_TRACERS="proctime(infer-only=true);detectioncount(infer-only=true);rangetime(from=nvinfer,to=nvtracker,infer-only=true)"
```

Accepted values: `true`, `1` (case-insensitive). Any other value leaves the
flag disabled (default).

---

## Visualising Results with gstshark-plot

`gstshark-plot` is a set of Octave scripts located in `scripts/graphics/` that
generate plots from the CTF trace files produced by GstShark.

**Requirements:** Octave, epstool, babeltrace (see [Dependencies](#dependencies)).

**Ensure CTF output is enabled** (unset the disable flag):

```bash
unset GST_SHARK_CTF_DISABLE
```

**Run your pipeline** to produce a trace directory, then plot:

```bash
cd scripts/graphics

# Display plots on screen
./gstshark-plot /path/to/gstshark_trace_dir/ -p

# Save all plots to a single PDF
./gstshark-plot /path/to/gstshark_trace_dir/ -s pdf

# Save each tracer as a separate PNG
./gstshark-plot /path/to/gstshark_trace_dir/ -s png
```

**Filter elements shown in the plot:**

```bash
# Single element
./gstshark-plot /path/to/trace_dir/ -p --filter nvinfer

# Multiple elements (extended regex)
./gstshark-plot /path/to/trace_dir/ -p --filter "nvinfer|nvtracker"
```

**Legend placement:**

```bash
./gstshark-plot /path/to/trace_dir/ -l inside    # default
./gstshark-plot /path/to/trace_dir/ -l outside
./gstshark-plot /path/to/trace_dir/ -l extern    # separate window/page
```

**Full help:**

```bash
./gstshark-plot --help
```

---

## Links

- [GstShark User Guide](https://developer.ridgerun.com/wiki/index.php/GstShark)
- [Getting Started](https://developer.ridgerun.com/wiki/index.php/GstShark_-_Getting_Started)
- [Generating Trace Files](https://developer.ridgerun.com/wiki/index.php/GstShark_-_Generating_trace_files)
- [Tracers](https://developer.ridgerun.com/wiki/index.php/GstShark_-_Tracers)
- [gstshark-plot](https://developer.ridgerun.com/wiki/index.php/GstShark_-_gstshark-plot)
- [GitHub](https://github.com/RidgeRun/gst-shark)


