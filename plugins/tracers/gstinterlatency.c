/* GStreamer
 * Copyright (C) 2013 Stefan Sauer <ensonic@users.sf.net>
 * Copyright (C) 2016 RidgeRun Engineering <carlos.rodriguez@ridgerun.com>
 *
 * gstinterlatency.c: tracing module that logs processing latencies
 * stats between source and intermediate elements
 *
 * This file is part of GstShark.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
/**
 * SECTION:gstinterlatency
 * @short_description: log processing latencies stats
 *
 * A tracing module that determines latencies between src and intermediate
 * elements. Sources tag each buffer with a GstSharkLatencyMeta holding the
 * source pad and push time, and every pad push logs one sample per meta on
 * the buffer.
 *
 * Elements that create new buffers without copying untagged metas (e.g.
 * aggregators such as compositor) drop the meta. For those buffers the tracer
 * falls back to custom probe events: each pad keeps one pending probe per
 * source pad and logs them on the next buffer that carries no meta.
 *
 * With DeepStream, nvstreammux builds new batch buffers. The tracer records
 * the metas entering each nvstreammux sink pad by (pad index, PTS) and matches
 * them to the NvDsFrameMeta of downstream batches, so nvstreamdemux branches
 * only report their own source.
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>

#include "gstinterlatency.h"
#include "gstctf.h"
#ifdef GST_NVDS_ENABLE
#include <gstnvdsmeta.h>
#endif

GST_DEBUG_CATEGORY_STATIC (gst_interlatency_debug);
#define GST_CAT_DEFAULT gst_interlatency_debug

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (gst_interlatency_debug, "interlatency", 0, "interlatency tracer");
#define gst_interlatency_tracer_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstInterLatencyTracer, gst_interlatency_tracer,
    GST_SHARK_TYPE_TRACER, _do_init);

static GQuark latency_probe_id;
static GQuark latency_probe_pad;
static GQuark latency_probe_ts;
static GQuark latency_pull_ts;

/* Protects the pending probe tables stored as pad qdata and the mux rings */
static GMutex probes_lock;

#ifdef GST_STABLE_RELEASE
static GstTracerRecord *tr_interlatency;
#endif

static const gchar interlatency_metadata_event[] = "event {\n\
    name = interlatency;\n\
    id = %d;\n\
    stream_id = %d;\n\
    fields := struct {\n\
        string from_pad;\n\
        string to_pad;\n\
        integer { size = 64; align = 8; signed = 0; encoding = none; base = 10; } _time;\n\
    };\n\
};\n\
\n";

static void gst_interlatency_tracer_dispose (GObject * object);
static void gst_interlatency_tracer_constructed (GObject *object);

/* latency meta */

typedef struct
{
  GstMeta meta;
  GstPad *src_pad;
  guint64 src_ts;
} GstSharkLatencyMeta;

