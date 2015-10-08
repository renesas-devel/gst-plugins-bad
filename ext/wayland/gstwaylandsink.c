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
#ifdef HAVE_WAYLAND_KMS
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE
        ("{ BGRx, BGRA, RGBx, xBGR, xRGB, RGBA, ABGR, ARGB, RGB, BGR, "
            "RGB16, BGR16, YUY2, YVYU, UYVY, AYUV, NV12, NV21, NV16, "
            "IYU1, I420, v308 }"))
#else
    GST_STATIC_CAPS (GST_VIDEO_CAPS_MAKE (CAPS))
#endif
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
static gboolean gst_wayland_sink_stop (GstBaseSink * bsink);
static gboolean gst_wayland_sink_preroll (GstBaseSink * bsink,
    GstBuffer * buffer);
static gboolean
gst_wayland_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query);
static gboolean gst_wayland_sink_render (GstBaseSink * bsink,
    GstBuffer * buffer);
static gboolean gst_wayland_sink_query (GstBaseSink * bsink, GstQuery * query);
static GstStateChangeReturn gst_wayland_sink_change_state (GstElement * element,
    GstStateChange transition);

static gboolean create_display (GstWaylandSink * sink);
static void registry_handle_global (void *data, struct wl_registry *registry,
    uint32_t id, const char *interface, uint32_t version);
static void create_window (GstWaylandSink * sink, struct display *display,
    int width, int height);
static void shm_pool_destroy (struct shm_pool *pool);


typedef struct
{
  uint32_t wl_format;
  GstVideoFormat gst_format;
} wl_VideoFormat;

static const wl_VideoFormat formats[] = {
#ifdef HAVE_WAYLAND_KMS
#if G_BYTE_ORDER == G_BIG_ENDIAN
  {WL_KMS_FORMAT_XRGB8888, GST_VIDEO_FORMAT_xRGB},
  {WL_KMS_FORMAT_XBGR8888, GST_VIDEO_FORMAT_BGRx},
  {WL_KMS_FORMAT_RGBX8888, GST_VIDEO_FORMAT_RGBx},
  {WL_KMS_FORMAT_BGRX8888, GST_VIDEO_FORMAT_xBGR},
  {WL_KMS_FORMAT_ARGB8888, GST_VIDEO_FORMAT_ARGB},
  {WL_KMS_FORMAT_ABGR8888, GST_VIDEO_FORMAT_ABGR},
  {WL_KMS_FORMAT_RGBA8888, GST_VIDEO_FORMAT_RGBA},
  {WL_KMS_FORMAT_BGRA8888, GST_VIDEO_FORMAT_BGRA},
#else
  {WL_KMS_FORMAT_XRGB8888, GST_VIDEO_FORMAT_BGRx},
  {WL_KMS_FORMAT_XBGR8888, GST_VIDEO_FORMAT_xRGB},
  {WL_KMS_FORMAT_RGBX8888, GST_VIDEO_FORMAT_xBGR},
  {WL_KMS_FORMAT_BGRX8888, GST_VIDEO_FORMAT_RGBx},
  {WL_KMS_FORMAT_ARGB8888, GST_VIDEO_FORMAT_BGRA},
  {WL_KMS_FORMAT_ABGR8888, GST_VIDEO_FORMAT_RGBA},
  {WL_KMS_FORMAT_RGBA8888, GST_VIDEO_FORMAT_ABGR},
  {WL_KMS_FORMAT_BGRA8888, GST_VIDEO_FORMAT_ARGB},
#endif /* G_BYTE_ORDER == G_BIG_ENDIAN */
  {WL_KMS_FORMAT_RGB565, GST_VIDEO_FORMAT_RGB16},
  {WL_KMS_FORMAT_BGR565, GST_VIDEO_FORMAT_BGR16},
  {WL_KMS_FORMAT_RGB888, GST_VIDEO_FORMAT_RGB},
  {WL_KMS_FORMAT_BGR888, GST_VIDEO_FORMAT_BGR},
  {WL_KMS_FORMAT_YUYV, GST_VIDEO_FORMAT_YUY2},
  {WL_KMS_FORMAT_YVYU, GST_VIDEO_FORMAT_YVYU},
  {WL_KMS_FORMAT_UYVY, GST_VIDEO_FORMAT_UYVY},
  {WL_KMS_FORMAT_AYUV, GST_VIDEO_FORMAT_AYUV},
  {WL_KMS_FORMAT_NV12, GST_VIDEO_FORMAT_NV12},
  {WL_KMS_FORMAT_NV21, GST_VIDEO_FORMAT_NV21},
  {WL_KMS_FORMAT_NV16, GST_VIDEO_FORMAT_NV16},
  {WL_KMS_FORMAT_YUV411, GST_VIDEO_FORMAT_IYU1},
  {WL_KMS_FORMAT_YUV420, GST_VIDEO_FORMAT_I420},
  {WL_KMS_FORMAT_YUV422, GST_VIDEO_FORMAT_YUY2},
  {WL_KMS_FORMAT_YVU422, GST_VIDEO_FORMAT_YVYU},
  {WL_KMS_FORMAT_YUV444, GST_VIDEO_FORMAT_v308},
#else
#if G_BYTE_ORDER == G_BIG_ENDIAN
  {WL_SHM_FORMAT_XRGB8888, GST_VIDEO_FORMAT_xRGB},
  {WL_SHM_FORMAT_ARGB8888, GST_VIDEO_FORMAT_ARGB},
#else
  {WL_SHM_FORMAT_XRGB8888, GST_VIDEO_FORMAT_BGRx},
  {WL_SHM_FORMAT_ARGB8888, GST_VIDEO_FORMAT_BGRA},
#endif
#endif /* HAVE_WAYLAND_KMS */
};

