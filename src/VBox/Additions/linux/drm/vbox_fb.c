/* $Id$ */
/** @file
 * VirtualBox Additions Linux kernel video driver
 */

/*
 * Copyright (C) 2013 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 * --------------------------------------------------------------------
 *
 * This code is based on
 * ast_fb.c
 * with the following copyright and permission notice:
 *
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 */
/*
 * Authors: Dave Airlie <airlied@redhat.com>
 */
/* Include from most specific to most general to be able to override things. */
#include "vbox_drv.h"
#include <VBox/VBoxVideo.h>
#include <VBox/VMMDev.h>

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/tty.h>
#include <linux/sysrq.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/init.h>


#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_helper.h>
#include "vbox_drv.h"

/** This is called whenever there is a virtual console switch.  We do two things
 * here.  First we re-set all video modes in case the last console owner
 * programmed the card directly.  Second we disable VBVA in case the new console
 * owner is about to programme the card directly and doesn't know about VBVA.
 * We re-enable VBVA if necessary when we get dirty rectangle information, as
 * the owner should not be sending that if they plan to programme the card
 * themselves.  Update: we also do the same for reporting hot-plug support. I
 * wonder whether we should allow it at all on the console. */
static int VBoxSetPar(struct fb_info *pInfo)
{
    struct vbox_fbdev *pVFBDev = pInfo->par;
    struct drm_device *pDev;
    struct vbox_private *pVBox;
    unsigned i;

    LogFunc(("vboxvideo: %d\n", __LINE__));
    pDev = pVFBDev->helper.dev;
    pVBox = pDev->dev_private;
    VBoxRefreshModes(pDev);
    for (i = 0; i < pVBox->cCrtcs; ++i)
        VBoxVBVADisable(&pVBox->paVBVACtx[i], &pVBox->Ctx, i);
    VBoxHGSMISendCapsInfo(&pVBox->Ctx, 0);
    return drm_fb_helper_set_par(pInfo);
}

/**
 * Tell the host about dirty rectangles to update.
 */
static void vbox_dirty_update(struct vbox_fbdev *afbdev,
                 int x, int y, int width, int height)
{
    struct drm_device *dev = afbdev->helper.dev;
    struct vbox_private *vbox = dev->dev_private;
    int i;

    struct drm_gem_object *obj;
    struct vbox_bo *bo;
    int src_offset, dst_offset;
    int bpp = (afbdev->afb.base.bits_per_pixel + 7)/8;
    int ret;
    bool unmap = false;
    bool store_for_later = false;
    int x2, y2;
    unsigned long flags;
    struct drm_clip_rect rect;

    LogFunc(("vboxvideo: %d\n", __LINE__));
    obj = afbdev->afb.obj;
    bo = gem_to_vbox_bo(obj);

    /*
     * try and reserve the BO, if we fail with busy
     * then the BO is being moved and we should
     * store up the damage until later.
     */
    ret = vbox_bo_reserve(bo, true);
    if (ret)
    {
        if (ret != -EBUSY)
            return;

        store_for_later = true;
    }

    x2 = x + width - 1;
    y2 = y + height - 1;
    spin_lock_irqsave(&vbox->dev_lock, flags);

    if (afbdev->y1 < y)
        y = afbdev->y1;
    if (afbdev->y2 > y2)
        y2 = afbdev->y2;
    if (afbdev->x1 < x)
        x = afbdev->x1;
    if (afbdev->x2 > x2)
        x2 = afbdev->x2;

    if (store_for_later)
    {
        afbdev->x1 = x;
        afbdev->x2 = x2;
        afbdev->y1 = y;
        afbdev->y2 = y2;
        spin_unlock_irqrestore(&vbox->dev_lock, flags);
        LogFunc(("vboxvideo: %d\n", __LINE__));
        return;
    }

    afbdev->x1 = afbdev->y1 = INT_MAX;
    afbdev->x2 = afbdev->y2 = 0;
    spin_unlock_irqrestore(&vbox->dev_lock, flags);

    if (!bo->kmap.virtual)
    {
        ret = ttm_bo_kmap(&bo->bo, 0, bo->bo.num_pages, &bo->kmap);
        if (ret)
        {
            DRM_ERROR("failed to kmap fb updates\n");
            vbox_bo_unreserve(bo);
            return;
        }
        unmap = true;
    }
    for (i = y; i <= y2; i++)
    {
        /* assume equal stride for now */
        src_offset = dst_offset = i * afbdev->afb.base.pitches[0] + (x * bpp);
        memcpy_toio(bo->kmap.virtual + src_offset, (char *)afbdev->sysram + src_offset, (x2 - x + 1) * bpp);
    }
    /* Not sure why the original code subtracted 1 here, but I will keep it that
     * way to avoid unnecessary differences. */
    rect.x1 = x;
    rect.x2 = x2 + 1;
    rect.y1 = y;
    rect.y2 = y2 + 1;
    vbox_framebuffer_dirty_rectangles(&afbdev->afb.base, &rect, 1);
    if (unmap)
        ttm_bo_kunmap(&bo->kmap);

    vbox_bo_unreserve(bo);
    LogFunc(("vboxvideo: %d\n", __LINE__));
}