static GType
shark_latency_meta_api_get_type (void)
{
  static gsize type = 0;
  /* no tags, so elements copy the meta by default */
  static const gchar *tags[] = { NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstSharkLatencyMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return (GType) type;
}

#define SHARK_LATENCY_META_API_TYPE (shark_latency_meta_api_get_type ())

static const GstMetaInfo *shark_latency_meta_get_info (void);

static gboolean
shark_latency_meta_init (GstMeta * meta, gpointer params, GstBuffer * buffer)
{
  GstSharkLatencyMeta *lmeta = (GstSharkLatencyMeta *) meta;

  lmeta->src_pad = NULL;
  lmeta->src_ts = 0;
  return TRUE;
}

static void
shark_latency_meta_free (GstMeta * meta, GstBuffer * buffer)
{
  GstSharkLatencyMeta *lmeta = (GstSharkLatencyMeta *) meta;

  gst_clear_object (&lmeta->src_pad);
}

/* Add a meta for src_pad, or keep the earliest ts if the buffer already has
 * one from the same source (e.g. several RTP packets merged into a frame) */
static void
add_latency_meta (GstBuffer * buffer, GstPad * src_pad, guint64 src_ts)
{
  gpointer state = NULL;
  GstMeta *meta;
  GstSharkLatencyMeta *lmeta;

  while ((meta = gst_buffer_iterate_meta_filtered (buffer, &state,
              SHARK_LATENCY_META_API_TYPE))) {
    lmeta = (GstSharkLatencyMeta *) meta;
    if (lmeta->src_pad == src_pad) {
      lmeta->src_ts = MIN (lmeta->src_ts, src_ts);
      return;
    }
  }

  lmeta = (GstSharkLatencyMeta *) gst_buffer_add_meta (buffer,
      shark_latency_meta_get_info (), NULL);
  if (lmeta) {
    lmeta->src_pad = gst_object_ref (src_pad);
    lmeta->src_ts = src_ts;
  }
}

static gboolean
shark_latency_meta_transform (GstBuffer * dest, GstMeta * meta,
    GstBuffer * buffer, GQuark type, gpointer data)
{
  GstSharkLatencyMeta *lmeta = (GstSharkLatencyMeta *) meta;

  /* the origin does not depend on the buffer contents, so keep it for any
   * transform (copy, scale, convert, ...) */
  add_latency_meta (dest, lmeta->src_pad, lmeta->src_ts);
  return TRUE;
}

static const GstMetaInfo *
shark_latency_meta_get_info (void)
{
  static const GstMetaInfo *info = NULL;

  if (g_once_init_enter ((GstMetaInfo **) & info)) {
    const GstMetaInfo *meta = gst_meta_register (SHARK_LATENCY_META_API_TYPE,
        "GstSharkLatencyMeta", sizeof (GstSharkLatencyMeta),
        shark_latency_meta_init, shark_latency_meta_free,
        shark_latency_meta_transform);
    g_once_init_leave ((GstMetaInfo **) & info, (GstMetaInfo *) meta);
  }
  return info;
}

/* data helpers */

/*
 * Get the element/bin owning the pad.
 *
 * in: a normal pad
 * out: the element
 *
 * in: a proxy pad
 * out: the element that contains the peer of the proxy
 *
 * in: a ghost pad
 * out: the bin owning the ghostpad
 */
/* TODO(ensonic): gst_pad_get_parent_element() would not work here, should we
 * add this as new api, e.g. gst_pad_find_parent_element();
 */
static GstElement *
get_real_pad_parent (GstPad * pad)
{
  GstObject *parent = NULL;

  if (!pad)
    return NULL;

  parent = GST_OBJECT_PARENT (pad);

  /* if parent of pad is a ghost-pad, then pad is a proxy_pad */
  if (parent && GST_IS_GHOST_PAD (parent)) {
    pad = GST_PAD_CAST (parent);
    parent = GST_OBJECT_PARENT (pad);
  }

  return GST_ELEMENT_CAST (parent);
}

/* hooks */

static void
log_latency (GstInterLatencyTracer * interlatency_tracer,
    GstPad * src_pad, guint64 src_ts, GstPad * sink_pad, guint64 sink_ts)
{
  gchar *src = NULL, *sink = NULL;
  guint64 time;
  GString *time_string = NULL;

  src = g_strdup_printf ("%s_%s", GST_DEBUG_PAD_NAME (src_pad));
  sink = g_strdup_printf ("%s_%s", GST_DEBUG_PAD_NAME (sink_pad));

  time = GST_CLOCK_DIFF (src_ts, sink_ts);

  time_string = g_string_new ("");
  g_string_printf (time_string, "%" GST_TIME_FORMAT, GST_TIME_ARGS (time));

#ifdef GST_STABLE_RELEASE
  gst_tracer_record_log (tr_interlatency, src, sink, time_string->str);
#else
  /* TODO(ensonic): report format is still unstable */
  gst_tracer_log_trace (gst_structure_new ("interlatency",
          "from_pad", G_TYPE_STRING, src,
          "to_pad", G_TYPE_STRING, sink,
          "time", G_TYPE_STRING, time_string->str, NULL));
#endif
  do_print_interlatency_event (INTERLATENCY_EVENT_ID, src, sink, time);

  g_string_free (time_string, TRUE);
  g_free (src);
  g_free (sink);
}

static void
log_probe_latency (GstInterLatencyTracer * interlatency_tracer,
    const GstStructure * data, GstPad * sink_pad, guint64 sink_ts)
{
  GstPad *src_pad;
  guint64 src_ts;

  src_pad = g_value_get_object (gst_structure_id_get_value (data,
          latency_probe_pad));
  gst_structure_id_get (data, latency_probe_ts, G_TYPE_UINT64, &src_ts, NULL);

  log_latency (interlatency_tracer, src_pad, src_ts, sink_pad, sink_ts);
}

static void
tag_buffer (GstBuffer * buffer, GstPad * pad, guint64 ts)
{
  if (!gst_buffer_is_writable (buffer)) {
    GST_DEBUG_OBJECT (pad, "buffer not writable, not tagged");
    return;
  }
  add_latency_meta (buffer, pad, ts);
}

static void
tag_buffers (GstPad * pad, GstBuffer * buffer, GstBufferList * list,
    guint64 ts)
{
  guint i;

  if (buffer)
    tag_buffer (buffer, pad, ts);
  for (i = 0; list && i < gst_buffer_list_length (list); i++)
    tag_buffer (gst_buffer_list_get (list, i), pad, ts);
}

#ifdef GST_NVDS_ENABLE
/* Frames that entered each nvstreammux sink pad, matched later against the
 * NvDsFrameMeta (pad_index, buf_pts) of the batches */
#define MUX_HISTORY 256

typedef struct
{
  GstClockTime pts;
  GstPad *src_pad;
  guint64 src_ts;
} MuxEntry;

typedef struct
{
  MuxEntry entries[MUX_HISTORY];
  guint next;
} MuxRing;

/* guint pad_index -> MuxRing, protected by probes_lock */
static GHashTable *mux_rings;

static void
mux_ring_free (MuxRing * ring)
{
  guint i;

  for (i = 0; i < MUX_HISTORY; i++)
    gst_clear_object (&ring->entries[i].src_pad);
  g_free (ring);
}

static gboolean
is_nvstreammux (GstElement * element)
{
  GstElementFactory *factory;

  if (!element || GST_IS_BIN (element))
    return FALSE;

  factory = gst_element_get_factory (element);
  return factory
      && g_str_equal (GST_OBJECT_NAME (factory), "nvstreammux");
}

static void
record_mux_buffer (MuxRing * ring, GstBuffer * buffer)
{
  gpointer state = NULL;
  GstMeta *meta;

  while ((meta = gst_buffer_iterate_meta_filtered (buffer, &state,
              SHARK_LATENCY_META_API_TYPE))) {
    GstSharkLatencyMeta *lmeta = (GstSharkLatencyMeta *) meta;
    MuxEntry *entry = &ring->entries[ring->next];

    gst_object_replace ((GstObject **) & entry->src_pad,
        (GstObject *) lmeta->src_pad);
    entry->pts = GST_BUFFER_PTS (buffer);
    entry->src_ts = lmeta->src_ts;
    ring->next = (ring->next + 1) % MUX_HISTORY;
  }
}

static void
record_mux_input (GstElement * mux, GstPad * mux_pad, GstBuffer * buffer,
    GstBufferList * list)
{
  MuxRing *ring;
  guint pad_index, i;

  if (!is_nvstreammux (mux)
      || sscanf (GST_PAD_NAME (mux_pad), "sink_%u", &pad_index) != 1)
    return;

  g_mutex_lock (&probes_lock);
  if (!mux_rings)
    mux_rings = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
        (GDestroyNotify) mux_ring_free);
  ring = g_hash_table_lookup (mux_rings, GUINT_TO_POINTER (pad_index));
  if (!ring) {
    ring = g_new0 (MuxRing, 1);
    g_hash_table_insert (mux_rings, GUINT_TO_POINTER (pad_index), ring);
  }

  if (buffer)
    record_mux_buffer (ring, buffer);
  for (i = 0; list && i < gst_buffer_list_length (list); i++)
    record_mux_buffer (ring, gst_buffer_list_get (list, i));
  g_mutex_unlock (&probes_lock);
}

