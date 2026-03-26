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
/**
 * SECTION:gstrangetime
 * @short_description: measure elapsed time across a pipeline segment
 *
 * A tracing module that measures the wall-clock time from when a named
 * element begins receiving a buffer to when another named element finishes
 * pushing that buffer downstream.  Both element names are matched by
 * substring, so "nvinfer" matches "nvinfer0".
 *
 * Usage:
 *   GST_TRACERS="rangetime(from=nvinfer,to=nvtracker)"
 *   GST_TRACERS="rangetime(from=nvinfer,to=nvtracker,infer-only=true)"
 *
 * With infer-only=true only buffers on which the inference plugin actually
 * ran (bInferDone=TRUE in NvDsBatchMeta) are timed; pass-through frames are
 * silently skipped.
 *
 * The result is logged under the label "nvinfer->nvtracker" and is
 * compatible with the standard proctime parser and Octave plots.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif
#include "gstrangetime.h"
#include "gstctf.h"
#ifdef GST_NVDS_ENABLE
#include <gstnvdsmeta.h>
#endif

GST_DEBUG_CATEGORY_STATIC (gst_range_time_debug);
#define GST_CAT_DEFAULT gst_range_time_debug

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (gst_range_time_debug, "rangetime", 0, "rangetime tracer");

G_DEFINE_TYPE_WITH_CODE (GstRangeTimeTracer, gst_range_time_tracer,
    GST_SHARK_TYPE_TRACER, _do_init);

static void gst_range_time_tracer_constructed (GObject * object);
static void gst_range_time_tracer_finalize (GObject * object);

static GstTracerRecord *tr_range_time;

/* CTF metadata event — identical wire format to proctime, different name/id. */
static const gchar rangetime_metadata_event[] = "event {\n\
    name = rangetime;\n\
    id = %d;\n\
    stream_id = %d;\n\
    fields := struct {\n\
        string element; \n\
        integer { size = 64; align = 8; signed = 0; encoding = none; base = 10; } _time;\n\
    };\n\
};\n\
\n";

/* ---------------------------------------------------------------------------
 * Hook: pad-push-pre
 *
 * Called for every src-pad push in the pipeline.  We check:
 *   1. If the receiving element (peer_pad's parent) matches "from" → record
 *      the start timestamp (the buffer is entering the from-element).
 *   2. If the pushing element (pad's parent) matches "to" → compute elapsed
 *      since the last start and log it (the buffer is leaving the to-element).
 *
 * Thread notes: start_ts / started are protected by tracer->lock.  In a
 * typical linear pipeline the lock is uncontested; it becomes important only
 * when processing branches share the same tracer instance.
 * --------------------------------------------------------------------------- */
static void
do_push_buffer_pre (GstTracer * self, guint64 ts, GstPad * pad,
    GstBuffer * buffer)
{
  GstRangeTimeTracer *tracer = GST_RANGE_TIME_TRACER (self);
  GstPad *peer_pad;
  GstObject *pusher_parent;
  GstObject *receiver_parent;
  const gchar *pusher_name;
  const gchar *receiver_name;
  GstClockTime elapsed;
  gchar *time_string;

  peer_pad = gst_pad_get_peer (pad);
  if (!peer_pad)
    return;

  pusher_parent   = GST_OBJECT_PARENT (pad);
  receiver_parent = GST_OBJECT_PARENT (peer_pad);
  gst_object_unref (peer_pad);

  if (!pusher_parent || !receiver_parent)
    return;

  pusher_name   = GST_OBJECT_NAME (pusher_parent);
  receiver_name = GST_OBJECT_NAME (receiver_parent);

  g_mutex_lock (&tracer->lock);

  /* --- Start condition ---
   * The "from" element is about to receive a buffer: begin timing.
   * When infer-only is active, only start the timer if bInferDone is set
   * on the buffer's NvDsBatchMeta — i.e. inference actually ran. */
  if (strstr (receiver_name, tracer->from) != NULL) {
    gboolean do_start = TRUE;
#ifdef GST_NVDS_ENABLE
    if (tracer->infer_only) {
      NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buffer);
      if (batch_meta && batch_meta->frame_meta_list) {
        NvDsFrameMeta *frame_meta =
            (NvDsFrameMeta *) batch_meta->frame_meta_list->data;
        do_start = (gboolean) frame_meta->bInferDone;
      } else {
        do_start = FALSE;
      }
    }
#endif /* GST_NVDS_ENABLE */
    if (do_start) {
      tracer->start_ts = ts;
      tracer->started  = TRUE;
      GST_LOG_OBJECT (self, "rangetime start on element '%s' (matched from='%s')",
          receiver_name, tracer->from);
    } else {
      /* infer-only: inference did not run on this buffer — reset so that
       * a stale start_ts from a previous inference buffer is not reused. */
      tracer->started = FALSE;
      GST_LOG_OBJECT (self,
          "rangetime: skipping buffer on '%s' (infer-only, bInferDone=FALSE)",
          receiver_name);
    }
  }

  /* --- Stop condition ---
   * The "to" element is pushing its result downstream: stop timing. */
  if (tracer->started && strstr (pusher_name, tracer->to) != NULL) {
    if (ts > tracer->start_ts) {
      elapsed = ts - tracer->start_ts;
    } else {
      GST_WARNING_OBJECT (self, "rangetime: timestamp out of order, skipping sample");
      tracer->started = FALSE;
      g_mutex_unlock (&tracer->lock);
      return;
    }

    tracer->started = FALSE;
    g_mutex_unlock (&tracer->lock);

    time_string = g_strdup_printf ("%" GST_TIME_FORMAT, GST_TIME_ARGS (elapsed));
    gst_tracer_record_log (tr_range_time, tracer->label, time_string);
    g_free (time_string);

    /* Reuse proctime's wire format: element name string + uint64 nanoseconds. */
    do_print_proctime_event (RANGETIME_EVENT_ID, tracer->label, elapsed);

    GST_LOG_OBJECT (self,
        "rangetime '%s': %" GST_TIME_FORMAT " (to='%s' on element '%s')",
        tracer->label, GST_TIME_ARGS (elapsed), tracer->to, pusher_name);
    return;
  }

  g_mutex_unlock (&tracer->lock);
}

