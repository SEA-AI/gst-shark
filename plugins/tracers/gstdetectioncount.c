/* GstShark - A Front End for GstTracer
 * Copyright (C) 2024 SEA.AI
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
 * SECTION:gstdetectioncount
 * @short_description: Count DetectorObject and TrackerObject detections per buffer.
 *
 * Logs the number of DetectorObject and TrackerObject detections found in the
 * NvDsBatchMeta of every GstBuffer that traverses a pad. Both counts
 * accumulate across all camera streams (pad_index / source_id) present in
 * the batch so that the values reflect the total number of detected and
 * tracked targets visible to the system at the time the buffer was produced.
 *
 * Activate with:
 *   GST_TRACERS=detectioncount GST_DEBUG="GST_TRACER:7" gst-launch-1.0 ...
 *
 * Optional parameter:
 *   GST_TRACERS="detectioncount(infer-only=true)" — only log buffers on
 *   which at least one inference was performed (bInferDone is set).
 *   Requires GST_NVDS_ENABLE; when NvDs is absent the flag is ignored.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif
#include "gstdetectioncount.h"
#include "gstctf.h"
#ifdef GST_NVDS_ENABLE
#include <gstnvdsmeta.h>
#endif

GST_DEBUG_CATEGORY_STATIC (gst_detection_count_debug);
#define GST_CAT_DEFAULT gst_detection_count_debug

/* ---- Internal structure ------------------------------------------------- */

struct _GstDetectionCountTracer
{
  GstSharkTracer parent;

#ifdef GST_NVDS_ENABLE
  /* Cached NvDs meta types for user metadata.
   * Resolved once on first use via nvds_get_user_meta_type(). */
  NvDsMetaType detector_meta_type;
  gboolean     detector_meta_type_resolved;
  NvDsMetaType tracker_meta_type;
  gboolean     tracker_meta_type_resolved;

  /* When TRUE, only log buffers where bInferDone is set on their
   * NvDsBatchMeta first frame.  Activated via
   * GST_TRACERS="detectioncount(infer-only=true)". */
  gboolean infer_only;
#endif
};

/* ---- GType boilerplate -------------------------------------------------- */

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (gst_detection_count_debug, "detectioncount", 0, \
        "detectioncount tracer");

G_DEFINE_TYPE_WITH_CODE (GstDetectionCountTracer,
    gst_detection_count_tracer, GST_SHARK_TYPE_TRACER, _do_init);

static void gst_detection_count_tracer_constructed (GObject * object);

/* ---- GstTracerRecord ------------------------------------------------------ */

static GstTracerRecord *tr_detection_count;

/* CTF metadata event template (kept for binary tracing compatibility) */
static const gchar detection_count_metadata_event[] =
    "event {\n"
    "    name = detectioncount;\n"
    "    id = %d;\n"
    "    stream_id = %d;\n"
    "    fields := struct {\n"
    "        string pad;\n"
    "        integer { size = 64; align = 8; signed = 0; encoding = none;"
    " base = 10; } pts;\n"
    "        integer { size = 32; align = 8; signed = 0; encoding = none;"
    " base = 10; } detector_count;\n"
    "        integer { size = 32; align = 8; signed = 0; encoding = none;"
    " base = 10; } tracker_count;\n"
    "    };\n"
    "};\n\n";

/* ---- Helper: count TrackerObjects in a buffer ---------------------------- */

#ifdef GST_NVDS_ENABLE
static NvDsMetaType
get_detector_meta_type (GstDetectionCountTracer * self)
{
  if (!self->detector_meta_type_resolved) {
    self->detector_meta_type =
        nvds_get_user_meta_type ((gchar *) "NVDS_OBJ_DETECTOR_META");
    self->detector_meta_type_resolved = TRUE;
    GST_LOG_OBJECT (self, "Resolved NVDS_OBJ_DETECTOR_META type: %d",
        self->detector_meta_type);
  }
  return self->detector_meta_type;
}

static NvDsMetaType
get_tracker_meta_type (GstDetectionCountTracer * self)
{
  if (!self->tracker_meta_type_resolved) {
    self->tracker_meta_type =
        nvds_get_user_meta_type ((gchar *) "NVDS_OBJ_TRACKER_META");
    self->tracker_meta_type_resolved = TRUE;
    GST_LOG_OBJECT (self, "Resolved NVDS_OBJ_TRACKER_META type: %d",
        self->tracker_meta_type);
  }
  return self->tracker_meta_type;
}
#endif /* GST_NVDS_ENABLE */