static int fullscreen;

uint32_t
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
  gstbasesink_class->stop = GST_DEBUG_FUNCPTR (gst_wayland_sink_stop);
  gstbasesink_class->preroll = GST_DEBUG_FUNCPTR (gst_wayland_sink_preroll);
  gstbasesink_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_propose_allocation);
  gstbasesink_class->render = GST_DEBUG_FUNCPTR (gst_wayland_sink_render);
  gstbasesink_class->query = GST_DEBUG_FUNCPTR (gst_wayland_sink_query);
  gstelement_class->change_state =
      GST_DEBUG_FUNCPTR (gst_wayland_sink_change_state);

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
#ifdef HAVE_WAYLAND_KMS
  sink->display->drm_fd = -1;
#endif

  sink->window = NULL;
  sink->shm_pool = NULL;
  sink->pool = NULL;
  sink->ext_display = FALSE;
  sink->preroll_buffer = NULL;

  g_mutex_init (&sink->wayland_lock);

  g_cond_init (&sink->sync_cond);
  g_mutex_init (&sink->sync_mutex);

  sink->wl_fd_poll = gst_poll_new (TRUE);
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

  if (display->display) {
    wl_display_flush (display->display);
    if (!ext_display)
      wl_display_disconnect (display->display);
  }
#ifdef HAVE_WAYLAND_KMS
  if (display->drm_fd >= 0)
    close (display->drm_fd);
#endif
}