/* --------------------------------------------------------------------------- */

static void
gst_range_time_tracer_class_init (GstRangeTimeTracerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->constructed = gst_range_time_tracer_constructed;
  gobject_class->finalize    = gst_range_time_tracer_finalize;

  tr_range_time = gst_tracer_record_new ("rangetime.class",
      "element", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "type", G_TYPE_GTYPE, G_TYPE_STRING,
          "related-to", GST_TYPE_TRACER_VALUE_SCOPE,
          GST_TRACER_VALUE_SCOPE_ELEMENT, NULL),
      "time", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "type", G_TYPE_GTYPE, G_TYPE_STRING,
          "related-to", GST_TYPE_TRACER_VALUE_SCOPE,
          GST_TRACER_VALUE_SCOPE_PROCESS, NULL),
      NULL);
}

static void
gst_range_time_tracer_init (GstRangeTimeTracer * self)
{
  GstTracer *tracer = GST_TRACER (self);

  self->from       = NULL;
  self->to         = NULL;
  self->label      = NULL;
  self->start_ts   = GST_CLOCK_TIME_NONE;
  self->started    = FALSE;
#ifdef GST_NVDS_ENABLE
  self->infer_only = FALSE;
#endif
  g_mutex_init (&self->lock);

  gst_tracing_register_hook (tracer, "pad-push-pre",
      G_CALLBACK (do_push_buffer_pre));
}

static void
gst_range_time_tracer_constructed (GObject * object)
{
  GstRangeTimeTracer *self = GST_RANGE_TIME_TRACER (object);
  GstSharkTracer *shark_tracer = GST_SHARK_TRACER (object);
  GList *param_from    = NULL;
  GList *param_to      = NULL;
  gchar *metadata_event = NULL;

  /* Chain up so the parent runs gst_ctf_init(). */
  G_OBJECT_CLASS (gst_range_time_tracer_parent_class)->constructed (object);

  /* Read "from" param — required. */
  param_from = gst_shark_tracer_get_param (shark_tracer, "from");
  if (param_from != NULL) {
    self->from = g_strdup ((const gchar *) param_from->data);
  } else {
    GST_WARNING_OBJECT (self,
        "rangetime: 'from' param not set; no measurements will be emitted. "
        "Usage: GST_TRACERS=\"rangetime(from=elemA,to=elemB)\"");
    self->from = g_strdup ("");
  }

  /* Read "to" param — required. */
  param_to = gst_shark_tracer_get_param (shark_tracer, "to");
  if (param_to != NULL) {
    self->to = g_strdup ((const gchar *) param_to->data);
  } else {
    GST_WARNING_OBJECT (self,
        "rangetime: 'to' param not set; no measurements will be emitted. "
        "Usage: GST_TRACERS=\"rangetime(from=elemA,to=elemB)\"");
    self->to = g_strdup ("");
  }

  self->label = g_strdup_printf ("%s->%s", self->from, self->to);

#ifdef GST_NVDS_ENABLE
  /* Read optional "infer-only" param. */
  GList *param_infer = gst_shark_tracer_get_param (shark_tracer, "infer-only");
  if (param_infer != NULL) {
    const gchar *val = (const gchar *) param_infer->data;
    self->infer_only = (g_ascii_strcasecmp (val, "true") == 0 ||
        g_strcmp0 (val, "1") == 0);
  }
#endif /* GST_NVDS_ENABLE */

  /* Register the CTF metadata event for this tracer. */
  metadata_event =
      g_strdup_printf (rangetime_metadata_event, RANGETIME_EVENT_ID, 0);
  add_metadata_event_struct (metadata_event);
  g_free (metadata_event);
}

static void
gst_range_time_tracer_finalize (GObject * object)
{
  GstRangeTimeTracer *self = GST_RANGE_TIME_TRACER (object);

  g_mutex_clear (&self->lock);
  g_free (self->from);
  g_free (self->to);
  g_free (self->label);

  G_OBJECT_CLASS (gst_range_time_tracer_parent_class)->finalize (object);
}