/**
 * count_detection_objects:
 * @self: the tracer instance
 * @buffer: the GstBuffer to inspect
 * @detector_count: (out): number of NVDS_OBJ_DETECTOR_META entries found
 * @tracker_count: (out): number of NVDS_OBJ_TRACKER_META entries found
 *
 * Iterates the NvDsBatchMeta attached to @buffer and counts every
 * NvDsObjectMeta that carries a NvDsUserMeta of type NVDS_OBJ_DETECTOR_META
 * or NVDS_OBJ_TRACKER_META independently.
 */
static void
count_detection_objects (GstDetectionCountTracer * self, GstBuffer * buffer,
    guint * detector_count, guint * tracker_count)
{
  *detector_count = 0;
  *tracker_count  = 0;

#ifdef GST_NVDS_ENABLE
  NvDsBatchMeta      *batch_meta;
  NvDsFrameMetaList  *l_frame;
  NvDsObjectMetaList *l_obj;
  NvDsUserMetaList   *l_user;
  NvDsMetaType        det_type;
  NvDsMetaType        trk_type;

  batch_meta = gst_buffer_get_nvds_batch_meta (buffer);
  if (batch_meta == NULL) {
    GST_LOG_OBJECT (self, "No NvDsBatchMeta on buffer — skipping");
    return;
  }

  det_type = get_detector_meta_type (self);
  trk_type = get_tracker_meta_type (self);

  for (l_frame = batch_meta->frame_meta_list;
       l_frame != NULL;
       l_frame = l_frame->next) {

    NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) (l_frame->data);

    for (l_obj = frame_meta->obj_meta_list;
         l_obj != NULL;
         l_obj = l_obj->next) {

      NvDsObjectMeta *obj_meta = (NvDsObjectMeta *) (l_obj->data);

      for (l_user = obj_meta->obj_user_meta_list;
           l_user != NULL;
           l_user = l_user->next) {

        NvDsUserMeta *user_meta = (NvDsUserMeta *) (l_user->data);
        if (user_meta->base_meta.meta_type == det_type)
          (*detector_count)++;
        else if (user_meta->base_meta.meta_type == trk_type)
          (*tracker_count)++;
      }
    }
  }
#else
  GST_LOG_OBJECT (self, "NvDs not available — detectioncount always 0");
#endif /* GST_NVDS_ENABLE */
}

/* ---- Hook callbacks ------------------------------------------------------ */

static void
pad_push_buffer_pre (GstDetectionCountTracer * self, GstClockTime ts,
    GstPad * pad, GstBuffer * buffer)
{
  gchar      *pad_name;
  GstClockTime pts;
  guint       det_count = 0;
  guint       trk_count = 0;

#ifdef GST_NVDS_ENABLE
  /* When infer-only mode is active, skip buffers that did not go through
   * inference (bInferDone is not set on the first frame meta). */
  if (self->infer_only) {
    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buffer);
    if (!batch_meta || !batch_meta->frame_meta_list)
      return;
    NvDsFrameMeta *frame_meta =
        (NvDsFrameMeta *) batch_meta->frame_meta_list->data;
    if (!frame_meta->bInferDone)
      return;
  }
#endif /* GST_NVDS_ENABLE */

  pad_name = g_strdup_printf ("%s_%s", GST_DEBUG_PAD_NAME (pad));
  pts      = GST_BUFFER_PTS (buffer);

  count_detection_objects (self, buffer, &det_count, &trk_count);

  GST_TRACE_OBJECT (self, "pad=%s pts=%" GST_TIME_FORMAT
      " detector=%u tracker=%u",
      pad_name, GST_TIME_ARGS (pts), det_count, trk_count);

  gst_tracer_record_log (tr_detection_count, pad_name, pts,
      det_count, trk_count);
  do_print_detection_count_event (DETECTION_COUNT_EVENT_ID, pad_name, pts,
      det_count, trk_count);

  g_free (pad_name);
}

static void
pad_push_list_pre (GstDetectionCountTracer * self, GstClockTime ts,
    GstPad * pad, GstBufferList * list)
{
  guint idx;

  for (idx = 0; idx < gst_buffer_list_length (list); idx++) {
    GstBuffer *buffer = gst_buffer_list_get (list, idx);
    pad_push_buffer_pre (self, ts, pad, buffer);
  }
}

