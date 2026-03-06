/* GstShark - A Front End for GstTracer
 *
 * This file is part of GstShark.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef __GST_RANGE_TIME_TRACER_H__
#define __GST_RANGE_TIME_TRACER_H__

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gstsharktracer.h"

G_BEGIN_DECLS

#define GST_TYPE_RANGE_TIME_TRACER \
  (gst_range_time_tracer_get_type ())
#define GST_RANGE_TIME_TRACER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_RANGE_TIME_TRACER, GstRangeTimeTracer))
#define GST_RANGE_TIME_TRACER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), GST_TYPE_RANGE_TIME_TRACER, GstRangeTimeTracerClass))
#define GST_IS_RANGE_TIME_TRACER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_RANGE_TIME_TRACER))
#define GST_IS_RANGE_TIME_TRACER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), GST_TYPE_RANGE_TIME_TRACER))
#define GST_RANGE_TIME_TRACER_CAST(obj) ((GstRangeTimeTracer *)(obj))

typedef struct _GstRangeTimeTracer GstRangeTimeTracer;
typedef struct _GstRangeTimeTracerClass GstRangeTimeTracerClass;

/**
 * GstRangeTimeTracer:
 *
 * Opaque #GstRangeTimeTracer data structure.
 *
 * Measures the elapsed time from the moment a named element (from=)
 * begins receiving a buffer to the moment another element (to=) finishes
 * pushing that buffer downstream.  Both element names are matched as
 * substrings of the GStreamer object name, so "nvinfer" matches "nvinfer0".
 *
 * Usage:  GST_TRACERS="rangetime(from=nvinfer,to=nvtracker)"
 */
struct _GstRangeTimeTracer
{
  GstSharkTracer parent;
  /*< private > */
  gchar *from;          /* substring to match against the "from" element name */
  gchar *to;            /* substring to match against the "to" element name   */
  gchar *label;         /* "from->to" — used as the series name in logs       */
  GstClockTime start_ts;
  gboolean started;
#ifdef GST_NVDS_ENABLE
  /* When TRUE, only time buffers that have bInferDone set on their
   * NvDsBatchMeta (i.e. the inference plugin ran on this buffer).
   * Activated via GST_TRACERS="rangetime(from=A,to=B,infer-only=true)". */
  gboolean infer_only;
#endif
  GMutex lock;
};

struct _GstRangeTimeTracerClass
{
  GstSharkTracerClass parent_class;
};

GType gst_range_time_tracer_get_type (void);

G_END_DECLS

#endif /* __GST_RANGE_TIME_TRACER_H__ */