/* Log one sample per batched frame found in the mux rings. Returns TRUE if
 * the buffer is a batch and the nvstreammux bridge is active, so the probe
 * events (which do not follow batching) are not used for it */
static gboolean
log_batch_latency (GstInterLatencyTracer * interlatency_tracer,
    GstBuffer * buffer, GstPad * pad, guint64 ts)
{
  NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta (buffer);
  NvDsFrameMetaList *l_frame;
  GArray *matches;
  gboolean active;
  guint i;

  if (!batch_meta)
    return FALSE;

  matches = g_array_new (FALSE, FALSE, sizeof (MuxEntry));

  g_mutex_lock (&probes_lock);
  active = mux_rings != NULL;
  for (l_frame = batch_meta->frame_meta_list; active && l_frame;
      l_frame = l_frame->next) {
    NvDsFrameMeta *frame_meta = (NvDsFrameMeta *) l_frame->data;
    MuxRing *ring = g_hash_table_lookup (mux_rings,
        GUINT_TO_POINTER (frame_meta->pad_index));

    for (i = 0; ring && i < MUX_HISTORY; i++) {
      MuxEntry entry = ring->entries[i];

      if (entry.src_pad && entry.pts == frame_meta->buf_pts) {
        gst_object_ref (entry.src_pad);
        g_array_append_val (matches, entry);
      }
    }
  }
  g_mutex_unlock (&probes_lock);

  for (i = 0; i < matches->len; i++) {
    MuxEntry *entry = &g_array_index (matches, MuxEntry, i);

    log_latency (interlatency_tracer, entry->src_pad, entry->src_ts, pad, ts);
    gst_object_unref (entry->src_pad);
  }
  g_array_free (matches, TRUE);

  return active;
}
#endif /* GST_NVDS_ENABLE */

