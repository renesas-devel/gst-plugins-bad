/* GStreamer
 * Copyright (C) 2012 Intel Corporation
 * Copyright (C) 2012 Sreerenj Balachandran <sreerenj.balachandran@intel.com>

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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* Object header */
#include "gstwaylandsink.h"

/* Debugging category */
#include <gst/gstinfo.h>

/* Helper functions */
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>

/* wl metadata */
GType
gst_wl_meta_api_get_type (void)
{
  static volatile GType type;
  static const gchar *tags[] =
      { "memory", "size", "colorspace", "orientation", NULL };

  if (g_once_init_enter (&type)) {
    GType _type = gst_meta_api_type_register ("GstWlMetaAPI", tags);
    g_once_init_leave (&type, _type);
  }
  return type;
}

static void
gst_wl_meta_free (GstWlMeta * meta, GstBuffer * buffer)
{
  gst_object_unref (meta->sink);
#ifdef HAVE_WAYLAND_KMS
  if (meta->kms_bo) {
    if (meta->data)
      kms_bo_unmap (meta->kms_bo);
    kms_bo_destroy (&meta->kms_bo);
  } else {
    if (meta->data)
      munmap (meta->data, meta->size);
  }
#else
  munmap (meta->data, meta->size);
#endif
  wl_buffer_destroy (meta->wbuffer);
}

static gboolean
gst_wl_meta_transform (GstBuffer * dest, GstMeta * meta, GstBuffer * buffer,
    GQuark type, gpointer data)
{
  GstWlMeta *dst_meta, *src_meta;

  src_meta = (GstWlMeta *) meta;

  if (GST_META_TRANSFORM_IS_COPY (type)) {
    dst_meta = (GstWlMeta *) gst_buffer_add_meta (dest, GST_WL_META_INFO, NULL);

    dst_meta->sink = gst_object_ref (src_meta->sink);
    dst_meta->wbuffer = src_meta->wbuffer;
    dst_meta->data = src_meta->data;
    dst_meta->size = src_meta->size;
#ifdef HAVE_WAYLAND_KMS
    dst_meta->kms_bo = src_meta->kms_bo;
#endif
  }

  return TRUE;
}

const GstMetaInfo *
gst_wl_meta_get_info (void)
{
  static const GstMetaInfo *wl_meta_info = NULL;

  if (g_once_init_enter (&wl_meta_info)) {
    const GstMetaInfo *meta =
        gst_meta_register (GST_WL_META_API_TYPE, "GstWlMeta",
        sizeof (GstWlMeta), (GstMetaInitFunction) NULL,
        (GstMetaFreeFunction) gst_wl_meta_free,
        (GstMetaTransformFunction) gst_wl_meta_transform);
    g_once_init_leave (&wl_meta_info, meta);
  }
  return wl_meta_info;
}

#ifdef HAVE_WAYLAND_KMS
GstBuffer *
gst_wayland_buffer_pool_create_buffer_from_dmabuf (GstWaylandBufferPool * wpool,
    gint dmabuf[GST_VIDEO_MAX_PLANES], GstAllocator * allocator, gint width,
    gint height, gint in_stride[GST_VIDEO_MAX_PLANES], GstVideoFormat format,
    gint n_planes)
{
  GstBuffer *buffer;
  GstWlMeta *wmeta;
  GstWaylandSink *sink;
  gsize offset[GST_VIDEO_MAX_PLANES] = { 0 };
  gint stride[GST_VIDEO_MAX_PLANES] = { 0 };
  gint i;

  sink = wpool->sink;

  buffer = gst_buffer_new ();

  wmeta = (GstWlMeta *) gst_buffer_add_meta (buffer, GST_WL_META_INFO, NULL);
  wmeta->sink = gst_object_ref (sink);

  wmeta->wbuffer =
      wl_kms_create_mp_buffer (sink->display->wl_kms, width, height,
      gst_wayland_format_to_wl_format (format), dmabuf[0], in_stride[0],
      dmabuf[1], in_stride[1], dmabuf[2], in_stride[2]);

  for (i = 0; i < n_planes; i++) {
    gst_buffer_append_memory (buffer,
        gst_dmabuf_allocator_alloc (allocator, dmabuf[i], 0));

    stride[i] = in_stride[i];
  }

  wmeta->data = NULL;

  gst_buffer_add_video_meta_full (buffer, GST_VIDEO_FRAME_FLAG_NONE, format,
      width, height, n_planes, offset, stride);

  /* To avoid deattaching meta data when a buffer returns to the buffer pool */
  GST_META_FLAG_SET (wmeta, GST_META_FLAG_POOLED);

  return buffer;
}
#endif

