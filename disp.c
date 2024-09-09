/*
SPDX-License-Identifier: MPL-2.0
SPDX-FileCopyrightText: 2023 Martin Cerveny <martin@c-home.cz>
*/

#include <stdint.h>
#include <errno.h>
#include <strings.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>
#include <signal.h>
#include <limits.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>

#include "globals.h"
#include "disp.h"

#undef DBG
#define DBG(...)

enum supported_eotf_type
{
    TRADITIONAL_GAMMA_SDR = 0,
    TRADITIONAL_GAMMA_HDR,
    SMPTE_ST2084,
    HLG,
    FUTURE_EOTF
};

enum drm_hdmi_output_type
{
    DRM_HDMI_OUTPUT_DEFAULT_RGB,
    DRM_HDMI_OUTPUT_YCBCR444,
    DRM_HDMI_OUTPUT_YCBCR422,
    DRM_HDMI_OUTPUT_YCBCR420,
    DRM_HDMI_OUTPUT_YCBCR_HQ,
    DRM_HDMI_OUTPUT_YCBCR_LQ,
    DRM_HDMI_OUTPUT_INVALID,
};

enum drm_color_encoding
{
    DRM_COLOR_YCBCR_BT601,
    DRM_COLOR_YCBCR_BT709,
    DRM_COLOR_YCBCR_BT2020,
    DRM_COLOR_ENCODING_MAX,
};

enum drm_color_range
{
    DRM_COLOR_YCBCR_LIMITED_RANGE,
    DRM_COLOR_YCBCR_FULL_RANGE,
    DRM_COLOR_RANGE_MAX,
};

typedef struct hdr_static_metadata
{
    uint16_t eotf;
    uint16_t type;
    uint16_t display_primaries_x[3];
    uint16_t display_primaries_y[3];
    uint16_t white_point_x;
    uint16_t white_point_y;
    uint16_t max_mastering_display_luminance;
    uint16_t min_mastering_display_luminance;
    uint16_t max_fall;
    uint16_t max_cll;
    uint16_t min_cll;
} hdr_static_metadata;

typedef struct plane
{
    uint32_t plane_id;
    drmModePropertyPtr plane_props[32];
    drmModeAtomicReqPtr request;
    uint32_t format, width, height, offsets[DISP_MAX_PLANES], pitches[DISP_MAX_PLANES], zpos;
    uint32_t last_fb_id;
    uint32_t s_x, s_y, s_width, s_height, s_fb_x, s_fb_y, s_fb_width, s_fb_height;

    struct
    {
        int prime_fd;
        uint32_t fb_id;
    } frame_to_drm[DISP_PICTURE_HANDLES];
} plane_t;

struct
{
    int fd;
    pthread_mutex_t mutex;

    uint32_t crtc_id, connector_id;
    drmModePropertyPtr connector_props[16];

    plane_t planes[4];

    plane_t *vi_plane, *ui_plane;

    hdr_static_metadata hdr_panel;
    int frm_eos;

    int crtc_width;
    int crtc_height;
} disp;

