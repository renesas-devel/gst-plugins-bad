/* GStreamer Wayland video sink
 *
 * Copyright (C) 2011 Intel Corporation
 * Copyright (C) 2011 Sreerenj Balachandran <sreerenj.balachandran@intel.com>
 * Copyright (C) 2012 Wim Taymans <wim.taymans@gmail.com>
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
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 */

/**
 * SECTION:element-waylandsink
 *
 *  The waylandsink is creating its own window and render the decoded video frames to that.
 *  Setup the Wayland environment as described in
 *  <ulink url="http://wayland.freedesktop.org/building.html">Wayland</ulink> home page.
 *  The current implementaion is based on weston compositor.
 *
 * <refsect2>
 * <title>Example pipelines</title>
 * |[
 * gst-launch -v videotestsrc ! waylandsink
 * ]| test the video rendering in wayland
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "gstwaylandsink.h"

/* signals */
enum
{
  SIGNAL_0,
  LAST_SIGNAL
};

/* Properties */
enum
{
  PROP_0,
  PROP_WAYLAND_DISPLAY
};

GST_DEBUG_CATEGORY (gstwayland_debug);
#define GST_CAT_DEFAULT gstwayland_debug

#if G_BYTE_ORDER == G_BIG_ENDIAN
#define CAPS "{xRGB, ARGB}"
#else
#define CAPS "{BGRx, BGRA}"
#endif

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (CAPS))
    );

/*Fixme: Add more interfaces */
#define gst_wayland_sink_parent_class parent_class
G_DEFINE_TYPE (GstWaylandSink, gst_wayland_sink, GST_TYPE_VIDEO_SINK);

static void gst_wayland_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec);
static void gst_wayland_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec);
static void gst_wayland_sink_finalize (GObject * object);
static GstCaps *gst_wayland_sink_get_caps (GstBaseSink * bsink,
    GstCaps * filter);
static gboolean gst_wayland_sink_set_caps (GstBaseSink * bsink, GstCaps * caps);
static gboolean gst_wayland_sink_start (GstBaseSink * bsink);
static gboolean gst_wayland_sink_preroll (GstBaseSink * bsink,
    GstBuffer * buffer);
static gboolean
gst_wayland_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query);
static gboolean gst_wayland_sink_render (GstBaseSink * bsink,
    GstBuffer * buffer);

static gboolean create_display (GstWaylandSink * sink);
static void registry_handle_global (void *data, struct wl_registry *registry,
    uint32_t id, const char *interface, uint32_t version);
static void frame_redraw_callback (void *data,
    struct wl_callback *callback, uint32_t time);
static void create_window (GstWaylandSink * sink, struct display *display,
    int width, int height);
static void shm_pool_destroy (struct shm_pool *pool);

typedef struct
{
  uint32_t wl_format;
  GstVideoFormat gst_format;
} wl_VideoFormat;

static const wl_VideoFormat formats[] = {
#if G_BYTE_ORDER == G_BIG_ENDIAN
  {WL_SHM_FORMAT_XRGB8888, GST_VIDEO_FORMAT_xRGB},
  {WL_SHM_FORMAT_ARGB8888, GST_VIDEO_FORMAT_ARGB},
#else
  {WL_SHM_FORMAT_XRGB8888, GST_VIDEO_FORMAT_BGRx},
  {WL_SHM_FORMAT_ARGB8888, GST_VIDEO_FORMAT_BGRA},
#endif
};

static int fullscreen;

static uint32_t
gst_wayland_format_to_wl_format (GstVideoFormat format)
{
  guint i;

  for (i = 0; i < G_N_ELEMENTS (formats); i++)
    if (formats[i].gst_format == format)
      return formats[i].wl_format;

  GST_WARNING ("wayland video format not found");
  return -1;
}

#ifndef GST_DISABLE_GST_DEBUG
static const gchar *
gst_wayland_format_to_string (uint32_t wl_format)
{
  guint i;
  GstVideoFormat format = GST_VIDEO_FORMAT_UNKNOWN;

  for (i = 0; i < G_N_ELEMENTS (formats); i++)
    if (formats[i].wl_format == wl_format)
      format = formats[i].gst_format;

  return gst_video_format_to_string (format);
}
#endif