/* bufferpool */
static void gst_wayland_buffer_pool_finalize (GObject * object);

#define gst_wayland_buffer_pool_parent_class parent_class
G_DEFINE_TYPE (GstWaylandBufferPool, gst_wayland_buffer_pool,
    GST_TYPE_BUFFER_POOL);

static gboolean
wayland_buffer_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  GstWaylandBufferPool *wpool = GST_WAYLAND_BUFFER_POOL_CAST (pool);
#ifdef HAVE_WAYLAND_KMS
  GstAllocationParams params;
#endif
  GstVideoInfo info;
  GstCaps *caps;

  if (wpool->allocator)
    gst_object_unref (wpool->allocator);
  wpool->allocator = NULL;

  if (!gst_buffer_pool_config_get_params (config, &caps, NULL, NULL, NULL))
    goto wrong_config;

  if (caps == NULL)
    goto no_caps;

  /* now parse the caps from the config */
  if (!gst_video_info_from_caps (&info, caps))
    goto wrong_caps;

  GST_LOG_OBJECT (pool, "%dx%d, caps %" GST_PTR_FORMAT, info.width, info.height,
      caps);
#ifdef HAVE_WAYLAND_KMS
  if (!gst_buffer_pool_config_get_allocator (config, &wpool->allocator,
                                             &params))
    goto wrong_allocator;

  if (wpool->allocator)
    gst_object_ref (wpool->allocator);
#endif

  /*Fixme: Enable metadata checking handling based on the config of pool */

  wpool->caps = gst_caps_ref (caps);
  wpool->info = info;
  wpool->width = info.width;
  wpool->height = info.height;

  return GST_BUFFER_POOL_CLASS (parent_class)->set_config (pool, config);
  /* ERRORS */
wrong_allocator:
  {
    GST_WARNING_OBJECT (pool, "no allocator");
    return FALSE;
  }