/* Log one sample per latency meta on the buffer. Returns TRUE if the buffer
 * carried latency information */
static gboolean
log_buffer_latency (GstInterLatencyTracer * interlatency_tracer,
    GstBuffer * buffer, GstPad * pad, guint64 ts)
{
  gpointer state = NULL;
  GstMeta *meta;
  gboolean found = FALSE;

  while ((meta = gst_buffer_iterate_meta_filtered (buffer, &state,
              SHARK_LATENCY_META_API_TYPE))) {
    GstSharkLatencyMeta *lmeta = (GstSharkLatencyMeta *) meta;

    log_latency (interlatency_tracer, lmeta->src_pad, lmeta->src_ts, pad, ts);
    found = TRUE;
  }

#ifdef GST_NVDS_ENABLE
  if (!found)
    found = log_batch_latency (interlatency_tracer, buffer, pad, ts);
#endif

  return found;
}

static void
send_latency_probe (GstElement * parent, GstPad * pad, guint64 ts)
{
  g_return_if_fail (parent);
  g_return_if_fail (pad);

  if (!GST_IS_BIN (parent)) {
    GstEvent *latency_probe = gst_event_new_custom (GST_EVENT_CUSTOM_DOWNSTREAM,
        gst_structure_new_id (latency_probe_id,
            latency_probe_pad, GST_TYPE_PAD, pad,
            latency_probe_ts, G_TYPE_UINT64, ts,
            NULL));
    gst_pad_push_event (pad, latency_probe);
  }
}

/* Store the probe on the pad, replacing any pending probe from the same
 * source pad but keeping those from other sources */
static void
store_latency_probe (GstPad * pad, GstEvent * ev)
{
  GstPad *src_pad;
  GHashTable *probes;

  src_pad = g_value_get_object (gst_structure_id_get_value
      (gst_event_get_structure (ev), latency_probe_pad));

  g_mutex_lock (&probes_lock);
  probes = g_object_get_qdata ((GObject *) pad, latency_probe_id);
  if (!probes) {
    probes = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
        (GDestroyNotify) gst_event_unref);
    g_object_set_qdata_full ((GObject *) pad, latency_probe_id, probes,
        (GDestroyNotify) g_hash_table_unref);
  }
  /* the event holds a ref to src_pad, so the key lives as long as the value */
  g_hash_table_replace (probes, src_pad, gst_event_ref (ev));
  g_mutex_unlock (&probes_lock);
}

/* Log the latency of the buffer (or list) pushed through pad. Buffer metas
 * take precedence; pending probe events are only logged for buffers without
 * latency information */
static void
calculate_latency (GstInterLatencyTracer * interlatency_tracer,
    GstElement * parent, GstPad * pad, GstBuffer * buffer,
    GstBufferList * list, guint64 ts)
{
  GHashTable *probes;
  GHashTableIter iter;
  gpointer ev;
  gboolean found = FALSE;
  guint i;

  g_return_if_fail (interlatency_tracer);
  g_return_if_fail (parent);
  g_return_if_fail (pad);

  if (GST_IS_BIN (parent))
    return;

  if (buffer)
    found = log_buffer_latency (interlatency_tracer, buffer, pad, ts);
  for (i = 0; list && i < gst_buffer_list_length (list); i++)
    found |= log_buffer_latency (interlatency_tracer,
        gst_buffer_list_get (list, i), pad, ts);

  /* consume the pending probes so each is logged only once */
  g_mutex_lock (&probes_lock);
  probes = g_object_steal_qdata ((GObject *) pad, latency_probe_id);
  g_mutex_unlock (&probes_lock);

  if (!probes)
    return;

  if (!found) {
    g_hash_table_iter_init (&iter, probes);
    while (g_hash_table_iter_next (&iter, NULL, &ev))
      log_probe_latency (interlatency_tracer, gst_event_get_structure (ev),
          pad, ts);
  }

  g_hash_table_unref (probes);
}