static void vbox_fillrect(struct fb_info *info,
             const struct fb_fillrect *rect)
{
    struct vbox_fbdev *afbdev = info->par;
    LogFunc(("vboxvideo: %d\n", __LINE__));
    sys_fillrect(info, rect);
    vbox_dirty_update(afbdev, rect->dx, rect->dy, rect->width,
             rect->height);
}

static void vbox_copyarea(struct fb_info *info,
             const struct fb_copyarea *area)
{
    struct vbox_fbdev *afbdev = info->par;
    LogFunc(("vboxvideo: %d\n", __LINE__));
    sys_copyarea(info, area);
    vbox_dirty_update(afbdev, area->dx, area->dy, area->width,
             area->height);
}

static void vbox_imageblit(struct fb_info *info,
              const struct fb_image *image)
{
    struct vbox_fbdev *afbdev = info->par;
    LogFunc(("vboxvideo: %d\n", __LINE__));
    sys_imageblit(info, image);
    vbox_dirty_update(afbdev, image->dx, image->dy, image->width,
             image->height);
}

static struct fb_ops vboxfb_ops =
{
    .owner = THIS_MODULE,
    .fb_check_var = drm_fb_helper_check_var,
    .fb_set_par = VBoxSetPar,
    .fb_fillrect = vbox_fillrect,
    .fb_copyarea = vbox_copyarea,
    .fb_imageblit = vbox_imageblit,
    .fb_pan_display = drm_fb_helper_pan_display,
    .fb_blank = drm_fb_helper_blank,
    .fb_setcmap = drm_fb_helper_setcmap,
    .fb_debug_enter = drm_fb_helper_debug_enter,
    .fb_debug_leave = drm_fb_helper_debug_leave,
};

static int vboxfb_create_object(struct vbox_fbdev *afbdev,
                   struct DRM_MODE_FB_CMD *mode_cmd,
                   struct drm_gem_object **gobj_p)
{
    struct drm_device *dev = afbdev->helper.dev;
    u32 bpp, depth;
    u32 size;
    struct drm_gem_object *gobj;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 3, 0)
    __u32 pitch = mode_cmd->pitch;
#else
    __u32 pitch = mode_cmd->pitches[0];
#endif

    int ret = 0;
    LogFunc(("vboxvideo: %d\n", __LINE__));
    drm_fb_get_bpp_depth(mode_cmd->pixel_format, &depth, &bpp);

    size = pitch * mode_cmd->height;
    ret = vbox_gem_create(dev, size, true, &gobj);
    if (ret)
        return ret;

    *gobj_p = gobj;
    LogFunc(("vboxvideo: %d\n", __LINE__));
    return ret;
}

static int vboxfb_create(struct vbox_fbdev *afbdev,
            struct drm_fb_helper_surface_size *sizes)
{
    struct drm_device *dev = afbdev->helper.dev;
    struct DRM_MODE_FB_CMD mode_cmd;
    struct drm_framebuffer *fb;
    struct fb_info *info;
    __u32 pitch;
    unsigned int fb_pitch;
    int size, ret;
    struct device *device = &dev->pdev->dev;
    void *sysram;
    struct drm_gem_object *gobj = NULL;
    struct vbox_bo *bo = NULL;
    LogFunc(("vboxvideo: %d\n", __LINE__));
    mode_cmd.width = sizes->surface_width;
    mode_cmd.height = sizes->surface_height;
    pitch = mode_cmd.width * ((sizes->surface_bpp + 7) / 8);
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 3, 0)
    mode_cmd.bpp = sizes->surface_bpp;
    mode_cmd.depth = sizes->surface_depth;
    mode_cmd.pitch = pitch;
#else
    mode_cmd.pixel_format = drm_mode_legacy_fb_format(sizes->surface_bpp,
                                                      sizes->surface_depth);
    mode_cmd.pitches[0] = pitch;