static void
pad_pull_range_post (GstDetectionCountTracer * self, GstClockTime ts,
    GstPad * pad, GstBuffer * buffer, GstFlowReturn res)
{
  if (GST_FLOW_OK == res && buffer != NULL)
    pad_push_buffer_pre (self, ts, pad, buffer);
}

/* ---- GObject / GstTracer boilerplate ------------------------------------- */

static void
gst_detection_count_tracer_class_init (GstDetectionCountTracerClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->constructed = gst_detection_count_tracer_constructed;

  tr_detection_count = gst_tracer_record_new ("detectioncount.class",
      "pad", GST_TYPE_STRUCTURE,
      gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_STRING,
          "description", G_TYPE_STRING,
          "The pad through which the buffer is passing",
          NULL),
      "pts", GST_TYPE_STRUCTURE,
      gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_UINT64,
          "description", G_TYPE_STRING, "Buffer presentation timestamp",
          "min", G_TYPE_UINT64, G_GUINT64_CONSTANT (0),
          "max", G_TYPE_UINT64, G_MAXUINT64,
          NULL),
      "detector-count", GST_TYPE_STRUCTURE,
      gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_UINT,
          "description", G_TYPE_STRING,
          "Number of DetectorObject entries in the buffer batch",
          "flags", GST_TYPE_TRACER_VALUE_FLAGS,
          GST_TRACER_VALUE_FLAGS_AGGREGATED,
          "min", G_TYPE_UINT, 0,
          "max", G_TYPE_UINT, G_MAXUINT,
          NULL),
      "tracker-count", GST_TYPE_STRUCTURE,
      gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_UINT,
          "description", G_TYPE_STRING,
          "Number of TrackerObject entries in the buffer batch",
          "flags", GST_TYPE_TRACER_VALUE_FLAGS,
          GST_TRACER_VALUE_FLAGS_AGGREGATED,
          "min", G_TYPE_UINT, 0,
          "max", G_TYPE_UINT, G_MAXUINT,
          NULL),
      NULL);
}

static void
gst_detection_count_tracer_init (GstDetectionCountTracer * self)
{
  GstSharkTracer *stracer = GST_SHARK_TRACER (self);

#ifdef GST_NVDS_ENABLE
  self->detector_meta_type          = 0;
  self->detector_meta_type_resolved = FALSE;
  self->tracker_meta_type           = 0;
  self->tracker_meta_type_resolved  = FALSE;
  self->infer_only                  = FALSE;
#endif

  gst_shark_tracer_register_hook (stracer, "pad-push-pre",
      G_CALLBACK (pad_push_buffer_pre));

  gst_shark_tracer_register_hook (stracer, "pad-push-list-pre",
      G_CALLBACK (pad_push_list_pre));

  gst_shark_tracer_register_hook (stracer, "pad-pull-range-post",
      G_CALLBACK (pad_pull_range_post));
}

static void
gst_detection_count_tracer_constructed (GObject * object)
{
  gchar *metadata_event;

  /* Chain up so the parent constructed runs first (calls gst_ctf_init). */
  G_OBJECT_CLASS (gst_detection_count_tracer_parent_class)->constructed (object);

  metadata_event = g_strdup_printf (detection_count_metadata_event,
      DETECTION_COUNT_EVENT_ID, 0);
  add_metadata_event_struct (metadata_event);
  g_free (metadata_event);

#ifdef GST_NVDS_ENABLE
  GstDetectionCountTracer *self = GST_DETECTION_COUNT_TRACER (object);
  GstSharkTracer *stracer = GST_SHARK_TRACER (object);
  /* Read the optional "infer-only" param.
   * Usage: GST_TRACERS="detectioncount(infer-only=true)"
   * When enabled, buffers that did not trigger inference are silently
   * skipped and will not appear in the detection-count log. */
  GList *param = gst_shark_tracer_get_param (stracer, "infer-only");
  if (param != NULL) {
    const gchar *val = (const gchar *) param->data;
    self->infer_only = (g_ascii_strcasecmp (val, "true") == 0 ||
        g_strcmp0 (val, "1") == 0);
    GST_INFO_OBJECT (self, "infer-only mode: %s",
        self->infer_only ? "enabled" : "disabled");
  }
#endif /* GST_NVDS_ENABLE */
}