static void
handle_push (GstInterLatencyTracer * interlatency_tracer, guint64 ts,
    GstPad * pad, GstBuffer * buffer, GstBufferList * list)
{
  GstElement *parent = get_real_pad_parent (pad);
  GstPad *peer_pad = GST_PAD_PEER (pad);
  GstElement *peer_parent = get_real_pad_parent (peer_pad);

  /* Not having a peer pad means that the pad is not linked, which
     results in a segfault */
  if (!peer_pad) {
    return;
  }

  if (parent && GST_OBJECT_FLAG_IS_SET (parent, GST_ELEMENT_FLAG_SOURCE)) {
    if (!GST_IS_BIN (parent))
      tag_buffers (pad, buffer, list, ts);
    send_latency_probe (parent, pad, ts);
  } else if (parent)
    calculate_latency (interlatency_tracer, parent, pad, buffer, list, ts);

#ifdef GST_NVDS_ENABLE
  record_mux_input (peer_parent, peer_pad, buffer, list);
#endif

  if (peer_parent
      && GST_OBJECT_FLAG_IS_SET (peer_parent, GST_ELEMENT_FLAG_SINK))
    calculate_latency (interlatency_tracer, peer_parent, peer_pad, buffer,
        list, ts);
}

static void
do_push_buffer_pre (GstTracer * self, guint64 ts, GstPad * pad,
    GstBuffer * buffer)
{
  handle_push (GST_INTERLATENCY_TRACER_CAST (self), ts, pad, buffer, NULL);
}

static void
do_push_list_pre (GstTracer * self, guint64 ts, GstPad * pad,
    GstBufferList * list)
{
  handle_push (GST_INTERLATENCY_TRACER_CAST (self), ts, pad, NULL, list);
}

static void
do_pull_range_pre (GstTracer * self, guint64 ts, GstPad * pad)
{
  GstInterLatencyTracer *interlatency_tracer;

  GstPad *peer_pad = GST_PAD_PEER (pad);
  GstElement *parent_peer = get_real_pad_parent (peer_pad);

  interlatency_tracer = GST_INTERLATENCY_TRACER_CAST (self);

  if (parent_peer
      && GST_OBJECT_FLAG_IS_SET (parent_peer, GST_ELEMENT_FLAG_SOURCE)) {
    /* tag the pulled buffer with the request time in -post */
    guint64 *pull_ts = g_new (guint64, 1);

    *pull_ts = ts;
    g_object_set_qdata_full ((GObject *) pad, latency_pull_ts, pull_ts,
        g_free);
    send_latency_probe (parent_peer, peer_pad, ts);
  } else if (parent_peer)
    calculate_latency (interlatency_tracer, parent_peer, peer_pad, NULL, NULL,
        ts);
}

static void
do_pull_range_post (GstTracer * self, guint64 ts, GstPad * pad,
    GstBuffer * buffer, GstFlowReturn res)
{
  GstInterLatencyTracer *interlatency_tracer;
  GstElement *parent = get_real_pad_parent (pad);
  GstPad *peer_pad = GST_PAD_PEER (pad);
  GstElement *parent_peer = get_real_pad_parent (peer_pad);
  guint64 *pull_ts = g_object_get_qdata ((GObject *) pad, latency_pull_ts);

  interlatency_tracer = GST_INTERLATENCY_TRACER_CAST (self);

  if (res != GST_FLOW_OK)
    buffer = NULL;

  if (buffer && pull_ts && parent_peer && !GST_IS_BIN (parent_peer)
      && GST_OBJECT_FLAG_IS_SET (parent_peer, GST_ELEMENT_FLAG_SOURCE))
    tag_buffer (buffer, peer_pad, *pull_ts);

  if (parent && GST_OBJECT_FLAG_IS_SET (parent, GST_ELEMENT_FLAG_SINK))
    calculate_latency (interlatency_tracer, parent, pad, buffer, NULL, ts);
}

