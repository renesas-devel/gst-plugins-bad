/* GStreamer Wayland buffer pool
 * Copyright (C) 2012 Intel Corporation
 * Copyright (C) 2012 Sreerenj Balachandran <sreerenj.balachandran@intel.com>
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

#ifndef __GST_WAYLAND_BUFFER_POOL_H__
#define __GST_WAYLAND_BUFFER_POOL_H__

G_BEGIN_DECLS

#include "gstwaylandsink.h"
#ifdef HAVE_WAYLAND_KMS
#include "drm.h"
#include "libkms.h"
#include <xf86drm.h>
#endif
typedef struct _GstWlMeta GstWlMeta;

typedef struct _GstWaylandBufferPool GstWaylandBufferPool;
typedef struct _GstWaylandBufferPoolClass GstWaylandBufferPoolClass;

GType gst_wl_meta_api_get_type (void);
#define GST_WL_META_API_TYPE  (gst_wl_meta_api_get_type())
const GstMetaInfo * gst_wl_meta_get_info (void);
#define GST_WL_META_INFO  (gst_wl_meta_get_info())

#define gst_buffer_get_wl_meta(b) ((GstWlMeta*)gst_buffer_get_meta((b),GST_WL_META_API_TYPE))

#ifdef HAVE_WAYLAND_KMS
GstBuffer * gst_wayland_buffer_pool_create_buffer_from_dmabuf (
    GstWaylandBufferPool * wpool, gint dmabuf[GST_VIDEO_MAX_PLANES],
    GstAllocator *allocator, gint width, gint height,
    gint in_stride[GST_VIDEO_MAX_PLANES], GstVideoFormat format, gint n_planes);
#endif

#define GST_WAYLAND_BUFFER_POOL_NUM 3

struct _GstWlMeta {
  GstMeta meta;

  GstWaylandSink *sink;

  struct wl_buffer *wbuffer;
  void *data;
  size_t size;
#ifdef HAVE_WAYLAND_KMS
  struct kms_bo *kms_bo;
#endif
};

/* buffer pool functions */
#define GST_TYPE_WAYLAND_BUFFER_POOL      (gst_wayland_buffer_pool_get_type())
#define GST_IS_WAYLAND_BUFFER_POOL(obj)   (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_WAYLAND_BUFFER_POOL))
#define GST_WAYLAND_BUFFER_POOL(obj)      (G_TYPE_CHECK_INSTANCE_CAST ((obj), GST_TYPE_WAYLAND_BUFFER_POOL, GstWaylandBufferPool))
#define GST_WAYLAND_BUFFER_POOL_CAST(obj) ((GstWaylandBufferPool*)(obj))

struct _GstWaylandBufferPool
{
  GstBufferPool bufferpool;

  GstWaylandSink *sink;

  /*Fixme: keep all these in GstWaylandBufferPoolPrivate*/
  GstCaps *caps;
  GstVideoInfo info;
  guint width;
  guint height;

#ifdef HAVE_WAYLAND_KMS
  struct kms_driver *kms;
  GstAllocator *allocator;
#endif
};

struct _GstWaylandBufferPoolClass
{
  GstBufferPoolClass parent_class;
};

GType gst_wayland_buffer_pool_get_type (void);

GstBufferPool *gst_wayland_buffer_pool_new (GstWaylandSink * waylandsink);

G_END_DECLS

#endif /*__GST_WAYLAND_BUFFER_POOL_H__*/