void disp_setup(char *cmd_param, plane_t **vi, plane_t **ui, uint32_t *crtc_width, uint32_t *crtc_height)
{
    int i, j;
    uint64_t val;

    A(vi && ui);

    CAZ(pthread_mutex_init(&disp.mutex, NULL));

    disp.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    A(disp.fd >= 0);

    CAZ(drmSetClientCap(disp.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)); // overlays, primary, cursor
    CAZ(drmSetClientCap(disp.fd, DRM_CLIENT_CAP_ATOMIC, 1));           // atomic properties

    drmModeRes *resources;
    CAVNZ(resources, drmModeGetResources(disp.fd));

    CAZ(drmGetCap(disp.fd, DRM_CAP_PRIME, &val));
    A(val == (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT));

    // find active monitor
    drmModeConnector *connector;
    for (i = 0; i < resources->count_connectors; ++i)
    {
        connector = drmModeGetConnector(disp.fd, resources->connectors[i]);
        if (!connector)
            continue;
        if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0)
        {
            LOG("CONNECTOR type: %d id: %d\n", connector->connector_type, connector->connector_id);
            break;
        }
        drmModeFreeConnector(connector);
    }
    A(i < resources->count_connectors);
    disp.connector_id = connector->connector_id;

    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(disp.fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    A(props);
    A(props->count_props < (sizeof(disp.connector_props) / sizeof(disp.connector_props[0])));
    for (i = 0; i < props->count_props; i++)
    {
        drmModePropertyPtr prop = drmModeGetProperty(disp.fd, props->props[i]);
        if (!prop)
            continue;
        LOG("CONN PROP: %s=%" PRIu64, prop->name, props->prop_values[i]);
        if (drm_property_type_is(prop, DRM_MODE_PROP_ENUM))
        {
            LOGC(" (");
            for (j = 0; j < prop->count_enums; j++)
                LOGC(" %s=%llu", prop->enums[j].name, prop->enums[j].value);
            LOGC(" )\n");
        }
        else if (drm_property_type_is(prop, DRM_MODE_PROP_RANGE))
        {
            LOGC(" [");
            for (j = 0; j < prop->count_values; j++)
                LOGC(" %" PRIu64, prop->values[j]);
            LOGC(" ]\n");
        }
        else
            LOGC("\n");
        if (!strcasecmp(prop->name, "HDR_PANEL_METADATA"))
        {
            A(drm_property_type_is(prop, DRM_MODE_PROP_BLOB));
            drmModePropertyBlobPtr hdr_panel_metadata = drmModeGetPropertyBlob(disp.fd, props->prop_values[i]);
            A(hdr_panel_metadata);
            A(hdr_panel_metadata->length == sizeof(hdr_static_metadata));
            disp.hdr_panel = *((hdr_static_metadata *)hdr_panel_metadata->data);
            LOG("HDR CONNECTOR EOTF 0x%x\n", disp.hdr_panel.eotf);
        }
        disp.connector_props[i] = prop;
    }
    drmModeFreeObjectProperties(props);

    drmModeEncoder *encoder;
    for (i = 0; i < resources->count_encoders; ++i)
    {
        encoder = drmModeGetEncoder(disp.fd, resources->encoders[i]);
        if (!encoder)
            continue;
        if (encoder->encoder_id == connector->encoder_id)
        {
            LOG("ENCODER id: %d\n", encoder->encoder_id);
            break;
        }
        drmModeFreeEncoder(encoder);
    }
    A(i < resources->count_encoders);

    drmModeCrtcPtr crtc = NULL;
    for (i = 0; i < resources->count_crtcs; ++i)
    {
        if (resources->crtcs[i] == encoder->crtc_id)
        {
            crtc = drmModeGetCrtc(disp.fd, resources->crtcs[i]);
            A(crtc);
            break;
        }
    }
    A(i < resources->count_crtcs && crtc);
    disp.crtc_id = crtc->crtc_id;
    disp.crtc_width = crtc->width;
    disp.crtc_height = crtc->height;
    uint32_t crtc_bit = (1 << i);

    drmModePlaneRes *plane_resources;
    CAVNZ(plane_resources, drmModeGetPlaneResources(disp.fd));
    A(plane_resources);

    drmModePlane *plane;
    // search for OVERLAY (for active conector, unused, NV12 support)
    for (i = 0; i < plane_resources->count_planes && (!disp.vi_plane || !disp.ui_plane); i++)
    {
        plane = drmModeGetPlane(disp.fd, plane_resources->planes[i]);
        if (!plane)
            continue;
        LOG("PLID: %d\n", plane->plane_id);

        if (plane->possible_crtcs & crtc_bit)
        {
            props = drmModeObjectGetProperties(disp.fd, plane_resources->planes[i], DRM_MODE_OBJECT_PLANE);
            if (!props)
                continue;
            A(props->count_props < (sizeof(disp.planes[0].plane_props) / sizeof(disp.planes[0].plane_props[0])));
            disp.planes[i].plane_id = plane->plane_id;
            uint64_t type = ~0ull;
            for (j = 0; j < props->count_props; j++)
            {
                drmModePropertyPtr prop = drmModeGetProperty(disp.fd, props->props[j]);
                if (!prop)
                    continue;
                DBG("PLANE PROP: %s=%" PRIu64 "\n", prop->name, props->prop_values[j]);
                if (!strcmp(prop->name, "type"))
                    type = props->prop_values[j];
                disp.planes[i].plane_props[j] = prop;
            }
            drmModeFreeObjectProperties(props);

            if (type == DRM_PLANE_TYPE_OVERLAY)
            {
                if (!disp.vi_plane)
                {
                    for (j = 0; j < plane->count_formats; j++)
                        if (plane->formats[j] == DRM_FORMAT_NV12)
                        {
                            disp.vi_plane = &disp.planes[i];
                            break;
                        }
                }
                if (!disp.ui_plane && disp.vi_plane != &disp.planes[i])
                {
                    for (j = 0; j < plane->count_formats; j++)
                        if (plane->formats[j] == DRM_FORMAT_RGBA8888)
                        {
                            disp.ui_plane = &disp.planes[i];
                            break;
                        }
                }
            }
        }
        drmModeFreePlane(plane);
    }
    A(disp.vi_plane && disp.ui_plane);

    CAVNZ(disp.vi_plane->request, drmModeAtomicAlloc());
    CAVNZ(disp.ui_plane->request, drmModeAtomicAlloc());

    *ui = disp.ui_plane;
    *vi = disp.vi_plane;
    *crtc_width = disp.crtc_width;
    *crtc_height = disp.crtc_height;

    LOG("PLANE VI %d UI %d\n", disp.vi_plane->plane_id, disp.ui_plane->plane_id);
}