static void
do_push_event_pre (GstTracer * self, guint64 ts, GstPad * pad, GstEvent * ev)
{
  GstPad *peer_pad = GST_PAD_PEER (pad);
  GstElement *parent = get_real_pad_parent (pad);
  GstElement *parent_peer = get_real_pad_parent (peer_pad);

  if (parent_peer && (!GST_IS_BIN (parent_peer)) &&
      !GST_OBJECT_FLAG_IS_SET (parent, GST_ELEMENT_FLAG_SOURCE)) {
    if (GST_EVENT_TYPE (ev) == GST_EVENT_CUSTOM_DOWNSTREAM) {
      const GstStructure *data = gst_event_get_structure (ev);

      if (gst_structure_get_name_id (data) == latency_probe_id) {
        /* store event and calculate latency when the buffer that follows
         * has been processed */
        store_latency_probe (pad, ev);
        if (GST_OBJECT_FLAG_IS_SET (parent_peer, GST_ELEMENT_FLAG_SINK))
          store_latency_probe (peer_pad, ev);
      }
    }
  }
}

/* tracer class */

static void
gst_interlatency_tracer_class_init (GstInterLatencyTracerClass * klass)
{
  GObjectClass *oclass;

  oclass = G_OBJECT_CLASS (klass);

  latency_probe_id = g_quark_from_static_string ("latency_probe.id");
  latency_probe_pad = g_quark_from_static_string ("latency_probe.pad");
  latency_probe_ts = g_quark_from_static_string ("latency_probe.ts");
  latency_pull_ts = g_quark_from_static_string ("latency_pull.ts");

  /* announce trace formats */
  /* *INDENT-OFF* */
#ifdef GST_STABLE_RELEASE
  tr_interlatency = gst_tracer_record_new ("interlatency.class",
      "from_pad", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "type", G_TYPE_GTYPE, G_TYPE_STRING,
          "related-to", GST_TYPE_TRACER_VALUE_SCOPE, GST_TRACER_VALUE_SCOPE_PAD,
          NULL),
      "to_pad", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "type", G_TYPE_GTYPE, G_TYPE_STRING,
          "related-to", GST_TYPE_TRACER_VALUE_SCOPE, GST_TRACER_VALUE_SCOPE_PAD,
          NULL),
      "time", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "type", G_TYPE_GTYPE, G_TYPE_STRING,
          "related-to", GST_TYPE_TRACER_VALUE_SCOPE, GST_TRACER_VALUE_SCOPE_PROCESS,
          NULL),
      NULL);
#else
  gst_tracer_log_trace (gst_structure_new ("interlatency.class",
      "from_pad", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "pad", /* TODO: use genum */
          NULL),
      "to_pad", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "pad", /* TODO: use genum */
          NULL),
      "time", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "process", /* TODO: use genum */
          NULL),
      NULL));
#endif
  /* *INDENT-ON* */

  oclass->dispose = gst_interlatency_tracer_dispose;
  oclass->constructed = gst_interlatency_tracer_constructed;
}

static void
gst_interlatency_tracer_init (GstInterLatencyTracer * self)
{
  GstTracer *tracer = GST_TRACER (self);

  /* In push mode, pre/post will be called before/after the peer chain
   * function has been called. For this reason, we only use -pre to avoid
   * accounting for the processing time of the peer element (the sink).
   */
  gst_tracing_register_hook (tracer, "pad-push-pre",
      G_CALLBACK (do_push_buffer_pre));
  gst_tracing_register_hook (tracer, "pad-push-list-pre",
      G_CALLBACK (do_push_list_pre));

  /* While in pull mode, pre/post will happend before and after the upstream
   * pull_range call is made, so it already only account for the upstream
   * processing time. As a side effect, in pull mode, we can measure the
   * source processing latency, while in push mode, we can't .
   */
  gst_tracing_register_hook (tracer, "pad-pull-range-pre",
      G_CALLBACK (do_pull_range_pre));
  gst_tracing_register_hook (tracer, "pad-pull-range-post",
      G_CALLBACK (do_pull_range_post));
  gst_tracing_register_hook (tracer, "pad-push-event-pre",
      G_CALLBACK (do_push_event_pre));

}

static void
gst_interlatency_tracer_dispose (GObject * object)
{
#ifdef GST_NVDS_ENABLE
  g_mutex_lock (&probes_lock);
  g_clear_pointer (&mux_rings, g_hash_table_unref);
  g_mutex_unlock (&probes_lock);
#endif

  G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
gst_interlatency_tracer_constructed (GObject *object)
{
  gchar *metadata_event = NULL;

  /* chain up so parent constructed runs first (calls gst_ctf_init) */
  G_OBJECT_CLASS (gst_interlatency_tracer_parent_class)->constructed (object);

  metadata_event =
      g_strdup_printf (interlatency_metadata_event, INTERLATENCY_EVENT_ID, 0);
  add_metadata_event_struct (metadata_event);
  g_free (metadata_event);
}