#endif

    size = pitch * mode_cmd.height;

    ret = vboxfb_create_object(afbdev, &mode_cmd, &gobj);
    if (ret)
    {
        DRM_ERROR("failed to create fbcon backing object %d\n", ret);
        return ret;
    }
    bo = gem_to_vbox_bo(gobj);

    sysram = vmalloc(size);
    if (!sysram)
        return -ENOMEM;

    info = framebuffer_alloc(0, device);
    if (!info)
    {
        ret = -ENOMEM;
        goto out;
    }
    info->par = afbdev;

    ret = vbox_framebuffer_init(dev, &afbdev->afb, &mode_cmd, gobj);
    if (ret)
        goto out;

    afbdev->sysram = sysram;
    afbdev->size = size;

    fb = &afbdev->afb.base;
    afbdev->helper.fb = fb;
    afbdev->helper.fbdev = info;

    strcpy(info->fix.id, "vboxdrmfb");

    /* The last flag forces a mode set on VT switches even if the kernel does
     * not think it is needed. */
    info->flags =   FBINFO_DEFAULT | FBINFO_CAN_FORCE_OUTPUT
                  | FBINFO_MISC_ALWAYS_SETPAR;
    info->fbops = &vboxfb_ops;

    ret = fb_alloc_cmap(&info->cmap, 256, 0);
    if (ret)
    {
        ret = -ENOMEM;
        goto out;
    }

    /* This seems to be done for safety checking that the framebuffer is not
     * registered twice by different drivers. */
    info->apertures = alloc_apertures(1);
    if (!info->apertures)
    {
        ret = -ENOMEM;
        goto out;
    }
    info->apertures->ranges[0].base = pci_resource_start(dev->pdev, 0);
    info->apertures->ranges[0].size = pci_resource_len(dev->pdev, 0);

    drm_fb_helper_fill_fix(info, fb->pitches[0], fb->depth);
    drm_fb_helper_fill_var(info, &afbdev->helper, sizes->fb_width, sizes->fb_height);

    info->screen_base = sysram;
    info->screen_size = size;

    info->pixmap.flags = FB_PIXMAP_SYSTEM;

    DRM_DEBUG_KMS("allocated %dx%d\n",
              fb->width, fb->height);

    LogFunc(("vboxvideo: %d\n", __LINE__));
    return 0;
out:
    LogFunc(("vboxvideo: %d\n", __LINE__));
    return ret;
}

static void vbox_fb_gamma_set(struct drm_crtc *crtc, u16 red, u16 green,
                   u16 blue, int regno)
{

}

static void vbox_fb_gamma_get(struct drm_crtc *crtc, u16 *red, u16 *green,
                   u16 *blue, int regno)
{
    *red = regno;
    *green = regno;
    *blue = regno;
}

static int vbox_find_or_create_single(struct drm_fb_helper *helper,
                      struct drm_fb_helper_surface_size *sizes)
{
    struct vbox_fbdev *afbdev = (struct vbox_fbdev *)helper;
    int new_fb = 0;
    int ret;

    LogFunc(("vboxvideo: %d\n", __LINE__));
    if (!helper->fb)
    {
        ret = vboxfb_create(afbdev, sizes);
        if (ret)
            return ret;
        new_fb = 1;
    }
    LogFunc(("vboxvideo: %d\n", __LINE__));
    return new_fb;
}

static struct drm_fb_helper_funcs vbox_fb_helper_funcs =
{
    .gamma_set = vbox_fb_gamma_set,
    .gamma_get = vbox_fb_gamma_get,
    .fb_probe = vbox_find_or_create_single,
};

static void vbox_fbdev_destroy(struct drm_device *dev,
                  struct vbox_fbdev *afbdev)
{
    struct fb_info *info;
    struct vbox_framebuffer *afb = &afbdev->afb;
    LogFunc(("vboxvideo: %d\n", __LINE__));
    if (afbdev->helper.fbdev)
    {
        info = afbdev->helper.fbdev;
        unregister_framebuffer(info);
        if (info->cmap.len)
            fb_dealloc_cmap(&info->cmap);
        framebuffer_release(info);
    }

    if (afb->obj)
    {
        drm_gem_object_unreference_unlocked(afb->obj);
        afb->obj = NULL;
    }
    drm_fb_helper_fini(&afbdev->helper);

    vfree(afbdev->sysram);
    drm_framebuffer_cleanup(&afb->base);
    LogFunc(("vboxvideo: %d\n", __LINE__));
}

int vbox_fbdev_init(struct drm_device *dev)
{
    struct vbox_private *vbox = dev->dev_private;
    struct vbox_fbdev *afbdev;
    int ret;

    LogFunc(("vboxvideo: %d\n", __LINE__));
    afbdev = kzalloc(sizeof(struct vbox_fbdev), GFP_KERNEL);
    if (!afbdev)
        return -ENOMEM;

    vbox->fbdev = afbdev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
    afbdev->helper.funcs = &vbox_fb_helper_funcs;
#else
    drm_fb_helper_prepare(dev, &afbdev->helper, &vbox_fb_helper_funcs);
#endif
    ret = drm_fb_helper_init(dev, &afbdev->helper, vbox->cCrtcs, vbox->cCrtcs);
    if (ret)
    {
        kfree(afbdev);
        return ret;
    }

    drm_fb_helper_single_add_all_connectors(&afbdev->helper);
    drm_fb_helper_initial_config(&afbdev->helper, 32);
    LogFunc(("vboxvideo: %d\n", __LINE__));
    return 0;
}

void vbox_fbdev_fini(struct drm_device *dev)
{
    struct vbox_private *vbox = dev->dev_private;

    if (!vbox->fbdev)
        return;

    LogFunc(("vboxvideo: %d\n", __LINE__));
    vbox_fbdev_destroy(dev, vbox->fbdev);
    kfree(vbox->fbdev);
    vbox->fbdev = NULL;
}

void vbox_fbdev_set_suspend(struct drm_device *dev, int state)
{
    struct vbox_private *vbox = dev->dev_private;

    LogFunc(("vboxvideo: %d\n", __LINE__));
    if (!vbox->fbdev)
        return;

    fb_set_suspend(vbox->fbdev->helper.fbdev, state);
}