void disp_cleanup(void)
{
}

static int drm_object_add_property(drmModeAtomicReq *request, uint32_t id, drmModePropertyPtr *props, char *name, uint64_t value)
{
    while (*props)
    {
        if (!strcasecmp(name, (*props)->name))
        {
            return drmModeAtomicAddProperty(request, id, (*props)->prop_id, value);
        }
        props++;
    }
    DBG("ignore bad DRM roperty %s\n", name);
    return INT_MAX;
}

void disp_plane_create(plane_t *plane, uint32_t format, uint32_t width, uint32_t height, uint32_t bpp, int *prime_fd, uint32_t *pitch, uint32_t *size, uint32_t **map)
{
    struct drm_mode_create_dumb create_req;
    struct drm_mode_map_dumb map_req;

    memset(&create_req, 0, sizeof(struct drm_mode_create_dumb));
    create_req.width = width;
    create_req.height = height;
    create_req.bpp = bpp;

    CAZ(drmIoctl(disp.fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_req));
    if (pitch)
        *pitch = create_req.pitch;
    if (size)
        *size = create_req.size;

    if (prime_fd)
        CAZ(drmPrimeHandleToFD(disp.fd, create_req.handle, DRM_CLOEXEC | DRM_RDWR, prime_fd));
    if (map)
    {
        memset(&map_req, 0, sizeof(struct drm_mode_map_dumb));
        map_req.handle = create_req.handle;
        CAZ(drmIoctl(disp.fd, DRM_IOCTL_MODE_MAP_DUMB, &map_req));
        CAV(*map, mmap(0, create_req.size, PROT_READ | PROT_WRITE, MAP_SHARED, disp.fd, map_req.offset), != MAP_FAILED);
    }
}

void disp_plane_setup(plane_t *plane, uint32_t format, uint32_t width, uint32_t height, uint32_t pitches[DISP_MAX_PLANES], uint32_t offsets[DISP_MAX_PLANES], uint32_t zpos)
{
    CAZ(pthread_mutex_lock(&disp.mutex));

    if (plane->format == format && plane->width == width && plane->height == height)
        goto not_changed;
    else
        A(!plane->format);
    drmModeAtomicSetCursor(plane->request, 0);
    CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "FB_ID", 0));
    CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_ID", 0));
    CAZ(drmModeAtomicCommit(disp.fd, plane->request, DRM_MODE_ATOMIC_NONBLOCK, NULL));

    plane->format = format;
    plane->width = width;
    plane->height = height;
    plane->zpos = zpos;
    memcpy(plane->offsets, offsets, sizeof(plane->offsets));
    memcpy(plane->pitches, pitches, sizeof(plane->pitches));

not_changed:
    CAZ(pthread_mutex_unlock(&disp.mutex));
}

void disp_plane_hide(plane_t *plane)
{
    CAZ(pthread_mutex_lock(&disp.mutex));

    drmModeAtomicSetCursor(plane->request, 0);
    CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "FB_ID", 0));
    CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_ID", 0));
    CAZ(drmModeAtomicCommit(disp.fd, plane->request, DRM_MODE_ATOMIC_NONBLOCK, NULL));

    plane->last_fb_id = 0;

    CAZ(pthread_mutex_unlock(&disp.mutex));
}

void disp_plane_scale(plane_t *plane, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t fb_x, uint32_t fb_y, uint32_t fb_width, uint32_t fb_height)
{
    DBG("SC %ux%u-%u-%u %ux%u-%u-%u\n", width, height, x, y, fb_width, fb_height, fb_x, fb_y);

    plane->s_x = x;
    plane->s_y = y;
    plane->s_width = width;
    plane->s_height = height;
    plane->s_fb_x = fb_x;
    plane->s_fb_y = fb_y;
    plane->s_fb_width = fb_width;
    plane->s_fb_height = fb_height;
    if (plane->last_fb_id)
    {
        CAZ(pthread_mutex_lock(&disp.mutex));

        drmModeAtomicSetCursor(plane->request, 0);
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "SRC_X", x << 16));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "SRC_Y", y << 16));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "SRC_W", width << 16));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "SRC_H", height << 16));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_X", fb_x));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_Y", fb_y));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_W", fb_width));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_H", fb_height));
        CAZ(drmModeAtomicCommit(disp.fd, plane->request, 0, NULL));

        CAZ(pthread_mutex_unlock(&disp.mutex));
    }
}