static void
destroy_window (struct window *window)
{
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

  g_free (sink->display);

  if (sink->shm_pool)
    shm_pool_destroy (sink->shm_pool);

  gst_poll_free (sink->wl_fd_poll);

  g_mutex_clear (&sink->wayland_lock);

  g_cond_clear (&sink->sync_cond);
  g_mutex_clear (&sink->sync_mutex);

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
kms_color_fmt_free (uint32_t * fmt)
{
  g_slice_free (uint32_t, fmt);
}

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
  uint32_t *fmt;

  fmt = g_slice_new (uint32_t);
  *fmt = format;
  d->support_fmt_list = g_list_append (d->support_fmt_list, fmt);

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

static gpointer
gst_wl_display_thread_run (gpointer data)
{
  GstWaylandSink *sink = data;
  struct display *display;
  GstPollFD pollfd = GST_POLL_FD_INIT;

  display = sink->display;

  pollfd.fd = wl_display_get_fd (display->display);
  gst_poll_add_fd (sink->wl_fd_poll, &pollfd);
  gst_poll_fd_ctl_read (sink->wl_fd_poll, &pollfd, TRUE);

  /* main loop */
  while (1) {
    while (wl_display_prepare_read_queue (display->display,
            display->wl_queue) != 0)
      wl_display_dispatch_queue_pending (display->display, display->wl_queue);
    wl_display_flush (display->display);

    if (gst_poll_wait (sink->wl_fd_poll, GST_CLOCK_TIME_NONE) < 0) {
      gboolean normal = (errno == EBUSY);
      wl_display_cancel_read (display->display);
      if (normal)
        break;
      else
        goto error;
    } else {
      wl_display_read_events (display->display);
      wl_display_dispatch_queue_pending (display->display, display->wl_queue);
    }
  }

  return NULL;

error:
  GST_ERROR ("Error communicating with the wayland server");
  return NULL;
}

static gboolean
create_display (GstWaylandSink * sink)
{
  struct display *display;
  struct window *window;
  GError *err = NULL;

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

  window = g_malloc0 (sizeof *window);
  window->display = display;
  window->committed_num = 0;
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
  if (display->wl_kms && !display->support_fmt_list) {
    GST_ERROR_OBJECT (sink, "Could not get wl_kms support color format list");
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

  sink->thread = g_thread_try_new ("GstWaylandSink", gst_wl_display_thread_run,
      sink, &err);
  if (err) {
    GST_ERROR_OBJECT (sink,
        "Failed to start thread for the display's events: '%s'", err->message);
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

#ifdef HAVE_WAYLAND_KMS
static gboolean
gst_wayland_sink_is_kms_color_format_supported (GstWaylandSink * sink,
    uint32_t wl_fmt)
{
  GList *l;
  gboolean ret = FALSE;
  struct display *display;

  display = sink->display;

  if (display->support_fmt_list == NULL)
    return FALSE;

  for (l = display->support_fmt_list; l; l = l->next) {
    uint32_t *fmt = l->data;

    if (*fmt == wl_fmt) {
      ret = TRUE;
      break;
    }
  }

  return ret;
}
#endif

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

#ifdef HAVE_WAYLAND_KMS
  if (sink->display->wl_kms) {
    if (!gst_wayland_sink_is_kms_color_format_supported (sink, sink->format)) {
      GST_DEBUG_OBJECT (sink, "%s not available",
          gst_wayland_format_to_string (sink->format));
      return FALSE;
    }
  } else {
    if (!(sink->display->formats & (1 << sink->format))) {
      GST_DEBUG_OBJECT (sink, "%s not available",
          gst_wayland_format_to_string (sink->format));
      return FALSE;
    }
  }
#else
  if (!(sink->display->formats & (1 << sink->format))) {
    GST_DEBUG_OBJECT (sink, "%s not available",
        gst_wayland_format_to_string (sink->format));
    return FALSE;
  }
#endif

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
#ifdef HAVE_WAYLAND_KMS
  gst_structure_set (structure, "videosink_buffer_creation_request_supported",
      G_TYPE_BOOLEAN, TRUE, NULL);
#endif
  gst_buffer_pool_config_set_params (structure, caps, size,
      GST_WAYLAND_BUFFER_POOL_NUM, GST_WAYLAND_BUFFER_POOL_NUM);
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
gst_wayland_sink_stop (GstBaseSink * bsink)
{
  GstWaylandSink *sink = (GstWaylandSink *) bsink;
  struct display *display;

  GST_DEBUG_OBJECT (sink, "stop");

  display = sink->display;

  gst_poll_set_flushing (sink->wl_fd_poll, TRUE);
  g_thread_join (sink->thread);

  if (sink->pool) {
    gst_object_unref (sink->pool);
    sink->pool = NULL;
  }

  if (sink->window)
    destroy_window (sink->window);
  if (sink->display)
    destroy_display (display, sink->ext_display);

#ifdef HAVE_WAYLAND_KMS
  g_list_free_full (display->support_fmt_list,
      (GDestroyNotify) kms_color_fmt_free);
  display->support_fmt_list = NULL;
#endif

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
#ifdef HAVE_WAYLAND_KMS
  GstAllocator *allocator;
#endif
  GstAllocationParams params;

#ifdef HAVE_WAYLAND_KMS
  gst_allocation_params_init (&params);
#endif
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
    gst_buffer_pool_config_set_params (config, caps, size,
        GST_WAYLAND_BUFFER_POOL_NUM, GST_WAYLAND_BUFFER_POOL_NUM);
    gst_structure_set (config, "videosink_buffer_creation_request_supported",
        G_TYPE_BOOLEAN, TRUE, NULL);
#ifdef HAVE_WAYLAND_KMS
    gst_buffer_pool_config_set_allocator (config, NULL, &params);
#endif
    if (!gst_buffer_pool_set_config (pool, config))
      goto config_failed;
  }
  if (pool) {
    gst_query_add_allocation_pool (query, pool, size,
        GST_WAYLAND_BUFFER_POOL_NUM, GST_WAYLAND_BUFFER_POOL_NUM);
    /*
     * Add the default allocator for the plugins that can't use dmabuf
     * descriptors.
     */
    gst_query_add_allocation_param (query, gst_allocator_find (NULL), &params);

#ifdef HAVE_WAYLAND_KMS
    allocator = gst_dmabuf_allocator_new ();
    gst_query_add_allocation_param (query, allocator, &params);
    gst_object_unref (allocator);
#endif
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
  GstWaylandSink *sink = GST_WAYLAND_SINK (bsink);
  GstFlowReturn ret;

  GST_DEBUG_OBJECT (bsink, "preroll buffer %p", buffer);
  ret = gst_wayland_sink_render (bsink, buffer);
  if (ret == GST_FLOW_OK)
    sink->preroll_buffer = buffer;

  return ret;
}

static void
wl_sync_callback (void *data, struct wl_callback *callback, uint32_t serial)
{
  struct sync_cb_data *cb_data = data;

  g_mutex_lock (&cb_data->sink->sync_mutex);
  cb_data->done = 1;
  g_cond_signal (&cb_data->sink->sync_cond);
  g_mutex_unlock (&cb_data->sink->sync_mutex);

  wl_callback_destroy (callback);
}

static const struct wl_callback_listener wayland_sync_listener = {
  .done = wl_sync_callback
};

gboolean
wayland_sync (GstWaylandSink * sink)
{
  struct wl_callback *callback;
  struct display *display;
  struct sync_cb_data cb_data = { sink, 0 };
  gint64 timeout;
  gboolean ret = TRUE;

  display = sink->display;

  callback = wl_display_sync (display->display);
  wl_callback_add_listener (callback, &wayland_sync_listener, &cb_data);
  wl_proxy_set_queue ((struct wl_proxy *) callback, display->wl_queue);
  wl_display_flush (display->display);

  g_mutex_lock (&sink->sync_mutex);
  timeout = g_get_monotonic_time () + 2 * G_TIME_SPAN_SECOND;
  while (ret && !cb_data.done)
    ret = g_cond_wait_until (&sink->sync_cond, &sink->sync_mutex, timeout);
  g_mutex_unlock (&sink->sync_mutex);

  if (!cb_data.done)
    wl_callback_destroy (callback);

  return ret;
}

static void
gst_wayland_sink_center_rect (GstWaylandSink * sink, GstVideoRectangle * result,
    gboolean scaling)
{
  GstVideoRectangle src, dst;

  src.w = sink->video_width;
  src.h = sink->video_height;
  dst.w = sink->window->width;
  dst.h = sink->window->height;

  gst_video_sink_center_rect (src, dst, result, scaling);
}

static void
frame_redraw_callback (void *data, struct wl_callback *callback, uint32_t time)
{
  GST_LOG ("frame_redraw_cb");

  wl_callback_destroy (callback);
}

static const struct wl_callback_listener frame_callback_listener = {
  frame_redraw_callback
};

static GstFlowReturn
gst_wayland_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (bsink);
  GstVideoRectangle res;
  GstBuffer *to_render;
  GstWlMeta *meta;
  GstFlowReturn ret;
  struct window *window;
  struct display *display;
  struct wl_callback *callback;

  /* Avoid duplicate rendering of the first frame */
  if (sink->preroll_buffer) {
    if (sink->preroll_buffer == buffer)
      return GST_FLOW_OK;
    else
      sink->preroll_buffer = NULL;
  }

  GST_LOG_OBJECT (sink, "render buffer %p", buffer);
  if (!sink->window->init_complete)
    create_window (sink, sink->display, sink->video_width, sink->video_height);

  window = sink->window;
  display = sink->display;

  meta = gst_buffer_get_wl_meta (buffer);

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

  gst_wayland_sink_center_rect (sink, &res, FALSE);

  /* A wl_buffer release event for the one before a frame being displayed
   * will synchronize and follow a wl_surface_frame event.
   * No processing in the wl_surface_frame callback function in itself.
   */
  callback = wl_surface_frame (sink->window->surface);
  wl_proxy_set_queue ((struct wl_proxy *) callback, display->wl_queue);
  wl_callback_add_listener (callback, &frame_callback_listener, sink);

  /* Once increase a buffer reference count to take a buffer back to
   * the buffer pool, synchronizing with the frame sync callback.
   */
  gst_buffer_ref (buffer);

  g_atomic_int_inc (&sink->window->committed_num);

  wl_surface_attach (sink->window->surface, meta->wbuffer, 0, 0);
  wl_surface_damage (sink->window->surface, 0, 0, res.w, res.h);
  wl_surface_commit (window->surface);

  wl_display_flush (display->display);

  wayland_sync (sink);

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
gst_wayland_sink_query (GstBaseSink * bsink, GstQuery * query)
{
#ifdef HAVE_WAYLAND_KMS
  GstWaylandSink *sink = GST_WAYLAND_SINK (bsink);
#endif
  gboolean ret = FALSE;

  switch (GST_QUERY_TYPE (query)) {
#ifdef HAVE_WAYLAND_KMS
    case GST_QUERY_CUSTOM:
    {
      GstWaylandBufferPool *wpool;
      const GstStructure *structure;
      GstStructure *str_writable;
      gint dmabuf[GST_VIDEO_MAX_PLANES];
      GstAllocator *allocator;
      gint width, height;
      gint stride[GST_VIDEO_MAX_PLANES] = { 0 };
      const gchar *str;
      const GValue *p_val;
      GValue val = { 0, };
      GstVideoFormat format;
      GstBuffer *buffer;
      GArray *dmabuf_array;
      GArray *stride_array;
      gint n_planes;
      gint i;

      wpool = GST_WAYLAND_BUFFER_POOL_CAST (sink->pool);

      structure = gst_query_get_structure (query);
      if (structure == NULL
          || !gst_structure_has_name (structure,
              "videosink_buffer_creation_request")) {
        GST_LOG_OBJECT (sink, "not a videosink_buffer_creation_request query");
        break;
      }

      GST_DEBUG_OBJECT (sink,
          "received a videosink_buffer_creation_request query");

      gst_structure_get (structure, "width", G_TYPE_INT, &width,
          "height", G_TYPE_INT, &height, "stride", G_TYPE_ARRAY, &stride_array,
          "dmabuf", G_TYPE_ARRAY, &dmabuf_array,
          "n_planes", G_TYPE_INT, &n_planes,
          "allocator", G_TYPE_POINTER, &p_val,
          "format", G_TYPE_STRING, &str, NULL);

      allocator = (GstAllocator *) g_value_get_pointer (p_val);
      if (allocator == NULL) {
        GST_WARNING_OBJECT (sink,
            "an invalid allocator in videosink_buffer_creation_request query");
        break;
      }

      format = gst_video_format_from_string (str);
      if (format == GST_VIDEO_FORMAT_UNKNOWN) {
        GST_WARNING_OBJECT (sink,
            "invalid color format in videosink_buffer_creation_request query");
        break;
      }

      for (i = 0; i < n_planes; i++) {
        dmabuf[i] = g_array_index (dmabuf_array, gint, i);
        stride[i] = g_array_index (stride_array, gint, i);
        GST_DEBUG_OBJECT (sink, "plane:%d dmabuf:%d stride:%d\n", i, dmabuf[i],
            stride[i]);
      }

      GST_DEBUG_OBJECT (sink,
          "videosink_buffer_creation_request query param: width:%d height:%d allocator:%p format:%s",
          width, height, allocator, str);

      buffer = gst_wayland_buffer_pool_create_buffer_from_dmabuf (wpool,
          dmabuf, allocator, width, height, stride, format, n_planes);
      if (buffer == NULL) {
        GST_WARNING_OBJECT (sink,
            "failed to create a buffer from videosink_buffer_creation_request query");
        break;
      }

      g_value_init (&val, GST_TYPE_BUFFER);
      gst_value_set_buffer (&val, buffer);
      gst_buffer_unref (buffer);

      str_writable = gst_query_writable_structure (query);
      gst_structure_set_value (str_writable, "buffer", &val);

      ret = TRUE;
      break;
    }
#endif
    default:
      ret = GST_BASE_SINK_CLASS (parent_class)->query (bsink, query);
      break;
  }

  return ret;
}

static void
gst_wayland_sink_wait_for_all_buffers_release (GstWaylandSink * sink)
{
  if (!sink->window)
    return;

  /* The buffer release event can be buffered in the wayland server,
   * so the client side has to poll the server until all the buffers
   * we posted to the server are released.
   */
  wayland_sync (sink);
  while (g_atomic_int_get (&sink->window->committed_num) > 0) {
    usleep (10000);
    wayland_sync (sink);
  }
}

static GstStateChangeReturn
gst_wayland_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstWaylandSink *sink = GST_WAYLAND_SINK (element);
  GstStateChangeReturn ret;
  GstVideoRectangle res;

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);
  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (sink->window) {
        gst_wayland_sink_center_rect (sink, &res, FALSE);

        /* remove buffer from surface, show nothing */
        wl_surface_attach (sink->window->surface, NULL, 0, 0);
        wl_surface_damage (sink->window->surface, 0, 0, res.w, res.h);
        wl_surface_commit (sink->window->surface);
        wl_display_flush (sink->display->display);

        gst_wayland_sink_wait_for_all_buffers_release (sink);
      }
    default:
      break;
  }

  return ret;
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