wrong_config:
  {
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }
no_caps:
  {
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }
wrong_caps:
  {
    GST_WARNING_OBJECT (pool,
        "failed getting geometry from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
}

static struct wl_shm_pool *
make_shm_pool (struct display *display, int size, void **data)
{
  struct wl_shm_pool *pool;
  int fd;
  char filename[1024];
  static int init = 0;

  snprintf (filename, 256, "%s-%d-%s", "/tmp/wayland-shm", init++, "XXXXXX");

  fd = mkstemp (filename);
  if (fd < 0) {
    GST_ERROR ("open %s failed:", filename);
    return NULL;
  }
  if (ftruncate (fd, size) < 0) {
    GST_ERROR ("ftruncate failed:..!");
    close (fd);
    return NULL;
  }

  *data = mmap (NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (*data == MAP_FAILED) {
    GST_ERROR ("mmap failed: ");
    close (fd);
    return NULL;
  }

  pool = wl_shm_create_pool (display->shm, fd, size);

  close (fd);

  return pool;
}

static struct shm_pool *
shm_pool_create (struct display *display, size_t size)
{
  struct shm_pool *pool = malloc (sizeof *pool);

  if (!pool)
    return NULL;

  pool->pool = make_shm_pool (display, size, &pool->data);
  if (!pool->pool) {
    free (pool);
    return NULL;
  }

  pool->size = size;
  pool->used = 0;

  return pool;
}

static void *
shm_pool_allocate (struct shm_pool *pool, size_t size, int *offset)
{
  if (pool->used + size > pool->size)
    return NULL;

  *offset = pool->used;
  pool->used += size;

  return (char *) pool->data + *offset;
}

/* Start allocating from the beginning of the pool again */
static void
shm_pool_reset (struct shm_pool *pool)
{
  pool->used = 0;
}

static GstWlMeta *
gst_buffer_add_wayland_meta (GstBuffer * buffer, GstWaylandBufferPool * wpool)
{
  GstWlMeta *wmeta;
  GstWaylandSink *sink;
  void *data;
  gint offset;
  guint stride = 0;
  guint size = 0;

  sink = wpool->sink;
  stride = wpool->width * 4;
  size = stride * wpool->height;

  wmeta = (GstWlMeta *) gst_buffer_add_meta (buffer, GST_WL_META_INFO, NULL);
  wmeta->sink = gst_object_ref (sink);

  /*Fixme: size calculation should be more grcefull, have to consider the padding */
  if (!sink->shm_pool) {
    sink->shm_pool = shm_pool_create (sink->display, size * 15);
    shm_pool_reset (sink->shm_pool);
  }

  if (!sink->shm_pool) {
    GST_ERROR ("Failed to create shm_pool");
    return NULL;
  }

  data = shm_pool_allocate (sink->shm_pool, size, &offset);
  if (!data)
    return NULL;

  wmeta->wbuffer = wl_shm_pool_create_buffer (sink->shm_pool->pool, offset,
      sink->video_width, sink->video_height, stride, sink->format);

  wmeta->data = data;
  wmeta->size = size;

  gst_buffer_append_memory (buffer,
      gst_memory_new_wrapped (GST_MEMORY_FLAG_NO_SHARE, data,
          size, 0, size, NULL, NULL));

  return wmeta;
}

#ifdef HAVE_WAYLAND_KMS
static GstWlMeta *
gst_buffer_add_wayland_meta_kms (GstBuffer * buffer,
    GstWaylandBufferPool * wpool)
{
  GstWlMeta *wmeta;
  GstWaylandSink *sink;
  gsize offset[GST_VIDEO_MAX_PLANES] = { 0 };
  gint stride[GST_VIDEO_MAX_PLANES] = { 0 };
  gint err;
  void *data = NULL;
  guint32 handle;
  gint dmabuf_fd;
  unsigned attr[] = {
    KMS_BO_TYPE, KMS_BO_TYPE_SCANOUT_X8R8G8B8,
    KMS_WIDTH, 0,
    KMS_HEIGHT, 0,
    KMS_TERMINATE_PROP_LIST
  };

  sink = wpool->sink;

  attr[3] = ((wpool->width + 31) >> 5) << 5;
  attr[5] = wpool->height;

  wmeta = (GstWlMeta *) gst_buffer_add_meta (buffer, GST_WL_META_INFO, NULL);
  wmeta->sink = gst_object_ref (sink);

  err = kms_bo_create (wpool->kms, attr, &wmeta->kms_bo);
  if (err) {
    GST_ERROR ("Failed to create kms bo");
    return NULL;
  }

  kms_bo_get_prop (wmeta->kms_bo, KMS_PITCH, (guint *) & stride[0]);

  wmeta->size = stride[0] * wpool->height;

  kms_bo_get_prop (wmeta->kms_bo, KMS_HANDLE, &handle);

  err = drmPrimeHandleToFD (sink->display->drm_fd, handle, DRM_CLOEXEC,
      &dmabuf_fd);
  if (err) {
    GST_ERROR ("drmPrimeHandleToFD failed. %s\n", strerror (errno));
    return NULL;
  }

  wmeta->wbuffer = wl_kms_create_buffer (sink->display->wl_kms, dmabuf_fd,
      wpool->width, wpool->height, stride[0], WL_KMS_FORMAT_ARGB8888, 0);

  if (wpool->allocator &&
      g_strcmp0 (wpool->allocator->mem_type, GST_ALLOCATOR_DMABUF) == 0) {
    gst_buffer_append_memory (buffer,
        gst_dmabuf_allocator_alloc (wpool->allocator, dmabuf_fd, wmeta->size));

    wmeta->data = NULL;
  } else {
    err = kms_bo_map (wmeta->kms_bo, &data);
    if (err) {
      GST_ERROR ("Failed to map kms bo");
      return NULL;
    }

    wmeta->data = data;

    gst_buffer_append_memory (buffer,
        gst_memory_new_wrapped (GST_MEMORY_FLAG_NO_SHARE, data,
            wmeta->size, 0, wmeta->size, NULL, NULL));
  }

  gst_buffer_add_video_meta_full (buffer, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_FORMAT_BGRA, (int) wpool->width, (int) wpool->height, 1, offset,
      stride);

  return wmeta;
}
#endif /* HAVE_WAYLAND_KMS */

static GstFlowReturn
wayland_buffer_pool_alloc (GstBufferPool * pool, GstBuffer ** buffer,
    GstBufferPoolAcquireParams * params)
{
  GstWaylandBufferPool *w_pool = GST_WAYLAND_BUFFER_POOL_CAST (pool);
  GstBuffer *w_buffer;
  GstWlMeta *meta;

  w_buffer = gst_buffer_new ();
#ifdef HAVE_WAYLAND_KMS
  if (w_pool->sink->display->drm_fd >= 0)
    meta = gst_buffer_add_wayland_meta_kms (w_buffer, w_pool);
  else
    meta = gst_buffer_add_wayland_meta (w_buffer, w_pool);
#else
  meta = gst_buffer_add_wayland_meta (w_buffer, w_pool);
#endif
  if (meta == NULL) {
    gst_buffer_unref (w_buffer);
    goto no_buffer;
  }
  *buffer = w_buffer;

  return GST_FLOW_OK;

  /* ERROR */
no_buffer:
  {
    GST_WARNING_OBJECT (pool, "can't create buffer");
    return GST_FLOW_ERROR;
  }
}

GstBufferPool *
gst_wayland_buffer_pool_new (GstWaylandSink * waylandsink)
{
  GstWaylandBufferPool *pool;

  g_return_val_if_fail (GST_IS_WAYLAND_SINK (waylandsink), NULL);
  pool = g_object_new (GST_TYPE_WAYLAND_BUFFER_POOL, NULL);
  pool->sink = gst_object_ref (waylandsink);

#ifdef HAVE_WAYLAND_KMS
  if (kms_create (pool->sink->display->drm_fd, &pool->kms)) {
    GST_WARNING_OBJECT (pool, "kms_create failed");
    return NULL;
  }
#endif

  return GST_BUFFER_POOL_CAST (pool);
}

static void
gst_wayland_buffer_pool_class_init (GstWaylandBufferPoolClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  gobject_class->finalize = gst_wayland_buffer_pool_finalize;

  gstbufferpool_class->set_config = wayland_buffer_pool_set_config;
  gstbufferpool_class->alloc_buffer = wayland_buffer_pool_alloc;
}

static void
gst_wayland_buffer_pool_init (GstWaylandBufferPool * pool)
{
  pool->kms = NULL;
}

static void
gst_wayland_buffer_pool_finalize (GObject * object)
{
  GstWaylandBufferPool *pool = GST_WAYLAND_BUFFER_POOL_CAST (object);

#ifdef HAVE_WAYLAND_KMS
  if (pool->allocator)
    gst_object_unref (pool->allocator);
  pool->allocator = NULL;

  if (pool->kms)
    kms_destroy (&pool->kms);
#endif

  gst_object_unref (pool->sink);

  G_OBJECT_CLASS (gst_wayland_buffer_pool_parent_class)->finalize (object);
}