void disp_plane_show_pic(plane_t *plane, uint32_t prime_fd)
{
    CAZ(pthread_mutex_lock(&disp.mutex));
    int id;

    for (id = 0; id < DISP_PICTURE_HANDLES; id++)
    {
        if (prime_fd == plane->frame_to_drm[id].prime_fd)
            break;
    }

    if (id == DISP_PICTURE_HANDLES)
    {
        for (id = 0; id < DISP_PICTURE_HANDLES; id++)
            if (!plane->frame_to_drm[id].prime_fd)
                break;
        A(id < DISP_PICTURE_HANDLES);
        plane->frame_to_drm[id].prime_fd = prime_fd;

        uint32_t handle;
        CAZ(drmPrimeFDToHandle(disp.fd, prime_fd, &handle));

        uint32_t handles[DISP_MAX_PLANES];
        for (int j = 0; j < DISP_MAX_PLANES; j++)
            handles[j] = handle;

        CAZ(drmModeAddFB2(disp.fd, plane->width, plane->height, plane->format, handles, plane->pitches, plane->offsets, &plane->frame_to_drm[id].fb_id, 0));

        DBG("ID[%d] %d\n", id, prime_fd, plane->frame_to_drm[id].fb_id);
    }

    if (plane->last_fb_id)
    {
        drmModeAtomicSetCursor(plane->request, 0);
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "FB_ID", plane->frame_to_drm[id].fb_id));
        CAZ(drmModeAtomicCommit(disp.fd, plane->request, 0, NULL));
    }
    else
    {
        drmModeAtomicSetCursor(plane->request, 0);
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "FB_ID", plane->frame_to_drm[id].fb_id));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_ID", disp.crtc_id));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "SRC_X", plane->s_x << 16));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "SRC_Y", plane->s_y << 16));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "SRC_W", plane->s_width << 16));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "SRC_H", plane->s_height << 16));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_X", plane->s_fb_x));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_Y", plane->s_fb_y));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_W", plane->s_fb_width));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "CRTC_H", plane->s_fb_height));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "ZPOS", plane->zpos));
        // CAP(drm_object_add_property(plane->request, 37, plane->plane_props, "alpha", 10000));
        // CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "COLOR_ENCODING", DRM_COLOR_YCBCR_BT709));
        CAP(drm_object_add_property(plane->request, plane->plane_id, plane->plane_props, "COLOR_RANGE", DRM_COLOR_YCBCR_FULL_RANGE));
        CAZ(drmModeAtomicCommit(disp.fd, plane->request, 0, NULL));
    }

    plane->last_fb_id = plane->frame_to_drm[id].fb_id;

    CAZ(pthread_mutex_unlock(&disp.mutex));
}

void disp_plane_drop_pic(plane_t *plane, uint32_t prime_fd)
{
    CAZ(pthread_mutex_lock(&disp.mutex));
    int id, fbs = 0;

    for (id = 0; id < DISP_PICTURE_HANDLES; id++)
    {
        if (!prime_fd || prime_fd == plane->frame_to_drm[id].prime_fd)
        {
            if (plane->last_fb_id == plane->frame_to_drm[id].fb_id)
                plane->last_fb_id = 0;
            CAZ(drmModeRmFB(disp.fd, plane->frame_to_drm[id].fb_id));
            plane->frame_to_drm[id].fb_id = 0;
            plane->frame_to_drm[id].prime_fd = 0;
        }
        if (plane->frame_to_drm[id].fb_id)
            fbs++;
    }

    if (id < DISP_PICTURE_HANDLES)
        DBG("DISP: prime_fd not registered %d\n", prime_fd);
    CAZ(pthread_mutex_unlock(&disp.mutex));
}

void disp_wait(void)
{
    drm_wait_vblank_t wait_vblank;
    wait_vblank.request.type = _DRM_VBLANK_RELATIVE;
    wait_vblank.request.sequence = 1;
    wait_vblank.request.signal = 0;

    CAZ(drmIoctl(disp.fd, DRM_IOCTL_MODE_CREATE_DUMB, &wait_vblank));
}