static void
gst_wayland_sink_class_init (GstWaylandSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  gobject_class->set_property = gst_wayland_sink_set_property;
  gobject_class->get_property = gst_wayland_sink_get_property;
  gobject_class->finalize = GST_DEBUG_FUNCPTR (gst_wayland_sink_finalize);

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&sink_template));

  gst_element_class_set_static_metadata (gstelement_class,
      "wayland video sink", "Sink/Video",
      "Output to wayland surface",
      "Sreerenj Balachandran <sreerenj.balachandran@intel.com>");

  gstbasesink_class->get_caps = GST_DEBUG_FUNCPTR (gst_wayland_sink_get_caps);
  gstbasesink_class->set_caps = GST_DEBUG_FUNCPTR (gst_wayland_sink_set_caps);
  gstbasesink_class->start = GST_DEBUG_FUNCPTR (gst_wayland_sink_start);
  gstbasesink_class->preroll = GST_DEBUG_FUNCPTR (gst_wayland_sink_preroll);
  gstbasesink_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_propose_allocation);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_wayland_sink_render);

  g_object_class_install_property (gobject_class, PROP_WAYLAND_DISPLAY,
      g_param_spec_pointer ("wayland-display", "Wayland Display",
          "Wayland  Display handle created by the application ",
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_wayland_sink_init (GstWaylandSink * sink)
{
  sink->display = g_malloc0 (sizeof (struct display));
  if (!sink->display)
    GST_ELEMENT_ERROR (sink, RESOURCE, NO_SPACE_LEFT,
        ("Could not allocate display"), ("Could not allocate display"));
  sink->display->drm_fd = -1;

  sink->window = NULL;
  sink->shm_pool = NULL;
  sink->pool = NULL;
  sink->ext_display = FALSE;

  g_mutex_init (&sink->wayland_lock);
}

static void
gst_wayland_sink_get_property (GObject * object,
    guint prop_id, GValue * value, GParamSpec * pspec)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (object);

  switch (prop_id) {
    case PROP_WAYLAND_DISPLAY:
      g_value_set_pointer (value, sink->display->display);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_wayland_sink_set_property (GObject * object,
    guint prop_id, const GValue * value, GParamSpec * pspec)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (object);

  switch (prop_id) {
    case PROP_WAYLAND_DISPLAY:
      sink->display->display = g_value_get_pointer (value);
      sink->ext_display = TRUE;
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
destroy_display (struct display *display, gboolean ext_display)
{
  if (display->shm)
    wl_shm_destroy (display->shm);

  if (display->shell)
    wl_shell_destroy (display->shell);

  if (display->compositor)
    wl_compositor_destroy (display->compositor);

  wl_display_flush (display->display);
  if (!ext_display)
    wl_display_disconnect (display->display);

  if (display->drm_fd >= 0)
    close (display->drm_fd);

  g_free (display);
}

static void
destroy_window (struct window *window)
{
  if (window->callback)
    wl_callback_destroy (window->callback);

  if (window->buffer)
    wl_buffer_destroy (window->buffer);

  if (window->shell_surface)
    wl_shell_surface_destroy (window->shell_surface);

  if (window->surface)
    wl_surface_destroy (window->surface);

  free (window);
}

static void
shm_pool_destroy (struct shm_pool *pool)
{
  munmap (pool->data, pool->size);
  wl_shm_pool_destroy (pool->pool);
  free (pool);
}

static void
gst_wayland_sink_finalize (GObject * object)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (object);

  GST_DEBUG_OBJECT (sink, "Finalizing the sink..");

  if (sink->window)
    destroy_window (sink->window);
  if (sink->display)
    destroy_display (sink->display, sink->ext_display);
  if (sink->shm_pool)
    shm_pool_destroy (sink->shm_pool);

  g_mutex_clear (&sink->wayland_lock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static GstCaps *
gst_wayland_sink_get_caps (GstBaseSink * bsink, GstCaps * filter)
{
  GstWaylandSink *sink;
  GstCaps *caps;
  int i;

  sink = GST_WAYLAND_SINK (bsink);
  caps =
      gst_caps_copy (gst_pad_get_pad_template_caps (GST_VIDEO_SINK_PAD (sink)));

  if (!sink->window || !sink->window->screen_valid)
    goto skip;

  for (i = 0; i < gst_caps_get_size (caps); i++) {
    GstStructure *structure = gst_caps_get_structure (caps, i);

    gst_structure_set (structure,
        "width", G_TYPE_INT, sink->window->width,
        "height", G_TYPE_INT, sink->window->height, NULL);
  }

skip:

  if (filter) {
    GstCaps *intersection;

    intersection =
        gst_caps_intersect_full (filter, caps, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref (caps);
    caps = intersection;
  }
  return caps;
}

static void
shm_format (void *data, struct wl_shm *wl_shm, uint32_t format)
{
  struct display *d = data;

  d->formats |= (1 << format);
}

struct wl_shm_listener shm_listenter = {
  shm_format
};

#ifdef HAVE_WAYLAND_KMS
static void
kms_device (void *data, struct wl_kms *kms, const char *device)
{
  struct display *d = data;
  drm_magic_t magic;

  if ((d->drm_fd = open (device, O_RDWR | O_CLOEXEC)) < 0) {
    GST_ERROR ("%s open failed (%s)", device, strerror (errno));
    return;
  }

  drmGetMagic (d->drm_fd, &magic);
  wl_kms_authenticate (d->wl_kms, magic);
}

static void
kms_format (void *data, struct wl_kms *wl_shm, uint32_t format)
{
  struct display *d = data;

  if (format == WL_KMS_FORMAT_ARGB8888)
    d->kms_argb_supported = TRUE;

  GST_DEBUG ("kms_formats = 0x%08x", format);
}

static void
kms_handle_authenticated (void *data, struct wl_kms *kms)
{
  struct display *d = data;

  GST_DEBUG ("wl_kms has been authenticated");

  d->authenticated = TRUE;
}

static const struct wl_kms_listener kms_listenter = {
  .device = kms_device,
  .format = kms_format,
  .authenticated = kms_handle_authenticated
};
#endif

static void
registry_handle_global (void *data, struct wl_registry *registry,
    uint32_t id, const char *interface, uint32_t version)
{
  struct display *d = data;

  if (strcmp (interface, "wl_compositor") == 0) {
    d->compositor =
        wl_registry_bind (registry, id, &wl_compositor_interface, 1);
  } else if (strcmp (interface, "wl_shell") == 0) {
    d->shell = wl_registry_bind (registry, id, &wl_shell_interface, 1);
  } else if (strcmp (interface, "wl_shm") == 0) {
    d->shm = wl_registry_bind (registry, id, &wl_shm_interface, 1);
    wl_shm_add_listener (d->shm, &shm_listenter, d);
  } else if (strcmp (interface, "wl_output") == 0) {
    d->output = wl_registry_bind (registry, id, &wl_output_interface, 1);       /* always last display */
#ifdef HAVE_WAYLAND_KMS
  } else if (strcmp (interface, "wl_kms") == 0) {
    d->wl_kms = wl_registry_bind (registry, id, &wl_kms_interface, version);
    wl_kms_add_listener (d->wl_kms, &kms_listenter, d);
  }
#else
  }
#endif
}

static const struct wl_registry_listener registry_listener = {
  registry_handle_global
};

static void
handle_ping (void *data, struct wl_shell_surface *shell_surface,
    uint32_t serial)
{
  wl_shell_surface_pong (shell_surface, serial);
}

static void
handle_configure (void *data, struct wl_shell_surface *shell_surface,
    uint32_t edges, int32_t width, int32_t height)
{
  struct window *window = data;
  GST_DEBUG_OBJECT (NULL, "handle_configure: width = %d, height= %d", width,
      height);
  if (fullscreen) {
    window->width = width;
    window->height = height;
    window->screen_valid = TRUE;
  }
}

static void
handle_popup_done (void *data, struct wl_shell_surface *shell_surface)
{
}

static const struct wl_shell_surface_listener shell_surface_listener = {
  handle_ping,
  handle_configure,
  handle_popup_done
};

static gboolean
create_display (GstWaylandSink * sink)
{
  struct display *display;
  struct window *window;

  display = sink->display;

  if (!sink->ext_display) {
    GST_DEBUG_OBJECT (sink, "Try to connect wl_display by myself");
    display->display = wl_display_connect (NULL);
    if (display->display == NULL) {
      GST_ERROR_OBJECT (sink, "Failed to connect wl_display");
      return FALSE;
    }
  }

  display->registry = wl_display_get_registry (display->display);
  wl_registry_add_listener (display->registry, &registry_listener, display);

  wl_display_roundtrip (display->display);

  window = malloc (sizeof *window);
  window->display = display;
  window->redraw_pending = FALSE;
  window->screen_valid = FALSE;
  window->surface = wl_compositor_create_surface (display->compositor);

  if (display->shell) {
    window->shell_surface = wl_shell_get_shell_surface (display->shell,
        window->surface);

    if (!window->shell_surface) {
      GST_ERROR_OBJECT (sink, "Failed to create shell surface");
      return FALSE;
    }

    wl_shell_surface_add_listener (window->shell_surface,
        &shell_surface_listener, window);

    if (fullscreen) {
      wl_shell_surface_set_fullscreen (window->shell_surface,
          WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0, display->output);
    } else {
      wl_shell_surface_set_toplevel (window->shell_surface);
    }
  }
  sink->window = window;

#ifdef HAVE_WAYLAND_KMS
  if (!display->wl_kms && !display->shm) {
    GST_ERROR_OBJECT (sink,
        "Both wl_kms and wl_shm global objects couldn't be obtained");
    return FALSE;
  }
#else
  if (display->shm == NULL) {
    GST_ERROR_OBJECT (sink, "No wl_shm global..");
    return FALSE;
  }
#endif

  wl_display_roundtrip (display->display);

#ifdef HAVE_WAYLAND_KMS
  if (display->wl_kms && !display->kms_argb_supported) {
    GST_ERROR_OBJECT (sink, "wl_kms format isn't WL_KMS_FORMAT_ARGB8888");
    return FALSE;
  }

  wl_display_roundtrip (display->display);

  if (!display->authenticated) {
    GST_ERROR_OBJECT (sink, "Authentication failed...");
    return FALSE;
  }
#else
  if (!(display->formats & (1 << WL_SHM_FORMAT_XRGB8888))) {
    GST_ERROR_OBJECT (sink, "WL_SHM_FORMAT_XRGB32 not available");
    return FALSE;
  }
#endif

  wl_display_get_fd (display->display);

  /* Create a new event queue for frame callback */
  display->wl_queue = wl_display_create_queue (display->display);
  if (!display->wl_queue) {
    GST_ERROR_OBJECT (sink, "Failed to create an event queue");
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_wayland_sink_format_from_caps (uint32_t * wl_format, GstCaps * caps)
{
  GstStructure *structure;
  const gchar *format;
  GstVideoFormat fmt;

  structure = gst_caps_get_structure (caps, 0);
  format = gst_structure_get_string (structure, "format");
  fmt = gst_video_format_from_string (format);

  *wl_format = gst_wayland_format_to_wl_format (fmt);

  return (*wl_format != -1);
}

static gboolean
gst_wayland_sink_set_caps (GstBaseSink * bsink, GstCaps * caps)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (bsink);
  GstBufferPool *newpool, *oldpool;
  GstVideoInfo info;
  GstStructure *structure;
  static GstAllocationParams params = { 0, 0, 0, 15, };
  guint size;

  sink = GST_WAYLAND_SINK (bsink);

  GST_LOG_OBJECT (sink, "set caps %" GST_PTR_FORMAT, caps);

  if (!gst_video_info_from_caps (&info, caps))
    goto invalid_format;

  if (!gst_wayland_sink_format_from_caps (&sink->format, caps))
    goto invalid_format;

  if (!(sink->display->formats & (1 << sink->format))) {
    GST_DEBUG_OBJECT (sink, "%s not available",
        gst_wayland_format_to_string (sink->format));
    return FALSE;
  }

  sink->video_width = info.width;
  sink->video_height = info.height;
  size = info.size;

  /* create a new pool for the new configuration */
  newpool = gst_wayland_buffer_pool_new (sink);

  if (!newpool) {
    GST_DEBUG_OBJECT (sink, "Failed to create new pool");
    return FALSE;
  }

  structure = gst_buffer_pool_get_config (newpool);
  gst_buffer_pool_config_set_params (structure, caps, size, 2, 0);
  gst_buffer_pool_config_set_allocator (structure, NULL, &params);
  if (!gst_buffer_pool_set_config (newpool, structure))
    goto config_failed;

  oldpool = sink->pool;
  sink->pool = newpool;
  if (oldpool)
    gst_object_unref (oldpool);

  return TRUE;

invalid_format:
  {
    GST_DEBUG_OBJECT (sink,
        "Could not locate image format from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
config_failed:
  {
    GST_DEBUG_OBJECT (bsink, "failed setting config");
    return FALSE;
  }
}

static void
create_window (GstWaylandSink * sink, struct display *display, int width,
    int height)
{
  struct window *window;

/*  if (sink->window)
    return; */

  g_mutex_lock (&sink->wayland_lock);

  window = sink->window;

  window->width = width;
  window->height = height;

/*
  window = malloc (sizeof *window);
  window->display = display;
  window->redraw_pending = FALSE;
  window->surface = wl_compositor_create_surface (display->compositor);

  if (display->shell) {
    window->shell_surface = wl_shell_get_shell_surface (display->shell,
        window->surface);

    g_return_if_fail (window->shell_surface);

    wl_shell_surface_add_listener (window->shell_surface,
        &shell_surface_listener, window);

    wl_shell_surface_set_toplevel (window->shell_surface);
  }

  sink->window = window;
*/
  window->init_complete = TRUE;
  g_mutex_unlock (&sink->wayland_lock);
}

static gboolean
gst_wayland_sink_start (GstBaseSink * bsink)
{
  GstWaylandSink *sink = (GstWaylandSink *) bsink;
  char *env_full;

  GST_DEBUG_OBJECT (sink, "start");

  env_full = getenv ("WAYLANDSINK_FULLSCREEN");

  fullscreen = (env_full == NULL) ? 0 : atoi (env_full);

  if (!create_display (sink)) {
    GST_ELEMENT_ERROR (bsink, RESOURCE, OPEN_READ_WRITE,
        ("Could not initialise Wayland output"),
        ("Could not create Wayland display"));
    return FALSE;
  }

  return TRUE;
}

static gboolean
gst_wayland_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (bsink);
  GstBufferPool *pool;
  GstStructure *config;
  GstCaps *caps;
  guint size;
  gboolean need_pool;

  gst_query_parse_allocation (query, &caps, &need_pool);

  if (caps == NULL)
    goto no_caps;

  g_mutex_lock (&sink->wayland_lock);
  if ((pool = sink->pool))
    gst_object_ref (pool);
  g_mutex_unlock (&sink->wayland_lock);

  if (pool != NULL) {
    GstCaps *pcaps;

    /* we had a pool, check caps */
    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_params (config, &pcaps, &size, NULL, NULL);

    if (!gst_caps_is_equal (caps, pcaps)) {
      /* different caps, we can't use this pool */
      gst_object_unref (pool);
      pool = NULL;
    }
    gst_structure_free (config);
  }

  if (pool == NULL && need_pool) {
    GstVideoInfo info;

    if (!gst_video_info_from_caps (&info, caps))
      goto invalid_caps;

    GST_DEBUG_OBJECT (sink, "create new pool");
    pool = gst_wayland_buffer_pool_new (sink);

    /* the normal size of a frame */
    size = info.size;

    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_set_params (config, caps, size, 3, 0);
    if (!gst_buffer_pool_set_config (pool, config))
      goto config_failed;
  }
  if (pool) {
    gst_query_add_allocation_pool (query, pool, size, 3, 0);
    gst_object_unref (pool);
  }

  return TRUE;

  /* ERRORS */
no_caps:
  {
    GST_DEBUG_OBJECT (bsink, "no caps specified");
    return FALSE;
  }
invalid_caps:
  {
    GST_DEBUG_OBJECT (bsink, "invalid caps specified");
    return FALSE;
  }
config_failed:
  {
    GST_DEBUG_OBJECT (bsink, "failed setting config");
    gst_object_unref (pool);
    return FALSE;
  }
}

static GstFlowReturn
gst_wayland_sink_preroll (GstBaseSink * bsink, GstBuffer * buffer)
{
  GST_DEBUG_OBJECT (bsink, "preroll buffer %p", buffer);
  return gst_wayland_sink_render (bsink, buffer);
}

static void
frame_redraw_callback (void *data, struct wl_callback *callback, uint32_t time)
{
  struct window *window = (struct window *) data;
  window->redraw_pending = FALSE;
  wl_callback_destroy (callback);
}

static const struct wl_callback_listener frame_callback_listener = {
  frame_redraw_callback
};

static GstFlowReturn
gst_wayland_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (bsink);
  GstVideoRectangle src, dst, res;
  GstBuffer *to_render;
  GstWlMeta *meta;
  GstFlowReturn ret;
  struct window *window;
  struct display *display;

  GST_LOG_OBJECT (sink, "render buffer %p", buffer);
  if (!sink->window->init_complete)
    create_window (sink, sink->display, sink->video_width, sink->video_height);

  window = sink->window;
  display = sink->display;

  meta = gst_buffer_get_wl_meta (buffer);

  while (window->redraw_pending)
    wl_display_dispatch_queue (display->display, display->wl_queue);

  if (meta && meta->sink == sink) {
    GST_LOG_OBJECT (sink, "buffer %p from our pool, writing directly", buffer);
    to_render = buffer;
  } else {
    GstMapInfo src;
    GST_LOG_OBJECT (sink, "buffer %p not from our pool, copying", buffer);

    if (!sink->pool)
      goto no_pool;

    if (!gst_buffer_pool_set_active (sink->pool, TRUE))
      goto activate_failed;

    ret = gst_buffer_pool_acquire_buffer (sink->pool, &to_render, NULL);
    if (ret != GST_FLOW_OK)
      goto no_buffer;

    gst_buffer_map (buffer, &src, GST_MAP_READ);
    gst_buffer_fill (to_render, 0, src.data, src.size);
    gst_buffer_unmap (buffer, &src);

    meta = gst_buffer_get_wl_meta (to_render);
  }

  src.w = sink->video_width;
  src.h = sink->video_height;
  dst.w = sink->window->width;
  dst.h = sink->window->height;

  gst_video_sink_center_rect (src, dst, &res, FALSE);

  wl_surface_attach (sink->window->surface, meta->wbuffer, 0, 0);
  wl_surface_damage (sink->window->surface, 0, 0, res.w, res.h);
  window->redraw_pending = TRUE;
  window->callback = wl_surface_frame (window->surface);
  wl_callback_add_listener (window->callback, &frame_callback_listener, window);
  wl_proxy_set_queue ((struct wl_proxy *) window->callback, display->wl_queue);
  wl_surface_commit (window->surface);
  wl_display_dispatch (display->display);

  if (buffer != to_render)
    gst_buffer_unref (to_render);
  return GST_FLOW_OK;

no_buffer:
  {
    GST_WARNING_OBJECT (sink, "could not create image");
    return ret;
  }
no_pool:
  {
    GST_ELEMENT_ERROR (sink, RESOURCE, WRITE,
        ("Internal error: can't allocate images"),
        ("We don't have a bufferpool negotiated"));
    return GST_FLOW_ERROR;
  }
activate_failed:
  {
    GST_ERROR_OBJECT (sink, "failed to activate bufferpool.");
    ret = GST_FLOW_ERROR;
    return ret;
  }
}

static gboolean
plugin_init (GstPlugin * plugin)
{
  GST_DEBUG_CATEGORY_INIT (gstwayland_debug, "waylandsink", 0,
      " wayland video sink");

  return gst_element_register (plugin, "waylandsink", GST_RANK_MARGINAL,
      GST_TYPE_WAYLAND_SINK);
}

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    waylandsink,
    "Wayland Video Sink", plugin_init, VERSION, "LGPL", GST_PACKAGE_NAME,
    GST_PACKAGE_ORIGIN)
