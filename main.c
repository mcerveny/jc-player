/*
SPDX-License-Identifier: MPL-2.0
SPDX-FileCopyrightText: 2023 Martin Cerveny <martin@c-home.cz>
*/

#include <stdio.h>
#include <inttypes.h>
#include <math.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <sys/mman.h>
#include <dirent.h>
#include <libgen.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <regex.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext_drm.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <png.h>

#include <jansson.h>
#include <ulfius.h>

#include "globals.h"
#include "hid.h"
#include "disp.h"

#undef DBG
#define DBG(...)

#define LOOP_USLEEP (500 * 1000)

#define STREAM_FRAMES 100
#define STREAM_FPS 25
#define MS_IN_SEC (1000)
#define NS_IN_SEC (1000000000l)
#define NS_IN_MSEC (1000000l)
#define STREAM_FPS_MSEC (MS_IN_SEC / STREAM_FPS)
#define STREAM_FPS_NSEC (NS_IN_SEC / STREAM_FPS)

#define SKIP_START 500 * NS_IN_MSEC
#define SKIP_SPEEDUP 40 * NS_IN_MSEC

#define FRAMES_PRELOAD 20
#define FRAMES_TRESHOLD 26

#define STREAM_MAX_FILES 24000
#define MAX_CAM 4
#define MAX_MAT 8
#define MAX_BROWSE (4 * 8)
#define MAX_BOOKMARKS (5 * 7)
#define MAX_MEDICALS 256

#define MEDICAL_EXTEND 20 // timeframe to extend existing medical start or stop
#define MEDICAL_DELETE 5  // minimum medical duration, otherwise delete
#define BOOKMARK_DELAY 5  // GUI select time

#define SRVF "srv%d"
#define CAMF "cam%02d"

typedef enum
{
    GUI_EMPTY,
    GUI_PLAYER,
    GUI_CONFIG,
    GUI_BROWSE,
    GUI_BOOKMARKS,
} gui_t;

#define MT_FINGERS 2
#define MT_MIN 1
#define MT_MAX 2
#define MT_LONGPRESS 1500

typedef struct mt
{
    int x, y, track;
} mt_t;

typedef enum
{
    SPEED_BCK_SKIP = -9,
    SPEED_BCK_8,
    SPEED_BCK_4,
    SPEED_BCK_2,
    SPEED_BCK_PLAY,
    SPEED_BCK_SLOW_2,
    SPEED_BCK_SLOW_3,
    SPEED_BCK_SLOW_4,
    SPEED_BCK_SLOW_5,
    SPEED_PAUSE,
    SPEED_SLOW_5,
    SPEED_SLOW_4,
    SPEED_SLOW_3,
    SPEED_SLOW_2,
    SPEED_PLAY,
    SPEED_2,
    SPEED_4,
    SPEED_8,
    SPEED_SKIP
} speed_t;

typedef enum
{
    A_NONE,
    A_MOVE,
    A_MAT,
    A_CAM,
    A_SPEED,
    A_RESTORE,
    A_CFG_END,
    A_CFG_SELECT,
    A_CFG_PAUSE,
    A_BRS_SELECT,
    A_BRS_DELETE,
    A_BOOKMARK_ADD,
    A_BOOKMARK_SHOW,
    A_BOOKMARK_SELECT,
    A_BOOKMARK_DELETE,
    A_MEDICAL_START,
    A_MEDICAL_STOP,
} action_t;

struct
{
    uint32_t skip;
    uint64_t wait;
} speed_params[] = {
    {1, STREAM_FPS_NSEC},
    {1, STREAM_FPS_NSEC * 5},
    {1, STREAM_FPS_NSEC * 4},
    {1, STREAM_FPS_NSEC * 3},
    {1, STREAM_FPS_NSEC * 2},
    {1, STREAM_FPS_NSEC},
    {2, STREAM_FPS_NSEC},
    {4, STREAM_FPS_NSEC},
    {8, STREAM_FPS_NSEC},
    {STREAM_FRAMES, SKIP_START}};

int8_t speed_map[] = {-4, -3, -3, -3, -2, -1, -1, -1, -1, 0, 1, 1, 1, 1, 2, 3, 3, 3, 4};

typedef struct
{
    gui_t gui;
    uint32_t x, y, w, h;
    action_t action;
    int param;
} action_rect_t;

typedef struct
{
    uint8_t *b;
    size_t blen;
} bd_t;

typedef struct frame_cache
{
    uint64_t ms;
    uint32_t id;
    AVFrame *frame;
} frame_cache_t;

typedef struct config
{
    uint32_t camid;
    uint32_t mat;
    uint32_t position;
} config_t;

typedef struct chunk
{
    uint64_t ms;
    uint8_t srvid;
} chunk_t;

typedef struct img
{
    uint32_t *map;
    uint32_t width;
    uint32_t height;
} img_t;

struct
{
    int stopping;
    bool switching;
    char path[96];
    char day[4 + 1 + 2 + 1 + 2 + 1], actualday[4 + 1 + 2 + 1 + 2 + 1];
    char masteruri[HOST_NAME_MAX];
    char playerid[4];
    gui_t gui, config_gui;

    pthread_mutex_t rest_mutex;
    struct _u_request rest_cams;

    // file stream
    bool stream_initialized;
    pthread_t loader_tid;
    chunk_t ch[STREAM_MAX_FILES];
    uint32_t chlen;

    // command
    pthread_mutex_t cmd_mutex;
    pthread_cond_t cmd_cond;
    pthread_t cmd_tid;

    // public, cmd_mutex
    speed_t speed;
    uint32_t camid_switch;
    uint32_t camid;

    config_t configs[MAX_CAM * MAX_MAT];
    uint32_t configslen;

    // private

    // display
    bool display_initialized;
    plane_t *vi, *ui;
    uint32_t width, height;
    uint32_t crtc_width, crtc_height;

    // info
    pthread_mutex_t info_mutex;
    pthread_t info_tid;
    // public

    // info_mutex
    struct timespec info_time;
    time_t info_bookmarks[MAX_BOOKMARKS];
    int info_bookmarkslen;
    time_t info_medicals[MAX_MEDICALS]; // tuples start/stop
    int info_medicalslen;
    bool info_updating;
    uint32_t info_loadedmat;

    // private
    struct
    {
        int fd;
        uint32_t pitch;
        uint32_t size;
        uint32_t *map;
    } info_fbs[2];
    FT_Library info_library;
    FT_Face info_face;
    img_t info_timg[12], info_cbimg[5], info_cimg[2], info_simg[5][2], info_rimg[2], info_fimg, info_oimg[9];
    int8_t info_width[-SPEED_BCK_SKIP + SPEED_SKIP + 1];
    bool info_restore_prev;

    char info_browse[MAX_BROWSE][4 + 1 + 2 + 1 + 2 + 1];
    int info_browselen;
    bool info_browse_deleting;
    bool info_changed;
    bool info_config_switching;
    bool info_paused;
    bool info_touch;

    // decoder
    pthread_mutex_t decoder_mutex;
    pthread_cond_t decoder_cond;
    pthread_t decoder_tid;

    // public
    frame_cache_t frm[DISP_PICTURE_HANDLES];
    uint32_t frmlen;

    // private
    AVCodecContext *decoder_ctx;
    AVBufferRef *hw_device_ctx;
    bool decode_read_packet;
    AVIOContext *avio_ctx;
    AVFormatContext *input_ctx;
    int stream_index;

    uint64_t decode_ms;
    uint32_t decode_id;
    bd_t decode_bd;

    bd_t decoder_loader_bd;
    int decoder_loader_fd;
    uint64_t decoder_loader_ms;

    // show
    pthread_t show_tid;
    uint32_t show_skip; // skip frames (modulo)
    uint64_t show_wait; // inter frame wait in ns

    // private
    uint64_t show_a_ms, show_p_ms;
    uint32_t show_a_id, show_p_id;

    // decoder_mutex
    uint64_t show_ms; // actual show
    uint32_t show_id;

    uint64_t show_msec;
    uint64_t show_msec_seek;

    // touch
    int hid_absX[6];
    int hid_absY[6];
    int hid_absfd;
    mt_t hid_touch[MT_FINGERS];
    int hid_slot;
    bool hid_check;
    float hid_scale_x, hid_scale_y;
    struct timespec hid_pushtime;

    int hid_t_distance, hid_t_x, hid_t_y; // actual touch first finger (xy) and distance to seconf finger (distance)

    pthread_mutex_t scale_mutex;
    pthread_cond_t scale_cond;
    pthread_t scale_tid;
    int hid_x, hid_y, hid_w, hid_h, hid_mx, hid_my, hid_zoom; // actual position, size, maximum xy, zoom

    action_t hid_action;
    int hid_param;
    action_rect_t hid_actions[256];
    int hid_actionslen;

} stream;

#define INFO_WIDTH (stream.crtc_width)
#define INFO_HEIGHT (stream.crtc_height)

#ifndef INFO_DRAW_FINGER
#define INFO_DRAW_FINGER false
#endif

#define INFO_PREFIX "resources/"
#define INFO_FONT INFO_PREFIX "DejaVuSansMono-Bold.ttf"
#define INFO_TIMG INFO_PREFIX "t%d.png"
#define INFO_CIMG INFO_PREFIX "c%d.png"
#define INFO_CBIMG INFO_PREFIX "cb%d.png"
#define INFO_SIMG INFO_PREFIX "s%d%d.png"
#define INFO_RIMG INFO_PREFIX "r%d.png"
#define INFO_FIMG INFO_PREFIX "finger.png"
#define INFO_OIMG INFO_PREFIX "o%d.png"
#define INFO_FONT_LINE 50

#define INFO_CBIMG_NO 0
#define INFO_CBIMG_YES 1
#define INFO_CBIMG_SELECTED 2
#define INFO_CBIMG_PAUSED 3
#define INFO_CBIMG_RECORDING 4

#define INFO_TIMG_SETUP 9
#define INFO_TIMG_BROWSE 10
#define INFO_TIMG_NOTOUCH 11

#define INFO_OIMG_BOOKMARKADD 0
#define INFO_OIMG_BOOKMARKADDSEL 1
#define INFO_OIMG_BOOKMARKSHOW 2
#define INFO_OIMG_BOOKMARKSHOWSEL 3
#define INFO_OIMG_MEDICALSTART 4
#define INFO_OIMG_MEDICALSTARTSEL 5
#define INFO_OIMG_MEDICALSTOP 6
#define INFO_OIMG_MEDICALSTOPSEL 7
#define INFO_OIMG_MEDICAL 8

#define INFO_BORDER 20

#define INFO_TIME_W 308
#define INFO_TIME_H 108
#define INFO_TIME_X (INFO_WIDTH - INFO_TIME_W - INFO_BORDER)
#define INFO_TIME_Y (INFO_BORDER)

#define INFO_MAT_W 100
#define INFO_MAT_H 100
#define INFO_MAT_X (INFO_BORDER)
#define INFO_MAT_Y (INFO_BORDER)

#define INFO_BROWSE_W (400 + 2 * INFO_BORDER)
#define INFO_BROWSE_H (INFO_FONT_LINE + 2 * INFO_BORDER)
#define INFO_BROWSE_X (INFO_BORDER)
#define INFO_BROWSE_Y (INFO_MAT_Y + INFO_MAT_H + 2 * INFO_BORDER)
#define INFO_BROWSE_BG 0xff303030
#define INFO_BROWSE_BGS 0xffffffff
#define INFO_BROWSE_BGD 0xffa00000
#define INFO_BROWSE_FG 0xffffffff
#define INFO_BROWSE_FGS 0xff000000
#define INFO_BROWSE_FGD 0xffff0000

#define INFO_BOOKMARK_W (320 + 2 * INFO_BORDER)
#define INFO_BOOKMARK_H (INFO_FONT_LINE + 2 * INFO_BORDER)
#define INFO_BOOKMARK_X (INFO_BORDER)
#define INFO_BOOKMARK_Y (INFO_MAT_Y + INFO_MAT_H + 2 * INFO_BORDER)
#define INFO_BOOKMARK_BG 0x80303030
#define INFO_BOOKMARK_BGS 0xa0ffffff
#define INFO_BOOKMARK_FG 0x80ffffff
#define INFO_BOOKMARK_FGS 0xa0000000
#define INFO_BOOKMARK_FGD 0xa0ff0000

#define INFO_CONFIG_NEXT_X 240
#define INFO_CONFIG_X (INFO_BORDER + INFO_CONFIG_CAM_W / 2 + INFO_MAT_W / 2)
#define INFO_CONFIG_Y (INFO_HEIGHT - INFO_CONFIG_CAM_H - INFO_MAT_H / 2 - 2 * INFO_BORDER)
#define INFO_CONFIG_CAM_OFF_X 50
#define INFO_CONFIG_CAM_OFF_Y 200
#define INFO_CONFIG_CAM_W 100
#define INFO_CONFIG_CAM_H 100
#define INFO_CONFIG_CAM_FG 0xf0ffffff
#define INFO_CONFIG_CAM_FGS 0xf0000000

#define INFO_PAUSE_W 100
#define INFO_PAUSE_H 100
#define INFO_PAUSE_X (INFO_MAT_X + INFO_MAT_W + INFO_BORDER)
#define INFO_PAUSE_Y (INFO_MAT_Y)

#define INFO_BOTTOM_H 80
#define INFO_BOTTOM_Y (INFO_HEIGHT - INFO_BORDER - INFO_BOTTOM_H)

#define INFO_CAM_W 80
#define INFO_CAM_X (INFO_BORDER)

#define INFO_SPEED_X (INFO_CAM_X + 4 * (INFO_BORDER + INFO_CAM_W) + 2 * INFO_BORDER)
#define INFO_SPEED_BORDER 4

#define INFO_RESTORE_W 80
#define INFO_RESTORE_X (INFO_WIDTH - INFO_BORDER - INFO_RESTORE_W)

#define INFO_ALPHA (0x40 << 24)

#define HID_DIFF 8
#define HID_ZOOM_DIV (200)
#define HID_ZOOM_MAX (600)

void info_cfg_load();

config_t *get_config(uint8_t camid)
{
    int i;
    for (i = 0; i < stream.configslen; i++)
        if (camid == stream.configs[i].camid)
            break;
    if (i == stream.configslen)
        return NULL;
    return &stream.configs[i];
}

uint8_t get_config_cam(uint8_t mat, config_t *cams[MAX_CAM])
{
    int i;
    uint8_t camscnt = 0;
    memset(cams, 0, sizeof(cams[0]) * MAX_CAM);

    for (i = 0; i < stream.configslen; i++)
        if (stream.configs[i].mat == mat)
        {
            assert(stream.configs[i].position >= 1 && stream.configs[i].position <= MAX_CAM);
            cams[stream.configs[i].position - 1] = &stream.configs[i];
            camscnt++;
        }

    return camscnt;
}

void mat_load(uint8_t matid)
{
    int i;
    if (stream.info_updating)
        return;

    // LOG("load mat %d\n", matid);

    struct _u_request request;
    struct _u_response response;
    json_t *json, *jsona;
    char _mat[4];
    CAP(snprintf(_mat, sizeof(_mat), "%u", matid));

    ulfius_init_request(&request);
    ulfius_init_response(&response);
    CAZ(ulfius_set_request_properties(&request,
                                      U_OPT_HTTP_VERB, "GET",
                                      U_OPT_HTTP_URL, stream.masteruri,
                                      U_OPT_HTTP_URL_APPEND, "/mats/",
                                      U_OPT_HTTP_URL_APPEND, stream.day,
                                      U_OPT_HTTP_URL_APPEND, "/",
                                      U_OPT_HTTP_URL_APPEND, _mat,
                                      U_OPT_TIMEOUT, 1ul,
                                      U_OPT_NONE));
    CAZ(ulfius_send_http_request(&request, &response));
    CAVNZ(json, ulfius_get_json_body_response(&response, NULL));
    ulfius_clean_response(&response);
    ulfius_clean_request(&request);

    A(json_is_object(json));

    time_t info_bookmarks[MAX_BOOKMARKS];
    int info_bookmarkslen = 0;
    jsona = json_object_get(json, "bookmarks");
    if (jsona)
    {
        A(json_is_array(jsona));
        info_bookmarkslen = json_array_size(jsona);
        for (i = 0; i < info_bookmarkslen; i++)
            info_bookmarks[i] = json_integer_value(json_array_get(jsona, i));
    }

    time_t info_medicals[MAX_MEDICALS];
    int info_medicalslen = 0;
    jsona = json_object_get(json, "medicals");
    if (jsona)
    {
        A(json_is_array(jsona));
        info_medicalslen = json_array_size(jsona);
        for (i = 0; i < info_medicalslen; i++)
            info_medicals[i] = json_integer_value(json_array_get(jsona, i));
    }

    json_decref(json);

    CAZ(pthread_mutex_lock(&stream.info_mutex));
    if (stream.info_updating)
    {
        CAZ(pthread_mutex_unlock(&stream.info_mutex));
        return;
    }
    stream.info_bookmarkslen = info_bookmarkslen;
    memcpy(stream.info_bookmarks, info_bookmarks, sizeof(info_bookmarks[0]) * info_bookmarkslen);
    stream.info_medicalslen = info_medicalslen;
    memcpy(stream.info_medicals, info_medicals, sizeof(info_medicals[0]) * info_medicalslen);
    stream.info_loadedmat = matid;
    CAZ(pthread_mutex_unlock(&stream.info_mutex));
}

void mat_patch(char *arrayname, time_t *array, int arraylen)
{
    struct _u_request request;
    json_t *json, *jsona;
    char _mat[4];
    CAP(snprintf(_mat, sizeof(_mat), "%u", stream.info_loadedmat));

    CAVNZ(jsona, json_array());
    for (int i = 0; i < arraylen; i++)
        CAZ(json_array_append(jsona, json_integer(array[i])));

    CAZ(pthread_mutex_unlock(&stream.info_mutex));

    CAVNZ(json, json_object());
    CAZ(json_object_set(json, arrayname, jsona));

    ulfius_init_request(&request);
    CAZ(ulfius_set_request_properties(&request,
                                      U_OPT_HTTP_VERB, "PATCH",
                                      U_OPT_HTTP_URL, stream.masteruri,
                                      U_OPT_HTTP_URL_APPEND, "/mats/",
                                      U_OPT_HTTP_URL_APPEND, stream.day,
                                      U_OPT_HTTP_URL_APPEND, "/",
                                      U_OPT_HTTP_URL_APPEND, _mat,
                                      U_OPT_JSON_BODY, json,
                                      U_OPT_TIMEOUT, 20ul,
                                      U_OPT_NONE));
    CAZ(ulfius_send_http_request(&request, NULL));
    ulfius_clean_request(&request);
    json_decref(json);
    CAZ(pthread_mutex_lock(&stream.info_mutex));
}

void get_player_cam()
{
    // with stream.cmd_mutex
    stream.camid_switch = 0;
    stream.speed = SPEED_PLAY;
    stream.info_changed = true;
    info_cfg_load();

    if (!strcmp(stream.day, stream.actualday))
    {
        struct _u_request request;
        struct _u_response response;
        json_t *json;
        ulfius_init_request(&request);
        ulfius_init_response(&response);
        CAZ(ulfius_set_request_properties(&request,
                                          U_OPT_HTTP_VERB, "GET",
                                          U_OPT_HTTP_URL, stream.masteruri,
                                          U_OPT_HTTP_URL_APPEND, "/players",
                                          U_OPT_HTTP_URL_APPEND, "/",
                                          U_OPT_HTTP_URL_APPEND, stream.playerid,
                                          U_OPT_TIMEOUT, 1ul,
                                          U_OPT_NONE));
        CAZ(ulfius_send_http_request(&request, &response));
        CAVNZ(json, ulfius_get_json_body_response(&response, NULL));
        ulfius_clean_response(&response);
        ulfius_clean_request(&request);

        A(json_is_object(json));
        json_t *value = json_object_get(json, "camid");
        if (value)
        {
            stream.camid_switch = json_integer_value(value);
            config_t *config = get_config(stream.camid_switch);
            if (!config)
                stream.camid_switch = 0;
        }
        json_decref(json);
    }
}

void set_player_cam()
{
    if (!strcmp(stream.day, stream.actualday))
    {
        struct _u_request request;
        json_t *json;
        CAVNZ(json, json_pack("{s:i}", "camid", stream.camid_switch));
        ulfius_init_request(&request);
        CAZ(ulfius_set_request_properties(&request,
                                          U_OPT_HTTP_VERB, "POST",
                                          U_OPT_HTTP_URL, stream.masteruri,
                                          U_OPT_HTTP_URL_APPEND, "/players",
                                          U_OPT_HTTP_URL_APPEND, "/",
                                          U_OPT_HTTP_URL_APPEND, stream.playerid,
                                          U_OPT_JSON_BODY, json,
                                          U_OPT_TIMEOUT, 1ul,
                                          U_OPT_NONE));
        CAZ(ulfius_send_http_request(&request, NULL));
        ulfius_clean_request(&request);
        json_decref(json);
    }
}

void normalize_ts(struct timespec *ts)
{
    ts->tv_sec += ts->tv_nsec / NS_IN_SEC;
    ts->tv_nsec = ts->tv_nsec % NS_IN_SEC;
    if (ts->tv_sec > 0 && ts->tv_nsec < 0)
    {
        ts->tv_sec--;
        ts->tv_nsec = ts->tv_nsec + NS_IN_SEC;
    }
    else if (ts->tv_sec < 0 && ts->tv_nsec > 0)
    {
        ts->tv_sec++;
        ts->tv_nsec = ts->tv_nsec - NS_IN_SEC;
    }
}

void info_browse_refresh()
{
    struct _u_request request;
    struct _u_response response;
    json_t *json;
    ulfius_init_request(&request);
    ulfius_init_response(&response);
    CAZ(ulfius_set_request_properties(&request,
                                      U_OPT_HTTP_VERB, "GET",
                                      U_OPT_HTTP_URL, stream.masteruri,
                                      U_OPT_HTTP_URL_APPEND, "/cams",
                                      U_OPT_TIMEOUT, 10ul,
                                      U_OPT_NONE));
    CAZ(ulfius_send_http_request(&request, &response));
    CAVNZ(json, ulfius_get_json_body_response(&response, NULL));
    ulfius_clean_response(&response);
    ulfius_clean_request(&request);

    A(json_is_array(json));
    stream.info_browselen = json_array_size(json);
    if (stream.info_browselen > MAX_BROWSE)
        stream.info_browselen = MAX_BROWSE;
    A(stream.info_browselen < sizeof(stream.info_browse) / sizeof(stream.info_browse[0]));
    for (int i = 0; i < stream.info_browselen; i++)
    {
        json_t *date = json_array_get(json, i);
        A(json_is_string(date));
        strncpy(stream.info_browse[i], json_string_value(date), sizeof(stream.info_browse[0]));
    }
    json_decref(json);

    qsort(stream.info_browse, stream.info_browselen, sizeof(stream.info_browse[0]), (__compar_fn_t)strcmp);
}

void info_cfg_load()
{
    struct _u_request request;
    struct _u_response response;
    json_t *json;

    if (!stream.day[0])
    {
        stream.info_changed |= (0 != stream.configslen);
        stream.configslen = 0;
        return;
    }

    ulfius_init_request(&request);
    ulfius_init_response(&response);
    CAZ(ulfius_set_request_properties(&request,
                                      U_OPT_HTTP_VERB, "GET",
                                      U_OPT_HTTP_URL, stream.masteruri,
                                      U_OPT_HTTP_URL_APPEND, "/cams/",
                                      U_OPT_HTTP_URL_APPEND, stream.day,
                                      U_OPT_TIMEOUT, 20ul,
                                      U_OPT_NONE));
    CAZ(ulfius_send_http_request(&request, &response));
    int status = response.status;
    CAVNZ(json, ulfius_get_json_body_response(&response, NULL));
    ulfius_clean_response(&response);
    ulfius_clean_request(&request);

    if (status / 100 != 2)
    {
        stream.configslen = 0;
        return;
    }

    A(json_is_object(json));

    bool changed = json_object_size(json) != stream.configslen;
    CAV(stream.configslen, json_object_size(json), < MAX_CAM * MAX_MAT);

    void *iter = json_object_iter(json);
    for (int i = 0; iter; i++)
    {
        int camid = atoi(json_object_iter_key(iter));
        json_t *value = json_object_iter_value(iter);
        A(json_is_object(value));
        json_int_t mat = json_integer_value(json_object_get(value, "mat"));
        json_int_t position = json_integer_value(json_object_get(value, "position"));
        if (camid != stream.configs[i].camid ||
            mat != stream.configs[i].mat ||
            position != stream.configs[i].position)
        {
            changed = true;
            stream.configs[i].camid = camid;
            stream.configs[i].mat = mat;
            stream.configs[i].position = position;
        }
        iter = json_object_iter_next(json, iter);
    }
    json_decref(json);
    stream.info_changed |= changed;
}

void scale_compute(bool center)
{
    float crt_ratio = (float)stream.crtc_width / stream.crtc_height;
    float frame_ratio = (float)stream.width / stream.height;

    stream.hid_w = stream.width * HID_ZOOM_DIV / (stream.hid_zoom + HID_ZOOM_DIV);
    stream.hid_h = stream.height * HID_ZOOM_DIV / (stream.hid_zoom + HID_ZOOM_DIV);

    if (crt_ratio < frame_ratio)
        stream.hid_w *= crt_ratio / frame_ratio;
    else
        stream.hid_h *= crt_ratio / frame_ratio;

    stream.hid_mx = stream.width - stream.hid_w;
    stream.hid_my = stream.height - stream.hid_h;

    if (center)
    {
        stream.hid_x = stream.hid_mx / 2;
        stream.hid_y = stream.hid_my / 2;
    }
    else
    {
        if (stream.hid_x > stream.hid_mx)
            stream.hid_x = stream.hid_mx;
        if (stream.hid_y > stream.hid_my)
            stream.hid_y = stream.hid_my;
    }

    LOG("scale %ux%u-%u(<%d)-%u(<%d) %ux%u-%u-%u\n", stream.hid_w, stream.hid_h, stream.hid_x, stream.hid_mx, stream.hid_y, stream.hid_my, 0, 0, stream.crtc_width, stream.crtc_height);
}

void scale_reset()
{
    CAZ(pthread_mutex_lock(&stream.scale_mutex));
    stream.hid_zoom = 0;
    scale_compute(true);
    CAZ(pthread_cond_signal(&stream.scale_cond));
    CAZ(pthread_mutex_unlock(&stream.scale_mutex));
}

// +++ MULTI TOUCH

void hid_event(int fd, struct input_event *ev)
{
    int ret = 0, i;

    struct timespec ats;
    clock_gettime(CLOCK_REALTIME, &ats);

    // DBG("MT: [%d/%d] %x %x %d\n", fd, stream.hid_absfd, ev->type, ev->code, ev->value);
    if (ev->type == EV_ABS && (!stream.hid_absfd || stream.hid_absfd == fd))
    {
        switch (ev->code)
        {
        case ABS_MT_SLOT:
            if (ev->value < MT_FINGERS)
                stream.hid_slot = ev->value;
            else
                stream.hid_slot = -1;
            break;
        case ABS_MT_TRACKING_ID:
            // DBG("MT: id %d fd %d\n", ev->value, fd);
            if (!stream.hid_absfd)
            {
                stream.hid_absfd = fd;
                CAZ(ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), stream.hid_absX));
                CAZ(ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), stream.hid_absY));

                stream.hid_scale_x = ((float)INFO_WIDTH / (stream.hid_absX[MT_MAX] - stream.hid_absX[MT_MIN]));
                stream.hid_scale_y = ((float)INFO_HEIGHT / (stream.hid_absY[MT_MAX] - stream.hid_absY[MT_MIN]));
                stream.hid_check = true;
            }
            if (stream.hid_slot >= 0)
            {
                stream.hid_touch[stream.hid_slot].track = ev->value;
                // stream.hid_check = ev->value == -1;
                stream.hid_check = true;
            }
            break;
        case ABS_MT_POSITION_X:
            if (stream.hid_slot >= 0 && stream.hid_touch[stream.hid_slot].track >= 0)
            {
                stream.hid_touch[stream.hid_slot].x = (int)((float)(ev->value - stream.hid_absX[MT_MIN]) * stream.hid_scale_x);
                stream.hid_check = true;
            }
            break;
        case ABS_MT_POSITION_Y:
            if (stream.hid_slot >= 0 && stream.hid_touch[stream.hid_slot].track >= 0)
            {
                stream.hid_touch[stream.hid_slot].y = (int)((float)(ev->value - stream.hid_absY[MT_MIN]) * stream.hid_scale_y);
                stream.hid_check = true;
            }
            break;
        }
    }

    if (ev->type == EV_SYN && ev->code == SYN_REPORT)
    {
        int64_t pushtime = -1;
        if (stream.hid_pushtime.tv_sec)
        {
            struct timespec hid_pushtime = stream.hid_pushtime;
            // LOG("REL: %ld %ld\n", stream.hid_pushtime.tv_sec, stream.hid_pushtime.tv_nsec / 1000000);
            // LOG("RELA: %ld %ld\n", ats.tv_sec, ats.tv_nsec / 1000000);
            hid_pushtime.tv_sec -= ats.tv_sec;
            hid_pushtime.tv_nsec -= ats.tv_nsec;
            normalize_ts(&stream.hid_pushtime);
            pushtime = -hid_pushtime.tv_sec * 1000 - (hid_pushtime.tv_nsec / 1000000);
            // LOG("PUSHTIME: %ld\n", pushtime);

            stream.info_changed |= INFO_DRAW_FINGER;
        }

        if (stream.hid_check || pushtime > MT_LONGPRESS)
        {
            int finger = stream.hid_slot;
            int x = stream.hid_touch[finger].x;
            int y = stream.hid_touch[finger].y;
            bool push = stream.hid_touch[finger].track >= 0;

            stream.hid_check = false;

            for (i = 0; i < stream.hid_actionslen; i++)
                if (stream.gui == stream.hid_actions[i].gui &&
                    x >= stream.hid_actions[i].x && x <= stream.hid_actions[i].x + stream.hid_actions[i].w &&
                    y >= stream.hid_actions[i].y && y <= stream.hid_actions[i].y + stream.hid_actions[i].h)
                    break;

            assert(i < stream.hid_actionslen);
            action_t action = stream.hid_actions[i].action;
            int param = stream.hid_actions[i].param;

            LOG("MT: finger[%d] %d:%d %d %d/%d\n", finger, x, y, push, action, param);

            if (finger == 0)
            {
                if (!push)
                {
                    stream.info_changed |= INFO_DRAW_FINGER;

                    if (stream.hid_action == A_SPEED && abs(stream.speed) > SPEED_PLAY)
                    {
                        CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                        stream.speed = SPEED_PLAY;
                        CAZ(pthread_cond_signal(&stream.cmd_cond));
                        CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                        stream.info_changed = true;
                    }

                    else if (action == A_CFG_END && stream.hid_pushtime.tv_sec)
                    {
                        CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                        stream.info_config_switching = false;
                        stream.gui = GUI_PLAYER;
                        get_player_cam();
                        CAZ(pthread_cond_signal(&stream.cmd_cond));
                        CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                        stream.info_changed = true;
                    }

                    stream.hid_pushtime.tv_sec = 0;
                    stream.hid_action = A_NONE;
                    stream.hid_param = 0;
                    stream.hid_t_x = 0;
                    stream.hid_t_y = 0;
                }
                else
                {
                    if (pushtime > MT_LONGPRESS)
                    {
                        stream.hid_pushtime.tv_sec = 0;

                        if (action == A_MAT)
                        {
                            stream.gui = stream.config_gui;
                            if (stream.gui == GUI_BROWSE)
                                info_browse_refresh();

                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            stream.camid_switch = 0;
                            CAZ(pthread_cond_signal(&stream.cmd_cond));
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

                            scale_reset();

                            stream.hid_action = A_CFG_END;
                            stream.info_changed = true;
                        }
                        else if (action == A_CFG_END)
                        {
                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            stream.info_config_switching = false;
                            stream.camid_switch = 0;

                            stream.gui = stream.config_gui = (stream.config_gui == GUI_CONFIG ? GUI_BROWSE : GUI_CONFIG);
                            if (stream.gui == GUI_BROWSE)
                                info_browse_refresh();
                            else
                                strcpy(stream.day, stream.actualday);
                            info_cfg_load();

                            CAZ(pthread_cond_signal(&stream.cmd_cond));
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

                            stream.info_changed = true;
                        }
                        else if (action == A_BRS_DELETE && (param < stream.info_browselen && !strcmp(stream.info_browse[param], stream.day)))
                        {
                            LOG("DELETE: %s\n", stream.day);
                            stream.info_browse_deleting = true;
                            stream.info_changed = true;

                            struct _u_request request;
                            ulfius_init_request(&request);
                            CAZ(ulfius_set_request_properties(&request,
                                                              U_OPT_HTTP_VERB, "DELETE",
                                                              U_OPT_HTTP_URL, stream.masteruri,
                                                              U_OPT_HTTP_URL_APPEND, "/chunks/",
                                                              U_OPT_HTTP_URL_APPEND, stream.day,
                                                              U_OPT_TIMEOUT, 1800ul,
                                                              U_OPT_NONE));
                            CAZ(ulfius_send_http_request(&request, NULL));
                            ulfius_clean_request(&request);

                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            *stream.day = 0;
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

                            info_browse_refresh();
                            stream.info_browse_deleting = false;
                            stream.info_changed = true;
                        }
                        else if (action == A_CFG_SELECT)
                        {
                            LOG("CONFIG: " CAMF " %d:%d\n", stream.camid, (param & 0xff00) >> 8, (param & 0xff));
                            stream.info_config_switching = true;
                            stream.info_changed = true;
                        }
                        else if (action == A_CFG_PAUSE)
                        {
                            stream.info_paused = !stream.info_paused;
                            LOG("recording %s\n", stream.info_paused ? "STOPPED" : "STARTED");
                            stream.info_changed = true;

                            struct _u_request request;
                            json_t *json;
                            CAVNZ(json, json_pack("{s:b}", "recording", !stream.info_paused));

                            ulfius_init_request(&request);
                            CAZ(ulfius_set_request_properties(&request,
                                                              U_OPT_HTTP_VERB, "PUT",
                                                              U_OPT_HTTP_URL, stream.masteruri,
                                                              U_OPT_HTTP_URL_APPEND, "/recording",
                                                              U_OPT_JSON_BODY, json,
                                                              U_OPT_TIMEOUT, 10ul,
                                                              U_OPT_NONE));
                            CAZ(ulfius_send_http_request(&request, NULL));
                            ulfius_clean_request(&request);
                            json_decref(json);
                        }
                    }

                    else if (stream.hid_action != action || stream.hid_param != param)
                    {
                        stream.hid_pushtime = ats;
                        DBG("PUSH: %ld %ld\n", ats.tv_sec, ats.tv_nsec / 1000000);

                        if (stream.hid_action != action && stream.hid_action == A_SPEED && abs(stream.speed) > SPEED_PLAY)
                        {
                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            stream.speed = SPEED_PLAY;
                            CAZ(pthread_cond_signal(&stream.cmd_cond));
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                            stream.info_changed = true;
                        }

                        switch (action)
                        {
                        case A_MAT:
                        {
                            // switch to next mat
                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            info_cfg_load();

                            config_t *config = get_config(stream.camid);
                            uint8_t mat = MAX_MAT + 1;
                            if (config)
                            { // find next
                                for (i = 0; i < stream.configslen; i++)
                                    if (config->mat < stream.configs[i].mat && stream.configs[i].mat < mat)
                                        mat = stream.configs[i].mat;
                            }
                            if (mat == MAX_MAT + 1)
                            { // find from begin
                                for (i = 0; i < stream.configslen; i++)
                                    if (stream.configs[i].mat < mat)
                                        mat = stream.configs[i].mat;
                            }
                            if (mat < MAX_MAT + 1)
                            { // find first cam
                                uint8_t positionlen;
                                config_t *cams[MAX_CAM];
                                CAVNZ(positionlen, get_config_cam(mat, cams));
                                for (i = 0; i < MAX_CAM; i++)
                                    if (cams[i])
                                    {
                                        stream.camid_switch = cams[i]->camid;
                                        break;
                                    }
                            }
                            stream.speed = SPEED_PLAY;
                            set_player_cam();

                            CAZ(pthread_cond_signal(&stream.cmd_cond));
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                            stream.hid_action = action;
                            stream.hid_param = 0;
                            stream.info_changed = true;
                        }
                        break;
                        case A_CAM:
                        {
                            // change to valid cam
                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            config_t *config;
                            CAVNZ(config, get_config(stream.camid));
                            uint8_t positionlen, camid;
                            config_t *cams[MAX_CAM];
                            CAVNZ(positionlen, get_config_cam(config->mat, cams));
                            if (param < positionlen)
                            {
                                for (i = 0, camid = 0; i < MAX_CAM; i++)
                                    if (cams[i])
                                    {
                                        if (camid == param)
                                        {
                                            stream.camid_switch = cams[i]->camid;
                                            break;
                                        }
                                        camid++;
                                    }
                                set_player_cam();

                                CAZ(pthread_cond_signal(&stream.cmd_cond));
                                CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                                stream.hid_action = action;
                                stream.hid_param = param;
                                stream.info_changed = true;
                            }
                            else
                            {
                                CAZ(pthread_cond_signal(&stream.cmd_cond));
                                CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                                stream.hid_action = A_NONE;
                                stream.hid_param = 0;
                            }
                        }
                        break;
                        case A_SPEED:
                        {
                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            stream.speed = param;
                            CAZ(pthread_cond_signal(&stream.cmd_cond));
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                            stream.hid_action = action;
                            stream.hid_param = param;
                            stream.info_changed = true;
                        }
                        break;
                        case A_RESTORE:
                        {
                            scale_reset();

                            // reset camera, speed, ms
                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            stream.speed = SPEED_PLAY;

                            struct timespec ts;
                            clock_gettime(CLOCK_REALTIME, &ts);
                            stream.show_msec_seek = ts.tv_sec * 1000 + ts.tv_nsec / NS_IN_MSEC - 5000; // start from t-5sec
                            CAZ(pthread_cond_signal(&stream.cmd_cond));
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

                            stream.hid_action = action;
                            stream.hid_param = param;
                            stream.info_changed = true;
                        }
                        break;
                        case A_MOVE:
                        {
                            int diff_x = x - stream.hid_t_x;
                            int diff_y = y - stream.hid_t_y;
                            // DBG("MT: MOVE %d=%d-%d %d=%d-%d\n", diff_x, x, stream.hid_t_x, diff_y, y, stream.hid_t_y);
                            if (stream.hid_t_x && stream.hid_t_y)
                            {
                                if (abs(diff_x) > HID_DIFF || abs(diff_y) > HID_DIFF)
                                {

                                    CAZ(pthread_mutex_lock(&stream.scale_mutex));
                                    stream.hid_x -= diff_x;
                                    if (stream.hid_x > stream.hid_mx)
                                        stream.hid_x = stream.hid_mx;
                                    else if (stream.hid_x < 0)
                                        stream.hid_x = 0;
                                    stream.hid_y -= diff_y;
                                    if (stream.hid_y > stream.hid_my)
                                        stream.hid_y = stream.hid_my;
                                    else if (stream.hid_y < 0)
                                        stream.hid_y = 0;
                                    CAZ(pthread_cond_signal(&stream.scale_cond));
                                    CAZ(pthread_mutex_unlock(&stream.scale_mutex));

                                    stream.hid_t_x = x;
                                    stream.hid_t_y = y;
                                }
                            }
                            else
                            {
                                stream.hid_t_x = x;
                                stream.hid_t_y = y;
                            }
                            stream.hid_action = action;
                            stream.hid_param = 1;
                        }
                        break;
                        case A_BRS_SELECT:
                        case A_BRS_DELETE:
                        {
                            if (param < stream.info_browselen)
                            {
                                CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                                strcpy(stream.day, stream.info_browse[param]);
                                stream.camid = 0;
                                CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                                stream.hid_action = action;
                                stream.hid_param = param;
                                stream.info_changed = true;
                            }
                        }
                        break;
                        case A_CFG_SELECT:
                        {
                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            for (i = 0; i < stream.configslen; i++)
                                if ((param & 0xff00) >> 8 == stream.configs[i].mat && (param & 0xff) == stream.configs[i].position)
                                {
                                    stream.camid_switch = stream.configs[i].camid;
                                    stream.speed = SPEED_PLAY;
                                    break;
                                }

                            if (stream.info_config_switching)
                            {
                                struct _u_request request;
                                json_t *json;
                                char _cam[4];
                                CAP(snprintf(_cam, sizeof(_cam), "%u", stream.camid));
                                CAVNZ(json, json_pack("{s:i,s:i}", "mat", (param & 0xff00) >> 8, "position", param & 0xff));

                                ulfius_init_request(&request);
                                CAZ(ulfius_set_request_properties(&request,
                                                                  U_OPT_HTTP_VERB, "POST",
                                                                  U_OPT_HTTP_URL, stream.masteruri,
                                                                  U_OPT_HTTP_URL_APPEND, "/cams/",
                                                                  U_OPT_HTTP_URL_APPEND, stream.day,
                                                                  U_OPT_HTTP_URL_APPEND, "/",
                                                                  U_OPT_HTTP_URL_APPEND, _cam,
                                                                  U_OPT_JSON_BODY, json,
                                                                  U_OPT_TIMEOUT, 20ul,
                                                                  U_OPT_NONE));
                                CAZ(ulfius_send_http_request(&request, NULL));
                                ulfius_clean_request(&request);
                                json_decref(json);

                                stream.info_config_switching = false;
                            }

                            CAZ(pthread_cond_signal(&stream.cmd_cond));
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                            stream.hid_action = action;
                            stream.hid_param = param;

                            stream.info_changed = true;
                        }
                        break;
                        case A_BOOKMARK_ADD:
                        {
                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            config_t *config;
                            CAVNZ(config, get_config(stream.camid));
                            uint8_t matid = config ? config->mat : 0;
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

                            if (matid && stream.info_loadedmat == matid)
                            {
                                CAZ(pthread_mutex_lock(&stream.info_mutex));
                                if (stream.info_bookmarkslen < MAX_BOOKMARKS)
                                {
                                    for (i = 0; i < stream.info_bookmarkslen; i++)
                                        if (stream.info_bookmarks[i] >= stream.info_time.tv_sec)
                                            break;
                                    if (stream.info_bookmarks[i] != stream.info_time.tv_sec)
                                    {
                                        memmove(stream.info_bookmarks + i + 1, stream.info_bookmarks + i, sizeof(stream.info_bookmarks[0]) * stream.info_bookmarkslen - i);
                                        stream.info_bookmarks[i] = stream.info_time.tv_sec;
                                        stream.info_bookmarkslen++;
                                        stream.info_updating = true;
                                    }
                                }

                                if (stream.info_updating)
                                    mat_patch("bookmarks", stream.info_bookmarks, stream.info_bookmarkslen);
                                CAZ(pthread_mutex_unlock(&stream.info_mutex));
                            }

                            stream.info_updating = false;
                            stream.hid_action = action;
                            stream.hid_param = param;
                            stream.info_changed = true;
                        }
                        break;
                        case A_BOOKMARK_SHOW:
                        {
                            stream.gui = stream.gui == GUI_PLAYER ? GUI_BOOKMARKS : GUI_PLAYER;
                            stream.hid_action = action;
                            stream.hid_param = param;
                            stream.info_changed = true;
                        }
                        break;
                        case A_BOOKMARK_SELECT:
                        {
                            time_t sec = 0;

                            CAZ(pthread_mutex_lock(&stream.info_mutex));
                            if (param < stream.info_bookmarkslen)
                                sec = stream.info_bookmarks[param];
                            CAZ(pthread_mutex_unlock(&stream.info_mutex));

                            if (sec)
                            {
                                scale_reset();
                                CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                                stream.speed = SPEED_PLAY;
                                stream.show_msec_seek = sec * 1000;
                                CAZ(pthread_cond_signal(&stream.cmd_cond));
                                CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                            }

                            stream.hid_action = action;
                            stream.hid_param = param;
                            stream.info_changed = true;
                        }
                        break;
                        case A_BOOKMARK_DELETE:
                        {
                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            config_t *config;
                            CAVNZ(config, get_config(stream.camid));
                            uint8_t matid = config ? config->mat : 0;
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

                            if (matid && stream.info_loadedmat == matid)
                            {
                                CAZ(pthread_mutex_lock(&stream.info_mutex));
                                struct timespec info_time = stream.info_time;
                                if (param < stream.info_bookmarkslen)
                                {

                                    int in_bookmark_idx = -1;
                                    for (i = 0; i < stream.info_bookmarkslen; i++)
                                        if (info_time.tv_sec >= stream.info_bookmarks[i] && (i == stream.info_bookmarkslen - 1 || info_time.tv_sec < stream.info_bookmarks[i + 1]))
                                        {
                                            in_bookmark_idx = i;
                                            break;
                                        }

                                    if (param == in_bookmark_idx)
                                    {
                                        memmove(stream.info_bookmarks + param, stream.info_bookmarks + param + 1, sizeof(stream.info_bookmarks[0]) * stream.info_bookmarkslen - param - 1);
                                        stream.info_bookmarkslen--;
                                    }
                                    stream.info_updating = true;
                                }
                                if (stream.info_updating)
                                    mat_patch("bookmarks", stream.info_bookmarks, stream.info_bookmarkslen);
                                CAZ(pthread_mutex_unlock(&stream.info_mutex));
                            }

                            stream.info_updating = false;
                            stream.hid_action = action;
                            stream.hid_param = param;
                            stream.info_changed = true;
                        }
                        break;
                        case A_MEDICAL_STOP:
                        case A_MEDICAL_START:
                        {
                            CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                            config_t *config;
                            CAVNZ(config, get_config(stream.camid));
                            uint8_t matid = config ? config->mat : 0;
                            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

                            if (matid && stream.info_loadedmat == matid)
                            {
                                CAZ(pthread_mutex_lock(&stream.info_mutex));
                                struct timespec info_time = stream.info_time;

                                int in_medical_idx = -1;
                                for (i = 0; i < stream.info_medicalslen; i += 2)
                                    if (info_time.tv_sec >= stream.info_medicals[i] - MEDICAL_EXTEND && (info_time.tv_sec <= stream.info_medicals[i + 1] + MEDICAL_EXTEND))
                                    {
                                        in_medical_idx = i;
                                        break;
                                    }

                                if (in_medical_idx == -1)
                                {
                                    if (stream.info_medicalslen < MAX_MEDICALS && action == A_MEDICAL_START)
                                    {
                                        // new
                                        for (i = 0; i < stream.info_medicalslen; i += 2)
                                            if (stream.info_medicals[i + 1] >= stream.info_time.tv_sec)
                                                break;

                                        memmove(stream.info_medicals + i + 2, stream.info_medicals + i, sizeof(stream.info_medicals[0]) * stream.info_medicalslen - i);

                                        stream.info_medicals[i] = info_time.tv_sec;
                                        stream.info_medicals[i + 1] = LONG_MAX - MEDICAL_EXTEND;
                                        stream.info_medicalslen += 2;
                                        stream.info_updating = true;
                                    }
                                }
                                else
                                {
                                    // move start or end
                                    if (action == A_MEDICAL_START && info_time.tv_sec <= stream.info_medicals[in_medical_idx + 1])
                                        stream.info_medicals[in_medical_idx] = info_time.tv_sec;
                                    if (action == A_MEDICAL_STOP && info_time.tv_sec >= stream.info_medicals[in_medical_idx])
                                        stream.info_medicals[in_medical_idx + 1] = info_time.tv_sec;

                                    // delete
                                    if (stream.info_medicals[in_medical_idx + 1] - stream.info_medicals[in_medical_idx] < MEDICAL_DELETE)
                                    {
                                        memmove(stream.info_medicals + in_medical_idx, stream.info_medicals + in_medical_idx + 2, sizeof(stream.info_medicals[0]) * stream.info_medicalslen - 2);
                                        stream.info_medicalslen -= 2;
                                    }
                                    stream.info_updating = true;
                                }

                                if (stream.info_updating)
                                    mat_patch("medicals", stream.info_medicals, stream.info_medicalslen);
                                CAZ(pthread_mutex_unlock(&stream.info_mutex));
                            }

                            stream.info_updating = false;
                            stream.hid_action = action;
                            stream.hid_param = param;
                            stream.info_changed = true;
                        }
                        break;
                        case A_NONE: // not changed
                        default:
                            stream.hid_action = action;
                            stream.hid_param = 0;
                        }
                    }
                }
            }
            else if (finger == 1 && stream.hid_action == A_MOVE)
            {
                // resize

                if (!push)
                    stream.hid_t_distance = 0;
                else
                {
                    int x0 = stream.hid_touch[0].x;
                    int y0 = stream.hid_touch[0].y;

                    int distance = sqrt(pow(x0 - x, 2) + pow(y0 - y, 2));

                    if (!stream.hid_t_distance)
                        stream.hid_t_distance = distance;
                    else
                    {
                        int diff = distance - stream.hid_t_distance;
                        // DBG("MT: dist %d = %d - %d\n", diff, distance, stream.hid_t_distance);
                        if (abs(diff) > HID_DIFF)
                        {

                            CAZ(pthread_mutex_lock(&stream.scale_mutex));
                            stream.hid_zoom += diff;
                            if (stream.hid_zoom < 0)
                                stream.hid_zoom = 0;
                            else if (stream.hid_zoom > HID_ZOOM_MAX)
                                stream.hid_zoom = HID_ZOOM_MAX;

                            scale_compute(false);

                            CAZ(pthread_cond_signal(&stream.scale_cond));
                            CAZ(pthread_mutex_unlock(&stream.scale_mutex));

                            stream.hid_t_distance = distance;
                        }
                    }
                }
            }
        }
    }
    assert(!ret);
}

// +++ INFO

uint32_t *info_read_png(char *filename, uint32_t *rwidth, uint32_t *rheight)
{
    int width, height;
    png_byte color_type;
    png_byte bit_depth;
    png_bytep *row_pointers = NULL;

    FILE *fp;
    LOG("PNG %s\n", filename);
    CAVNZ(fp, fopen(filename, "rb"));
    png_structp png;
    CAVNZ(png, png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL));
    png_infop info;
    CAVNZ(info, png_create_info_struct(png));
    assert(!setjmp(png_jmpbuf(png)));

    png_init_io(png, fp);

    png_read_info(png, info);

    *rwidth = width = png_get_image_width(png, info);
    *rheight = height = png_get_image_height(png, info);
    color_type = png_get_color_type(png, info);
    bit_depth = png_get_bit_depth(png, info);

    if (bit_depth == 16)
        png_set_strip_16(png);

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png);

    // PNG_COLOR_TYPE_GRAY_ALPHA is always 8 or 16bit depth.
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8(png);

    if (png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png);

    // These color_type don't have an alpha channel then fill it with 0xff.
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler(png, 0xFF, PNG_FILLER_AFTER);

    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);

    png_read_update_info(png, info);

    row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
    for (int y = 0; y < height; y++)
        row_pointers[y] = (png_byte *)malloc(png_get_rowbytes(png, info));

    png_read_image(png, row_pointers);

    fclose(fp);

    png_destroy_read_struct(&png, &info, NULL);

    uint32_t *pixels, *_pixels;
    CAVNZ(pixels, malloc(width * height * sizeof(uint32_t)));
    _pixels = pixels;

    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            *_pixels++ = (row_pointers[y][x * 4 + 0] << 16) + (row_pointers[y][x * 4 + 1] << 8) +
                         (row_pointers[y][x * 4 + 2] << 0) + ((row_pointers[y][x * 4 + 3] / 2) << 24);

    for (int y = 0; y < height; y++)
        free(row_pointers[y]);
    free(row_pointers);

    return pixels;
}

static void stream_draw_unicode(uint32_t fb, uint32_t *x, uint32_t *y, uint32_t color, uint32_t chr)
{
    CAZ(FT_Load_Char(stream.info_face, chr, FT_LOAD_RENDER));
    FT_GlyphSlot slot = stream.info_face->glyph;
    uint8_t alpha = (color & 0xff000000) >> 24;

    for (uint32_t i = 0; i < slot->bitmap.width && (slot->bitmap_left + i + *x) < INFO_WIDTH; i++)
        for (uint32_t j = 0; j < slot->bitmap.rows && (j - slot->bitmap_top + *y) < INFO_HEIGHT; j++)
        {
            uint8_t pix = slot->bitmap.buffer[i + j * slot->bitmap.pitch];
            if (pix)
                *(stream.info_fbs[fb].map + (slot->bitmap_left + i + *x) + (j - slot->bitmap_top + *y) * stream.info_fbs[fb].pitch / sizeof(uint32_t)) = (pix + alpha > 255 ? 255 : pix + alpha) << 24 | (((color >> 16) & 0xff) * pix / 255) << 16 | (((color >> 8) & 0xff) * pix / 255) << 8 | (((color >> 0) & 0xff) * pix / 255) << 0;
        }
    *x += slot->advance.x / 64;
    *y += slot->advance.y / 64;
}

void info_fill(uint32_t fb, int x, int y, uint32_t w, uint32_t h, uint32_t color)
{
    uint32_t pitch = stream.info_fbs[fb].pitch / sizeof(uint32_t);
    uint32_t *fbmap = stream.info_fbs[fb].map;
    if (x < 0)
    {
        w += x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if (x + w > INFO_WIDTH)
        w = INFO_WIDTH - x;
    if (y + h > INFO_HEIGHT)
        h = INFO_HEIGHT - y;

    fbmap += y * pitch + x;
    for (; h--;)
    {
        for (uint32_t _x = 0; _x < w; _x++)
            *fbmap++ = color;
        fbmap += pitch - w;
    }
}

void info_img(uint32_t fb, int x, int y, img_t *img, bool mirror)
{
    uint32_t pitch = stream.info_fbs[fb].pitch / sizeof(uint32_t);
    uint32_t *fbmap = stream.info_fbs[fb].map;

    uint32_t w = img->width, h = img->height, *map = img->map;

    if (x < 0)
    {
        w += x;
        map += -x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        map += -y * img->width;
        y = 0;
    }
    if (x + w > INFO_WIDTH)
        w = INFO_WIDTH - x;
    if (y + h > INFO_HEIGHT)
        h = INFO_HEIGHT - y;

    fbmap += y * pitch + x;
    if (mirror)
    {
        for (; h--;)
        {
            map += w;
            for (uint32_t _x = 0; _x < w; _x++)
            {
                uint32_t pixel = *--map;
                if (pixel & 0xff000000)
                    *fbmap = pixel;
                fbmap++;
            }
            map += img->width;
            fbmap += pitch - w;
        }
    }
    else
    {
        for (; h--;)
        {
            for (uint32_t _x = 0; _x < w; _x++)
            {
                uint32_t pixel = *map++;
                if (pixel & 0xff000000)
                    *fbmap = pixel;
                fbmap++;
            }
            map += img->width - w;
            fbmap += pitch - w;
        }
    }
}

void *stream_scale_thread(void *param)
{
    int hid_x = 0, hid_y = 0, hid_w = 0, hid_h = 0;

    LOG("SCALE THREAD START\n");

    while (!stream.stopping)
    {
        CAZ(pthread_mutex_lock(&stream.scale_mutex));
        while (
            !stream.stopping &&
            hid_x == stream.hid_x &&
            hid_y == stream.hid_y &&
            hid_w == stream.hid_w &&
            hid_h == stream.hid_h &&
            !pthread_cond_wait(&stream.scale_cond, &stream.scale_mutex))
            ;
        hid_x = stream.hid_x;
        hid_y = stream.hid_y;
        hid_w = stream.hid_w;
        hid_h = stream.hid_h;
        CAZ(pthread_mutex_unlock(&stream.scale_mutex));
        if (stream.stopping)
            break;
        disp_plane_scale(stream.vi, hid_x, hid_y, hid_w, hid_h, 0, 0, stream.crtc_width, stream.crtc_height);
    }
    LOG("SCALE THREAD STOP\n");
    return NULL;
}

void *stream_info_thread(void *param)
{
    int i;

    LOG("INFO THREAD START\n");
    uint32_t fb = 0;
    uint64_t info_time_prev = 0, ts_prev = 0;

    while (!stream.stopping)
    {
        usleep(2 * STREAM_FPS_MSEC);

        uint32_t x, y, on;
        char str[256], *c;

        CAZ(pthread_mutex_lock(&stream.info_mutex));
        struct timespec info_time = stream.info_time;
        bool in_bookmark = false;
        int in_bookmark_idx = -1;
        for (i = 0; i < stream.info_bookmarkslen; i++)
            if (info_time.tv_sec >= stream.info_bookmarks[i] && (i == stream.info_bookmarkslen - 1 || info_time.tv_sec < stream.info_bookmarks[i + 1]))
            {
                in_bookmark_idx = i;
                in_bookmark = info_time.tv_sec <= stream.info_bookmarks[i] + BOOKMARK_DELAY;
                break;
            }
        bool in_startmedical = false, in_stopmedical = false;
        for (i = 0; i < stream.info_medicalslen; i += 2)
            if (info_time.tv_sec >= stream.info_medicals[i] - MEDICAL_EXTEND && (info_time.tv_sec <= stream.info_medicals[i + 1] + MEDICAL_EXTEND))
            {
                in_startmedical = info_time.tv_sec <= stream.info_medicals[i + 1];
                in_stopmedical = info_time.tv_sec >= stream.info_medicals[i];
                break;
            }
        CAZ(pthread_mutex_unlock(&stream.info_mutex));

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);

        if ((stream.gui == GUI_EMPTY || stream.gui == GUI_PLAYER || stream.gui == GUI_BOOKMARKS) && (info_time_prev != info_time.tv_sec || ts_prev != ts.tv_sec))
        {
            stream.info_changed = true;
            info_time_prev = info_time.tv_sec;
            ts_prev = ts.tv_sec;
        }

        DBG("I: FB %d %ld.%03ld\n", fb, ts.tv_sec, ts.tv_nsec / 1000000);

        if (!stream.info_changed || stream.camid != stream.camid_switch)
            continue;

        uint8_t positionseq = 0, position = 0, mat = 0, positionlen = 0;

        CAZ(pthread_mutex_lock(&stream.cmd_mutex));
        config_t *config = get_config(stream.camid);
        if (config)
        {
            mat = config->mat;
            position = config->position;
            config_t *cams[MAX_CAM];
            CAVNZ(positionlen, get_config_cam(config->mat, cams));
            for (i = 0; i < MAX_CAM; i++)
                if (cams[i])
                {
                    positionseq++;
                    if (cams[i]->position == config->position)
                        break;
                }
        }
        CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

        stream.info_changed = false;

        fb = (fb + 1) % 2;
        info_fill(fb, 0, 0, INFO_WIDTH, INFO_HEIGHT, 0);

        switch (stream.gui)
        {
        case GUI_CONFIG:
            info_img(fb, INFO_MAT_X, INFO_MAT_Y, &stream.info_timg[INFO_TIMG_SETUP], false);
            on = stream.info_paused ? INFO_CBIMG_PAUSED : INFO_CBIMG_RECORDING;
            info_img(fb, INFO_PAUSE_X, INFO_PAUSE_Y, &stream.info_cbimg[on], false);

            for (int _mat = 0; _mat < MAX_MAT; _mat++)
            {
                x = INFO_CONFIG_X + _mat * INFO_CONFIG_NEXT_X;
                y = INFO_CONFIG_Y;
                info_img(fb, x - INFO_MAT_W / 2, y - INFO_MAT_H / 2, &stream.info_timg[_mat + 1], false);
                for (int _position = 0; _position < MAX_CAM; _position++)
                {
                    *str = 0;
                    CAZ(pthread_mutex_lock(&stream.cmd_mutex));
                    for (i = 0; i < stream.configslen; i++)
                        if (stream.configs[i].mat == _mat + 1 && stream.configs[i].position == _position + 1)
                        {
                            sprintf(str, "%02u", stream.configs[i].camid);
                            break;
                        }
                    CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
                    on = (_mat + 1 == mat && _position + 1 == position) ? (stream.info_config_switching ? INFO_CBIMG_SELECTED : INFO_CBIMG_YES) : INFO_CBIMG_NO;
                    uint32_t fx = x - INFO_BORDER / 2 - INFO_CONFIG_CAM_W + (_position % 2) * (INFO_BORDER + INFO_CONFIG_CAM_W),
                             fy = y + INFO_BORDER / 2 + INFO_MAT_H / 2 - (INFO_CONFIG_CAM_H + INFO_MAT_H + INFO_BORDER) * (_position / 2);
                    info_img(fb, fx, fy, &stream.info_cbimg[on], (_position % 2));

                    fx += INFO_BORDER;
                    fy += 75;
                    for (c = str; *c; c++)
                        stream_draw_unicode(fb, &fx, &fy, on ? INFO_CONFIG_CAM_FGS : INFO_CONFIG_CAM_FG, *c);
                }
            }

            break;

        case GUI_BROWSE:
            info_img(fb, INFO_MAT_X, INFO_MAT_Y, &stream.info_timg[INFO_TIMG_BROWSE], false);
            int i = 0;
            for (y = INFO_BROWSE_Y; y < INFO_HEIGHT - INFO_BROWSE_H + INFO_BORDER && i < stream.info_browselen; y += INFO_BROWSE_H + INFO_BORDER)
                for (x = INFO_BROWSE_X; x < INFO_WIDTH - INFO_BROWSE_W + 2 * INFO_BORDER && i < stream.info_browselen; x += INFO_BROWSE_W + 2 * INFO_BORDER)
                {
                    bool selected = !strcmp(stream.info_browse[i], stream.day);
                    info_fill(fb, x, y, INFO_BROWSE_W, INFO_BROWSE_H, selected ? (stream.info_browse_deleting ? INFO_BROWSE_BGD : INFO_BROWSE_BGS) : INFO_BROWSE_BG);

                    uint32_t fx = x + INFO_BORDER, fy = y + INFO_BORDER - 5 + INFO_FONT_LINE;
                    for (c = stream.info_browse[i]; *c; c++)
                        stream_draw_unicode(fb, &fx, &fy, selected ? INFO_BROWSE_FGS : INFO_BROWSE_FG, *c);
                    if (selected)
                    {
                        fx = x + INFO_BROWSE_W - INFO_BORDER - 30;
                        stream_draw_unicode(fb, &fx, &fy, INFO_BROWSE_FGD, 0x00002715);
                    }
                    i++;
                }
            break;

        case GUI_BOOKMARKS:
            info_img(fb, INFO_MAT_X + 2 * (INFO_MAT_W + INFO_BORDER) + INFO_BORDER, INFO_MAT_Y, &stream.info_oimg[INFO_OIMG_BOOKMARKSHOWSEL], false);

            i = 0;
            for (y = INFO_BOOKMARK_Y; y < INFO_HEIGHT - INFO_BOOKMARK_H + INFO_BORDER && i < stream.info_bookmarkslen; y += INFO_BOOKMARK_H + INFO_BORDER)
                for (x = INFO_BOOKMARK_X; x < INFO_WIDTH - INFO_BOOKMARK_W + 2 * INFO_BORDER && i < stream.info_bookmarkslen; x += INFO_BOOKMARK_W + INFO_BORDER)
                {
                    info_fill(fb, x, y, INFO_BOOKMARK_W, INFO_BOOKMARK_H, in_bookmark_idx == i ? INFO_BOOKMARK_BGS : INFO_BOOKMARK_BG);

                    uint32_t fx = x + INFO_BORDER, fy = y + INFO_BORDER - 5 + INFO_FONT_LINE;
                    strftime(str, sizeof(str), "%H:%M:%S", localtime(&stream.info_bookmarks[i]));
                    for (c = str; *c; c++)
                        stream_draw_unicode(fb, &fx, &fy, in_bookmark_idx == i ? INFO_BOOKMARK_FGS : INFO_BOOKMARK_FG, *c);
                    if (in_bookmark_idx == i)
                    {
                        fx = x + INFO_BOOKMARK_W - INFO_BORDER - 30;
                        stream_draw_unicode(fb, &fx, &fy, INFO_BOOKMARK_FGD, 0x00002715);
                    }
                    i++;
                }

        case GUI_PLAYER:
        case GUI_EMPTY:

            if (mat)
            {
                if (stream.gui == GUI_EMPTY)
                    stream.gui = GUI_PLAYER;
            }
            else
                stream.gui = GUI_EMPTY;

            // TIMERS
            info_fill(fb, INFO_TIME_X, INFO_TIME_Y, INFO_TIME_W, INFO_TIME_H, INFO_ALPHA);

            x = INFO_TIME_X + INFO_BORDER;
            y = INFO_TIME_Y + INFO_FONT_LINE;
            if (strcmp(stream.day, stream.actualday))
            {
                if (strlen(stream.day) == 10)
                    for (c = stream.day + 2; *c; c++)
                        stream_draw_unicode(fb, &x, &y, 0xffffff | INFO_ALPHA, *c);
            }
            else
            {
                strftime(str, sizeof(str), "%H:%M:%S", localtime(&ts.tv_sec));
                for (c = str; *c; c++)
                    stream_draw_unicode(fb, &x, &y, 0xffffff | INFO_ALPHA, *c);
            }

            // MAT (maid 0=none)
            info_img(fb, INFO_MAT_X, INFO_MAT_Y, &stream.info_timg[mat], false);

            if (mat && positionseq)
            {
                if (info_time.tv_sec)
                {
                    x = INFO_TIME_X + INFO_BORDER;
                    y += INFO_FONT_LINE;
                    strftime(str, sizeof(str), "%H:%M:%S", localtime(&info_time.tv_sec));
                    for (c = str; *c; c++)
                        stream_draw_unicode(fb, &x, &y, 0xff8080 | INFO_ALPHA, *c);
                }

                // CAM
                for (i = 0; i < MAX_CAM; i++)
                    if (i < positionlen)
                    {
                        on = i + 1 == positionseq;
                        info_img(fb, INFO_CAM_X + i * (INFO_CAM_W + INFO_BORDER), INFO_BOTTOM_Y, &stream.info_cimg[on], false);
                    }

                // CMD SPEED
                x = INFO_SPEED_X;
                for (i = 0; i < sizeof(speed_map) / sizeof(speed_map[0]); i++)
                {
                    on = stream.speed == (i + SPEED_BCK_SKIP);
                    int img = abs(speed_map[i]);

                    info_img(fb, x, INFO_BOTTOM_Y, &stream.info_simg[img][on], speed_map[i] < 0);
                    x += stream.info_width[i] + INFO_SPEED_BORDER;
                }

                ts.tv_sec -= info_time.tv_sec;
                ts.tv_nsec -= info_time.tv_nsec;
                normalize_ts(&ts);
                on = ts.tv_sec <= 6 + 1 * stream.info_restore_prev;
                stream.info_restore_prev = on;
                info_img(fb, INFO_RESTORE_X, INFO_BOTTOM_Y, &stream.info_rimg[on], false);

                if (stream.gui == GUI_PLAYER)
                {
                    // playeronly
                    info_img(fb, INFO_MAT_X + (INFO_MAT_W + INFO_BORDER) + INFO_BORDER, INFO_MAT_Y, &stream.info_oimg[in_bookmark ? INFO_OIMG_BOOKMARKADDSEL : INFO_OIMG_BOOKMARKADD], false);
                    info_img(fb, INFO_MAT_X + 2 * (INFO_MAT_W + INFO_BORDER) + INFO_BORDER, INFO_MAT_Y, &stream.info_oimg[INFO_OIMG_BOOKMARKSHOW], false);
                    info_img(fb, INFO_MAT_X + 3 * (INFO_MAT_W + INFO_BORDER) + 2 * INFO_BORDER, INFO_MAT_Y, &stream.info_oimg[in_startmedical ? INFO_OIMG_MEDICALSTARTSEL : INFO_OIMG_MEDICALSTART], false);
                    info_img(fb, INFO_MAT_X + 4 * (INFO_MAT_W + INFO_BORDER) + 2 * INFO_BORDER, INFO_MAT_Y, &stream.info_oimg[in_stopmedical ? INFO_OIMG_MEDICALSTOPSEL : INFO_OIMG_MEDICALSTOP], false);
                    if (in_startmedical && in_stopmedical)
                    {
                        info_img(fb, INFO_WIDTH / 2 - stream.info_oimg[INFO_OIMG_MEDICAL].width / 2, INFO_HEIGHT / 2 - stream.info_oimg[INFO_OIMG_MEDICAL].height / 2, &stream.info_oimg[INFO_OIMG_MEDICAL], false);
                    }
                }
            }
            break;
        }

        if (!stream.info_touch)
        {
            info_fill(fb, INFO_MAT_X, INFO_MAT_Y, stream.info_timg[INFO_TIMG_NOTOUCH].width, stream.info_timg[INFO_TIMG_NOTOUCH].height, 0);
            info_img(fb, INFO_MAT_X, INFO_MAT_Y, &stream.info_timg[INFO_TIMG_NOTOUCH], false);
        }

        if (INFO_DRAW_FINGER)
        {
            for (i = 0; i < MT_FINGERS; i++)
                if (stream.hid_touch[i].track >= 0)
                    info_img(fb, stream.hid_touch[i].x - 25, stream.hid_touch[i].y - 25, &stream.info_fimg, false);
        }

        disp_plane_show_pic(stream.ui, stream.info_fbs[fb].fd);
    }

    LOG("INFO THREAD END\n");
    return NULL;
}

void info_setup()
{
    int i;
    uint32_t x, y;

    for (i = 0; i < sizeof(stream.info_fbs) / sizeof(stream.info_fbs[0]); i++)
        disp_plane_create(stream.ui, DRM_FORMAT_ARGB8888, INFO_WIDTH, INFO_HEIGHT, 32, &stream.info_fbs[i].fd, &stream.info_fbs[i].pitch, &stream.info_fbs[i].size, &stream.info_fbs[i].map);

    uint32_t pitches[DISP_MAX_PLANES], offsets[DISP_MAX_PLANES];
    memset(pitches, 0, sizeof(pitches));
    memset(offsets, 0, sizeof(offsets));
    pitches[0] = stream.info_fbs[0].pitch;
    disp_plane_setup(stream.ui, DRM_FORMAT_ARGB8888, INFO_WIDTH, INFO_HEIGHT, pitches, offsets, 3);

    memset(stream.info_fbs[0].map, 0, stream.info_fbs[0].size);
    // TODO: scaler for UI does not work (kernel driver dependent)
    disp_plane_scale(stream.ui, 0, 0, INFO_WIDTH, INFO_HEIGHT, 0, 0, stream.crtc_width, stream.crtc_height);
    disp_plane_show_pic(stream.ui, stream.info_fbs[0].fd);

    CAZ(FT_Init_FreeType(&stream.info_library));
    CAZ(FT_New_Face(stream.info_library, INFO_FONT, 0, &stream.info_face));
    CAZ(FT_Set_Char_Size(stream.info_face, 40 * 64, 0, 100, 0));
    CAZ(FT_Select_Charmap(stream.info_face, FT_ENCODING_UNICODE));

    assert(sizeof(speed_map) / sizeof(speed_map[0]) == -SPEED_BCK_SKIP + SPEED_SKIP + 1);

    for (i = 0; i < sizeof(stream.info_timg) / sizeof(stream.info_timg[0]); i++)
    {
        char fn[128];
        snprintf(fn, sizeof(fn), INFO_TIMG, i);
        stream.info_timg[i].map = info_read_png(fn, &stream.info_timg[i].width, &stream.info_timg[i].height);
    }
    for (i = 0; i < sizeof(stream.info_cbimg) / sizeof(stream.info_cbimg[0]); i++)
    {
        char fn[128];
        snprintf(fn, sizeof(fn), INFO_CBIMG, i);
        stream.info_cbimg[i].map = info_read_png(fn, &stream.info_cbimg[i].width, &stream.info_cbimg[i].height);
    }
    for (i = 0; i < 2; i++)
    {
        char fn[128];
        snprintf(fn, sizeof(fn), INFO_CIMG, i);
        stream.info_cimg[i].map = info_read_png(fn, &stream.info_cimg[i].width, &stream.info_cimg[i].height);
        snprintf(fn, sizeof(fn), INFO_RIMG, i);
        stream.info_rimg[i].map = info_read_png(fn, &stream.info_rimg[i].width, &stream.info_rimg[i].height);
        for (int j = 0; j < sizeof(stream.info_simg) / sizeof(stream.info_simg[0]); j++)
        {
            snprintf(fn, sizeof(fn), INFO_SIMG, j, i);
            stream.info_simg[j][i].map = info_read_png(fn, &stream.info_simg[j][i].width, &stream.info_simg[j][i].height);
            for (int k = 0; k < sizeof(speed_map) / sizeof(speed_map[0]); k++)
                if (abs(speed_map[k]) == j)
                    stream.info_width[k] = stream.info_simg[j][i].width;
        }
    }
    stream.info_fimg.map = info_read_png(INFO_FIMG, &stream.info_fimg.width, &stream.info_fimg.height);
    for (i = 0; i < sizeof(stream.info_oimg) / sizeof(stream.info_oimg[0]); i++)
    {
        char fn[128];
        snprintf(fn, sizeof(fn), INFO_OIMG, i);
        stream.info_oimg[i].map = info_read_png(fn, &stream.info_oimg[i].width, &stream.info_oimg[i].height);
    }

    // action setup PLAYER
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, INFO_MAT_X, INFO_MAT_Y, INFO_MAT_W, INFO_MAT_H, A_MAT};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, INFO_RESTORE_X, INFO_BOTTOM_Y, INFO_RESTORE_W, INFO_BOTTOM_H, A_RESTORE};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, INFO_MAT_X + (INFO_MAT_W + INFO_BORDER) + INFO_BORDER, INFO_MAT_Y, INFO_MAT_W, INFO_MAT_H, A_BOOKMARK_ADD};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, INFO_MAT_X + 2 * (INFO_MAT_W + INFO_BORDER) + INFO_BORDER, INFO_MAT_Y, INFO_MAT_W, INFO_MAT_H, A_BOOKMARK_SHOW};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, INFO_MAT_X + 3 * (INFO_MAT_W + INFO_BORDER) + 2 * INFO_BORDER, INFO_MAT_Y, INFO_MAT_W, INFO_MAT_H, A_MEDICAL_START};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, INFO_MAT_X + 4 * (INFO_MAT_W + INFO_BORDER) + 2 * INFO_BORDER, INFO_MAT_Y, INFO_MAT_W, INFO_MAT_H, A_MEDICAL_STOP};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, 0, 0, 5 * (INFO_MAT_W + INFO_BORDER) + 2 * INFO_BORDER, INFO_MAT_H + 2 * INFO_BORDER, A_NONE};

    for (i = 0; i < MAX_CAM; i++)
    {
        stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, INFO_CAM_X + i * (INFO_CAM_W + INFO_BORDER), INFO_BOTTOM_Y, INFO_CAM_W, INFO_BOTTOM_H, A_CAM, i};
    }
    x = INFO_SPEED_X - INFO_SPEED_BORDER / 2;
    for (i = 0; i < -SPEED_BCK_SKIP + SPEED_SKIP + 1; i++)
    {
        stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, x, INFO_BOTTOM_Y, stream.info_width[i] + INFO_SPEED_BORDER, INFO_BOTTOM_H, A_SPEED, i + SPEED_BCK_SKIP};
        x += stream.info_width[i] + INFO_SPEED_BORDER;
    }
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, 0, INFO_BOTTOM_Y - INFO_BORDER, INFO_WIDTH, INFO_MAT_H + 2 * INFO_BORDER, A_NONE};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_PLAYER, 0, 0, INFO_WIDTH, INFO_HEIGHT, A_MOVE};

    // action setup CONFIG
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_CONFIG, INFO_MAT_X, INFO_MAT_Y, INFO_MAT_W, INFO_MAT_H, A_CFG_END};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_CONFIG, INFO_PAUSE_X, INFO_PAUSE_Y, INFO_PAUSE_W, INFO_PAUSE_H, A_CFG_PAUSE};

    for (int mat = 0; mat < MAX_MAT; mat++)
    {
        x = INFO_CONFIG_X + mat * INFO_CONFIG_NEXT_X;
        y = INFO_CONFIG_Y;
        for (int cam = 0; cam < MAX_CAM; cam++)
        {
            uint32_t fx = x - INFO_BORDER / 2 - INFO_CONFIG_CAM_W + (cam % 2) * (INFO_BORDER + INFO_CONFIG_CAM_W),
                     fy = y + INFO_BORDER / 2 + INFO_MAT_H / 2 - (INFO_CONFIG_CAM_H + INFO_MAT_H + INFO_BORDER) * (cam / 2);
            stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_CONFIG, fx, fy, INFO_CONFIG_CAM_W, INFO_CONFIG_CAM_H, A_CFG_SELECT, (mat + 1) << 8 | (cam + 1)};
        }
    }
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_CONFIG, 0, 0, INFO_WIDTH, INFO_HEIGHT, A_NONE};

    // action setup BROWSE
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BROWSE, INFO_MAT_X, INFO_MAT_Y, INFO_MAT_W, INFO_MAT_H, A_CFG_END};
    i = 0;
    for (y = INFO_BROWSE_Y; y < INFO_HEIGHT - INFO_BROWSE_H + INFO_BORDER && i < MAX_BROWSE; y += INFO_BROWSE_H + INFO_BORDER)
        for (x = INFO_BROWSE_X; x < INFO_WIDTH - INFO_BROWSE_W + 2 * INFO_BORDER && i < MAX_BROWSE; x += INFO_BROWSE_W + 2 * INFO_BORDER)
        {
            stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BROWSE, x, y, INFO_BROWSE_W - INFO_BORDER - 50, INFO_BROWSE_H, A_BRS_SELECT, i};
            stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BROWSE, x + INFO_BROWSE_W - INFO_BORDER - 50, y, 50, INFO_BROWSE_H, A_BRS_DELETE, i};
            i++;
        }
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BROWSE, 0, 0, INFO_WIDTH, INFO_HEIGHT, A_NONE};

    // action setup BOOKMARKS
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BOOKMARKS, INFO_MAT_X, INFO_MAT_Y, INFO_MAT_W, INFO_MAT_H, A_MAT};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BOOKMARKS, INFO_RESTORE_X, INFO_BOTTOM_Y, INFO_RESTORE_W, INFO_BOTTOM_H, A_RESTORE};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BOOKMARKS, INFO_MAT_X + 2 * (INFO_MAT_W + INFO_BORDER) + INFO_BORDER, INFO_MAT_Y, INFO_MAT_W, INFO_MAT_H, A_BOOKMARK_SHOW};

    for (i = 0; i < MAX_CAM; i++)
    {
        stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BOOKMARKS, INFO_CAM_X + i * (INFO_CAM_W + INFO_BORDER), INFO_BOTTOM_Y, INFO_CAM_W, INFO_BOTTOM_H, A_CAM, i};
    }
    x = INFO_SPEED_X - INFO_SPEED_BORDER / 2;
    for (i = 0; i < -SPEED_BCK_SKIP + SPEED_SKIP + 1; i++)
    {
        stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BOOKMARKS, x, INFO_BOTTOM_Y, stream.info_width[i] + INFO_SPEED_BORDER, INFO_BOTTOM_H, A_SPEED, i + SPEED_BCK_SKIP};
        x += stream.info_width[i] + INFO_SPEED_BORDER;
    }

    i = 0;
    for (y = INFO_BOOKMARK_Y; y < INFO_HEIGHT - INFO_BOOKMARK_H + INFO_BORDER && i < MAX_BOOKMARKS; y += INFO_BOOKMARK_H + INFO_BORDER)
        for (x = INFO_BOOKMARK_X; x < INFO_WIDTH - INFO_BOOKMARK_W + INFO_BORDER && i < MAX_BOOKMARKS; x += INFO_BOOKMARK_W + INFO_BORDER)
        {
            stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BOOKMARKS, x, y, INFO_BOOKMARK_W - INFO_BORDER - 50, INFO_BOOKMARK_H, A_BOOKMARK_SELECT, i};
            stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BOOKMARKS, x + INFO_BOOKMARK_W - INFO_BORDER - 50, y, 50, INFO_BOOKMARK_H, A_BOOKMARK_DELETE, i};
            i++;
        }

    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_BOOKMARKS, 0, 0, INFO_WIDTH, INFO_HEIGHT, A_NONE};

    // empty gui
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_EMPTY, INFO_MAT_X, INFO_MAT_Y, INFO_MAT_W, INFO_MAT_H, A_MAT};
    stream.hid_actions[stream.hid_actionslen++] = (action_rect_t){GUI_EMPTY, 0, 0, INFO_WIDTH, INFO_HEIGHT, A_NONE};
}

void info_cleanup(void)
{
}

// +++ STREAM DECODER

static int read_buffer(void *opaque, uint8_t *buf, int buf_size)
{
    bd_t *bd = (bd_t *)opaque;
    buf_size = FFMIN(buf_size, bd->blen);
    if (!buf_size)
        return AVERROR_EOF;
    alarm(6);
    memcpy(buf, bd->b, buf_size);
    alarm(0);
    bd->b += buf_size;
    bd->blen -= buf_size;
    return buf_size;
}

static enum AVPixelFormat setup_get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;
    for (p = pix_fmts; *p != -1; p++)
    {
        if (*p == AV_PIX_FMT_DRM_PRIME)
            return *p;
    }
    ERR("failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

void stream_setup(uint8_t *bf, int bflen)
{

    AVIOContext *avio_ctx = NULL;
    uint8_t *avio_ctx_buffer = NULL;
    AVFormatContext *input_ctx = NULL;
    const AVCodec *decoder = NULL;

    stream.decode_bd.b = bf;
    stream.decode_bd.blen = FFMIN(4 * 1024, bflen);

    CAVNZ(input_ctx, avformat_alloc_context());
    CAVNZ(avio_ctx_buffer, av_malloc(4 * 1024));
    CAVNZ(avio_ctx, avio_alloc_context(avio_ctx_buffer, 4 * 1024, 0, &stream.decode_bd, &read_buffer, NULL, NULL));
    input_ctx->pb = avio_ctx;
    CAZ(avformat_open_input(&input_ctx, NULL, NULL, NULL));
    CAZ(avformat_find_stream_info(input_ctx, NULL));
    av_dump_format(input_ctx, 0, NULL, 0);
    CAVZP(stream.stream_index, av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0));
    CAVNZ(stream.decoder_ctx, avcodec_alloc_context3(decoder));
    CAZP(avcodec_parameters_to_context(stream.decoder_ctx, input_ctx->streams[stream.stream_index]->codecpar));
    stream.width = input_ctx->streams[stream.stream_index]->codecpar->width;
    stream.height = input_ctx->streams[stream.stream_index]->codecpar->height;
    stream.decoder_ctx->get_format = setup_get_hw_format;
    CAZP(av_hwdevice_ctx_create(&stream.hw_device_ctx, AV_HWDEVICE_TYPE_DRM, NULL, NULL, 0));
    stream.decoder_ctx->hw_device_ctx = av_buffer_ref(stream.hw_device_ctx);
    CAZP(avcodec_open2(stream.decoder_ctx, decoder, NULL));
    avformat_close_input(&input_ctx);
    av_freep(&avio_ctx->buffer);
    avio_context_free(&avio_ctx);

    stream.decode_bd.b = NULL;
    stream.decode_bd.blen = 0;
}

void stream_cleanup(void)
{
    if (stream.input_ctx && stream.avio_ctx)
    {
        avformat_close_input(&stream.input_ctx);
        av_freep(&stream.avio_ctx->buffer);
        avio_context_free(&stream.avio_ctx);
        stream.input_ctx = NULL;
        stream.avio_ctx = NULL;
        stream.decode_id = 0;
        stream.decode_ms = 0;
    }

    avcodec_free_context(&stream.decoder_ctx);
    av_buffer_unref(&stream.hw_device_ctx);
}

void stream_show_frame(AVFrame *frame)
{
    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)frame->data[0];

    assert(desc->nb_objects == 1);
    assert(desc->nb_layers == 1);

    if (!stream.display_initialized)
    {
        assert(frame->width == stream.width);
        assert(frame->height == stream.height);

        uint32_t pitches[DISP_MAX_PLANES], offsets[DISP_MAX_PLANES];
        memset(pitches, 0, sizeof(pitches));
        memset(offsets, 0, sizeof(offsets));
        for (int j = 0; j < desc->layers[0].nb_planes; j++)
        {
            offsets[j] = desc->layers[0].planes[j].offset;
            pitches[j] = desc->layers[0].planes[j].pitch;
        }

        disp_plane_setup(stream.vi, desc->layers[0].format, stream.width, stream.height, pitches, offsets, 2);

        CAZ(pthread_mutex_lock(&stream.scale_mutex));
        stream.hid_zoom = 0;
        scale_compute(true);
        CAZ(pthread_mutex_unlock(&stream.scale_mutex));

        disp_plane_scale(stream.vi, stream.hid_x, stream.hid_y, stream.hid_w, stream.hid_h, 0, 0, stream.crtc_width, stream.crtc_height);
        stream.display_initialized = true;
    }
    disp_plane_show_pic(stream.vi, desc->objects[0].fd);
}

bool stream_add_frame(AVFrame *frame, uint64_t ms, uint32_t id)
{
    // mutex held
    int i;
    DBG("D: ADD %lu/%d [%d]\n", ms, id, stream.frmlen);

    for (i = 0; i < stream.frmlen; i++)
        if ((ms < stream.frm[i].ms) || (ms == stream.frm[i].ms && id <= stream.frm[i].id))
        {
            if (ms == stream.frm[i].ms && id == stream.frm[i].id)
                return false;
            break;
        }
    memmove(stream.frm + i + 1, stream.frm + i, (stream.frmlen - i) * sizeof(stream.frm[0]));
    stream.frmlen++;
    assert(stream.frmlen < DISP_PICTURE_HANDLES);
    stream.frm[i].id = id;
    stream.frm[i].ms = ms;
    stream.frm[i].frame = frame;

    return true;
}

bool stream_remove_frame(uint64_t ms, uint32_t id)
{
    // mutex held
    DBG("D: REM %lu/%d [%d]\n", ms, id, stream.frmlen);
    if ((ms == stream.show_a_ms && id == stream.show_a_id) || (ms == stream.show_p_ms && id == stream.show_p_id))
    {
        // maybe from other stream
        DBG("D: NOT REM %lu/%d [%d]\n", ms, id, stream.frmlen);
        return false;
    }
    for (int i = 0; i < stream.frmlen; i++)
        if (ms == stream.frm[i].ms && id == stream.frm[i].id)
        {
            disp_plane_drop_pic(stream.vi, ((AVDRMFrameDescriptor *)stream.frm[i].frame->data[0])->objects[0].fd);
            av_frame_free(&stream.frm[i].frame);
            memmove(stream.frm + i, stream.frm + i + 1, (stream.frmlen - i - 1) * sizeof(stream.frm[0]));
            stream.frmlen--;
            break;
        }
    return true;
}

void stream_decode(uint8_t *bf, int bflen, uint64_t ms, uint32_t from, uint32_t to, uint32_t skip)
{
    uint8_t *avio_ctx_buffer;

    DBG("DECODE %lu/%d-%d enter\n", ms, from, to);

    assert(from <= STREAM_FRAMES && to <= STREAM_FRAMES);

    uint32_t _from = (from / skip * skip);
    if (_from < from)
        _from = ((from / skip + 1) * skip);
    to = ((to - 1) / skip * skip) + 1;

    LOG("DECODE %lu/%d-%d\n", ms, from, to);

    if (!(ms == stream.decode_ms && stream.decode_id <= from))
    {
        // start/restart decode
        // DBG("+++\n");
        if (stream.input_ctx && stream.avio_ctx)
        {
            avformat_close_input(&stream.input_ctx);
            av_freep(&stream.avio_ctx->buffer);
            avio_context_free(&stream.avio_ctx);
            stream.input_ctx = NULL;
            stream.avio_ctx = NULL;
        }
        // DBG("+++\n");
        assert(stream.stream_initialized);

        stream.decode_bd.b = bf;
        stream.decode_bd.blen = bflen;
        stream.decode_ms = ms;
        stream.decode_read_packet = true;
        stream.decode_id = 0;

        CAVNZ(stream.input_ctx, avformat_alloc_context());
        CAVNZ(avio_ctx_buffer, av_malloc(128 * 1024));
        CAVNZ(stream.avio_ctx, avio_alloc_context(avio_ctx_buffer, 128 * 1024, 0, &stream.decode_bd, &read_buffer, NULL, NULL));
        stream.input_ctx->pb = stream.avio_ctx;
        CAZ(avformat_open_input(&stream.input_ctx, NULL, NULL, NULL));
        // DBG("+++\n");
    }

    // DBG("+++\n");
    AVPacket packet = {};
    while (stream.decode_id < to)
    {
        if (stream.decode_read_packet)
        {
            if (av_read_frame(stream.input_ctx, &packet) < 0)
            {
                av_packet_unref(&packet);
                break;
            }
        }
        if (!stream.decode_read_packet || stream.stream_index == packet.stream_index)
        {
            // DBG("+++\n");

            if (stream.decode_read_packet)
                CAZ(avcodec_send_packet(stream.decoder_ctx, &packet));
            stream.decode_read_packet = false;
            while (stream.decode_id < to)
            {
                // DBG("+++\n");
                AVFrame *frame = NULL;
                CAVNZ(frame, av_frame_alloc());
                int ret = avcodec_receive_frame(stream.decoder_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                {
                    // DBG("+++\n");
                    av_frame_free(&frame);
                    stream.decode_read_packet = true;
                    break;
                }
                assert(!ret);
                assert(frame->format == AV_PIX_FMT_DRM_PRIME);
                if (stream.decode_id >= from && (stream.decode_id % skip == 0))
                {
                    // DBG("+++\n");
                    CAZ(pthread_mutex_lock(&stream.decoder_mutex));
                    if (!stream_add_frame(frame, ms, stream.decode_id))
                        av_frame_free(&frame);
                    CAZ(pthread_mutex_unlock(&stream.decoder_mutex));
                    // DBG("+++\n");
                }
                else
                    av_frame_free(&frame);
                stream.decode_id++;
            }
        }
        // DBG("+++\n");
        av_packet_unref(&packet);
    }
}

int compare_chunk(const void *a, const void *b)
{
    if (((const chunk_t *)a)->ms < ((const chunk_t *)b)->ms)
        return -1;
    if (((const chunk_t *)a)->ms > ((const chunk_t *)b)->ms)
        return 1;
    return 0;
}

void stream_map(uint64_t ms)
{
    char fn[256];

    if (stream.decoder_loader_ms == ms)
        return;
    if (stream.decoder_loader_ms)
    {
        CAZ(munmap(stream.decoder_loader_bd.b, stream.decoder_loader_bd.blen));
        close(stream.decoder_loader_fd);
    }

    stream.decoder_loader_ms = ms;
    chunk_t _ms = {ms, 0};
    chunk_t *ck = bsearch(&_ms, stream.ch, stream.chlen, sizeof(stream.ch[0]), compare_chunk);
    A(ck);
    snprintf(fn, sizeof(fn) - 1, "%s/" SRVF "/%s/" CAMF "/%lx.ts", stream.path, ck->srvid, stream.day, stream.camid, ms);
    LOG("L: loading %s\n", fn);

    alarm(6);
    CAVZP(stream.decoder_loader_fd, open(fn, O_RDONLY));
    CAVP(stream.decoder_loader_bd.blen, lseek(stream.decoder_loader_fd, 0, SEEK_END));
    CAV(stream.decoder_loader_bd.b, mmap(NULL, stream.decoder_loader_bd.blen, PROT_READ, MAP_PRIVATE, stream.decoder_loader_fd, 0), != MAP_FAILED);
    alarm(0);
}

static void *stream_decoder_thread(void *data)
{
    uint64_t prev_ms;
    int prev_id;

    LOG("DECODER THREAD START\n");
    DBG("D: frmlen %d\n", stream.frmlen);

    CAZ(pthread_mutex_lock(&stream.decoder_mutex));
    prev_ms = stream.show_ms;
    prev_id = stream.show_id;
    CAZ(pthread_mutex_unlock(&stream.decoder_mutex));

    while (!stream.stopping && !stream.switching)
    {
        int count = 0;              // frames
        int prev_count, next_count; // frm
        uint64_t ms;
        int id;
        int frmidx;

        CAZ(pthread_mutex_lock(&stream.decoder_mutex));

        while (!stream.stopping && !stream.switching && prev_ms == stream.show_ms && prev_id == stream.show_id && pthread_cond_wait(&stream.decoder_cond, &stream.decoder_mutex))
            ;

        if (stream.stopping || stream.switching)
        {
            CAZ(pthread_mutex_unlock(&stream.decoder_mutex));
            break;
        }

        prev_ms = ms = stream.show_ms;
        prev_id = id = stream.show_id;
        uint32_t skip = stream.show_skip;

        DBG("D: start request %lu/%d\n", ms, id);

        chunk_t *pms = bsearch(&ms, stream.ch, stream.chlen, sizeof(stream.ch[0]), compare_chunk);
        assert(pms);

        for (frmidx = 0; frmidx < stream.frmlen; frmidx++)
            if (ms == stream.frm[frmidx].ms && id == stream.frm[frmidx].id)
                break;

        // DBG("D: %d\n", stream.frmlen);

        if (frmidx < stream.frmlen)
        {
            // compute treshold
            int j;
            prev_count = frmidx, next_count = stream.frmlen - frmidx - 1;
            int frm_cnt = 0;

            if (stream.speed < 0)
            {
                for (j = 0; frmidx - j >= 0; j++)
                {
                    if (ms != stream.frm[frmidx - j].ms || id != stream.frm[frmidx - j].id)
                    {
                        /// DBG("D:1 %ld/%d exp %ld/%d\n", stream.frm[frmidx-j].ms, stream.frm[frmidx-j].id, ms, id);
                        if (!stream_remove_frame(stream.frm[frmidx - j].ms, stream.frm[frmidx - j].id))
                            continue;
                        frmidx--;
                        j--;
                        continue;
                    }
                    frm_cnt++;
                    id -= skip;
                    if (id < 0)
                    {
                        id = ((STREAM_FRAMES - 1) / skip) * skip;
                        if (pms - stream.ch > 0)
                            ms = (--pms)->ms;
                        else
                        {
                            ms = 0;
                            break;
                        }
                    }
                }
            }
            else
            {
                for (j = 0; frmidx + j < stream.frmlen; j++)
                {
                    if (ms != stream.frm[frmidx + j].ms || id != stream.frm[frmidx + j].id)
                    {
                        // DBG("D:2 %ld/%d exp %ld/%d\n", stream.frm[frmidx+j].ms, stream.frm[frmidx+j].id, ms, id);
                        if (!stream_remove_frame(stream.frm[frmidx + j].ms, stream.frm[frmidx + j].id))
                            continue;
                        j--;
                        continue;
                    }
                    // DBG("D:3 %ld/%d\n", ms, id);
                    frm_cnt++;
                    id += skip;
                    if (id >= STREAM_FRAMES)
                    {
                        id = 0;
                        if (pms - stream.ch < stream.chlen - 1)
                            ms = (++pms)->ms;
                        else
                        {
                            ms = 0;
                            break;
                        }
                    }
                }
            }
            // check threshold
            if (frm_cnt < FRAMES_TRESHOLD)
                count = FRAMES_PRELOAD * skip;
            else
                count = 0;
            // DBG("D:4 %d\n",count);
        }
        else
        {
            // miss actual load
            DBG("D: %ld/%d miss\n", ms, id);
            // restart
            int frm_cnt = 0;
            while (stream.frmlen > frm_cnt)
                if (!stream_remove_frame(stream.frm[stream.frmlen - 1 - frm_cnt].ms, stream.frm[stream.frmlen - 1 - frm_cnt].id))
                    frm_cnt++;
            count = (FRAMES_TRESHOLD)*skip;
            frmidx = prev_count = next_count = 0;
        }

        // free older frames
        if (count && ms)
        {
            if (stream.speed < 0)
            {
                for (int frm_cnt = 0, j = 1; frmidx + j < stream.frmlen; j++)
                {
                    if (stream.frm[frmidx + j].id % skip != 0 || frm_cnt > FRAMES_TRESHOLD / 2)
                    {
                        DBG("D:5 %ld/%d\n", stream.frm[frmidx + j].ms, stream.frm[frmidx + j].id);
                        if (!stream_remove_frame(stream.frm[frmidx + j].ms, stream.frm[frmidx + j].id))
                            continue;
                        j--;
                        continue;
                    }
                    frm_cnt++;
                }
            }
            else
            {
                for (int frm_cnt = 0, j = 1; frmidx - j >= 0; j++)
                {
                    if (stream.frm[frmidx + j].id % skip != 0 || frm_cnt > FRAMES_TRESHOLD / 2)
                    {
                        DBG("D:6 %ld/%d\n", stream.frm[frmidx - j].ms, stream.frm[frmidx - j].id);
                        if (!stream_remove_frame(stream.frm[frmidx - j].ms, stream.frm[frmidx - j].id))
                            continue;
                        frmidx--;
                        j--;
                        continue;
                    }
                    frm_cnt++;
                }
            }
        }

        CAZ(pthread_mutex_unlock(&stream.decoder_mutex));

        // cap count to video fragment size
        if (count > STREAM_FRAMES)
            count = STREAM_FRAMES;

        if (count && ms)
        {
            DBG("D:7 LOAD %ld/%d %d %d\n", ms, id, count, stream.speed);
            stream_map(ms);
            if (!stream.stream_initialized)
            {
                stream_setup(stream.decoder_loader_bd.b, stream.decoder_loader_bd.blen);
                stream.decode_ms = 0;
                stream.decode_id = 0;
                stream.stream_initialized = true;
            }

            if (stream.speed < 0)
            {
                while (1)
                {
                    if (id - count < 0)
                    {
                        stream_decode(stream.decoder_loader_bd.b, stream.decoder_loader_bd.blen, ms, 0, id + 1, skip);
                        count -= id;
                        if (count < FRAMES_PRELOAD)
                            break; // minimum load
                        id = STREAM_FRAMES - 1;
                        if (pms - stream.ch > 0)
                        {
                            ms = (--pms)->ms;
                            stream_map(ms);
                        }
                        else
                        {
                            ms = 0;
                            break;
                        }
                    }
                    else
                    {
                        stream_decode(stream.decoder_loader_bd.b, stream.decoder_loader_bd.blen, ms, id - count, id + 1, skip);
                        break;
                    }
                }
            }
            else
            {
                while (1)
                {
                    if (id + count > STREAM_FRAMES)
                    {
                        stream_decode(stream.decoder_loader_bd.b, stream.decoder_loader_bd.blen, ms, id, STREAM_FRAMES, skip);
                        count -= (STREAM_FRAMES - id);
                        id = 0;
                        if (pms - stream.ch < stream.chlen - 1)
                        {
                            ms = (++pms)->ms;
                            stream_map(ms);
                        }
                        else
                        {
                            ms = 0;
                            break;
                        }
                    }
                    else
                    {
                        stream_decode(stream.decoder_loader_bd.b, stream.decoder_loader_bd.blen, ms, id, id + count, skip);
                        break;
                    }
                }
            }
        }
    }
    CAZ(pthread_mutex_lock(&stream.decoder_mutex));
    int frm_cnt = 0;
    while (stream.frmlen > frm_cnt)
        if (!stream_remove_frame(stream.frm[stream.frmlen - 1 - frm_cnt].ms, stream.frm[stream.frmlen - 1 - frm_cnt].id))
            frm_cnt++;
    CAZ(pthread_mutex_unlock(&stream.decoder_mutex));

    if (stream.decoder_loader_ms)
    {
        CAZ(munmap(stream.decoder_loader_bd.b, stream.decoder_loader_bd.blen));
        close(stream.decoder_loader_fd);
        stream.decoder_loader_ms = 0;
    }

    LOG("DECODER THREAD END\n");
    return NULL;
}

static void *stream_loader_thread(void *data)
{
    LOG("LOADER THREAD START\n");

    chunk_t ch[STREAM_MAX_FILES];
    uint32_t chlen;

    assert(stream.chlen == 0);

    struct timespec prev_ts;
    clock_gettime(CLOCK_REALTIME, &prev_ts);

    while (!stream.stopping && !stream.switching)
    {
        struct timespec a_ts;

        struct _u_request request;
        struct _u_response response;
        json_t *json;
        char _cam[4];

        CAP(snprintf(_cam, sizeof(_cam), "%u", stream.camid));

        ulfius_init_request(&request);
        ulfius_init_response(&response);
        CAZ(ulfius_set_request_properties(&request,
                                          U_OPT_HTTP_VERB, "GET",
                                          U_OPT_HTTP_URL, stream.masteruri,
                                          U_OPT_HTTP_URL_APPEND, "/chunks/",
                                          U_OPT_HTTP_URL_APPEND, stream.day,
                                          U_OPT_HTTP_URL_APPEND, "/",
                                          U_OPT_HTTP_URL_APPEND, _cam,
                                          U_OPT_TIMEOUT, 10ul,
                                          U_OPT_NONE));
        CAZ(ulfius_send_http_request(&request, &response));
        int status = response.status;
        CAVNZ(json, ulfius_get_json_body_response(&response, NULL));
        ulfius_clean_response(&response);
        ulfius_clean_request(&request);

        chlen = 0;

        if (status / 100 == 2)
        {
            A(json_is_array(json));
            for (int i = 0; i < json_array_size(json); i++)
            {
                json_int_t srvid = json_integer_value(json_object_get(json_array_get(json, i), "srvid"));

                json_t *tss = json_object_get(json_array_get(json, i), "ts");
                A(json_is_array(tss));
                size_t len = json_array_size(tss);
                A(chlen + len < STREAM_MAX_FILES);
                for (int j = 0; j < len; j++)
                {
                    CAV(ch[chlen].ms, strtoull(json_string_value(json_array_get(tss, j)), NULL, 16), != ULLONG_MAX);
                    ch[chlen].srvid = srvid;
                    chlen++;
                }
            }
            json_decref(json);
        }

        if (!chlen)
        {
            LOG("LD: not ready %s\n", stream.day);
            disp_plane_hide(stream.vi);
            usleep(LOOP_USLEEP);
            continue;
        }
        qsort(ch, chlen, sizeof(ch[0]), compare_chunk);

        CAZ(pthread_mutex_lock(&stream.decoder_mutex));
        if (memcmp(stream.ch, ch, sizeof(chunk_t) * stream.chlen))
            stream.show_msec_seek = stream.show_msec;
        memcpy(stream.ch, ch, sizeof(chunk_t) * chlen);
        stream.chlen = chlen;

        LOG("L: refresh %ld.%ld chunks %d\n", prev_ts.tv_sec, prev_ts.tv_sec / NS_IN_MSEC, chlen);

        while (!stream.stopping && !stream.switching && !clock_gettime(CLOCK_REALTIME, &a_ts) && a_ts.tv_sec == prev_ts.tv_sec && !pthread_cond_wait(&stream.decoder_cond, &stream.decoder_mutex))
            ;
        prev_ts = a_ts;
        CAZ(pthread_mutex_unlock(&stream.decoder_mutex));
    }

    LOG("LOADER THREAD END\n");
    return NULL;
}

// +++ SHOW

void *stream_show_thread(void *param)
{
    LOG("SHOW THREAD START\n");

    assert(stream.show_id == 0 && stream.show_ms == 0);

    while (!stream.chlen && !stream.stopping && !stream.switching)
        usleep(5000);

    struct timespec prev_ts;
    clock_gettime(CLOCK_REALTIME, &prev_ts);
    if (!stream.show_msec_seek && !stream.stopping && !stream.switching)
    {
        if (stream.show_msec)
            stream.show_msec_seek = stream.show_msec; // continue from last
        else
            stream.show_msec_seek = prev_ts.tv_sec * 1000 + prev_ts.tv_nsec / NS_IN_MSEC - 5000; // start from t-5sec
    }

    while (!stream.stopping && !stream.switching)
    {
        AVFrame *frame = NULL;
        uint64_t frame_wait;

        CAZ(pthread_mutex_lock(&stream.decoder_mutex));

    reload:
        if (stream.show_msec_seek)
        {
            stream.show_msec = stream.show_msec_seek;
            stream.show_msec_seek = 0;

            chunk_t *lms = stream.ch, *rms = stream.ch + stream.chlen - 1;
            // find nearest low
            while (lms < rms)
            {
                DBG("S:1 %ld %ld %u | %lu %lu %lu\n", lms - stream.ch, rms - stream.ch, stream.chlen, lms->ms, stream.show_msec, rms->ms);
                chunk_t *mms = lms + ((rms - lms) / 2);
                if (stream.show_msec < mms->ms)
                    rms = mms - 1;
                else
                    lms = mms + 1;
            }
            if (stream.show_msec < lms->ms && lms - stream.ch > 0)
                lms--;
            DBG("S:2 %ld %ld %u | %lu %lu %lu\n", lms - stream.ch, rms - stream.ch, stream.chlen, lms->ms, stream.show_msec, rms->ms);

            stream.show_ms = lms->ms;
            if (stream.show_msec < lms->ms || (stream.show_msec - lms->ms) > STREAM_FRAMES * STREAM_FPS_MSEC)
                stream.show_id = 0;
            else
                stream.show_id = ((stream.show_msec - lms->ms) / STREAM_FPS_MSEC) / stream.show_skip * stream.show_skip;
            DBG("S:3 start search %lu/%d == %ld == %ld ~ %ld\n", stream.show_ms, stream.show_id, stream.show_ms + stream.show_id * STREAM_FPS_MSEC, stream.show_msec, stream.show_ms + stream.show_id * STREAM_FPS_MSEC - stream.show_msec);
        }

        for (int i = 0; i < stream.frmlen; i++)
            if (stream.show_ms == stream.frm[i].ms && stream.show_id == stream.frm[i].id)
            {
                stream.show_p_id = stream.show_a_id;
                stream.show_p_ms = stream.show_a_ms;
                stream.show_a_id = stream.show_id;
                stream.show_a_ms = stream.show_ms;

                // frame to show ready
                frame = stream.frm[i].frame;
                frame_wait = stream.show_wait;

                // compute next frame
                chunk_t _show_ms = {stream.show_ms, 0};
                chunk_t *pms = bsearch(&_show_ms, stream.ch, stream.chlen, sizeof(stream.ch[0]), compare_chunk);
                if (!pms)
                {
                    stream.show_msec_seek = stream.show_msec;
                    goto reload;
                }

                if (stream.speed > 0)
                {
                    if (stream.show_id + stream.show_skip >= STREAM_FRAMES)
                    {
                        if ((pms + 1 - stream.ch) < stream.chlen)
                        {
                            stream.show_id = 0;
                            stream.show_ms = (pms + 1)->ms;
                        } // else stop (end reached) and wait for new chunk
                    }
                    else
                        stream.show_id += stream.show_skip;
                }
                else if (stream.speed < 0)
                {
                    if (stream.show_id < stream.show_skip)
                    {
                        if ((pms - 1 - stream.ch) >= 0)
                        {
                            stream.show_id = (stream.show_id + STREAM_FRAMES - stream.show_skip) / stream.show_skip * stream.show_skip;
                            stream.show_ms = (pms - 1)->ms;
                        } // else stop (begin reached)
                    }
                    else
                        stream.show_id -= stream.show_skip;
                } // ignore SPEED_PAUSE
                break;
            }
        CAZ(pthread_cond_broadcast(&stream.decoder_cond));
        CAZ(pthread_mutex_unlock(&stream.decoder_mutex));

        if (frame)
        {
            DBG("S: frame start %ld/%d\n", stream.show_a_ms, stream.show_a_id);
            // wait for showtime
            CAZ(pthread_mutex_lock(&stream.info_mutex));
            uint64_t ns = (stream.show_ms % 1000) * 1000000 + 1000000l * stream.show_id * STREAM_FPS_MSEC;
            stream.info_time.tv_nsec = ns % 1000000000;
            stream.info_time.tv_sec = (stream.show_ms / 1000) + ns / 1000000000;
            normalize_ts(&stream.info_time);
            CAZ(pthread_mutex_unlock(&stream.info_mutex));

            struct timespec a_ts, sleep_ts;
            clock_gettime(CLOCK_REALTIME, &a_ts);

            frame_wait -= STREAM_FPS_NSEC;
            prev_ts.tv_nsec += STREAM_FPS_NSEC;
            normalize_ts(&prev_ts);
            sleep_ts.tv_sec = prev_ts.tv_sec - a_ts.tv_sec;
            sleep_ts.tv_nsec = prev_ts.tv_nsec - a_ts.tv_nsec;
            normalize_ts(&sleep_ts);
            DBG("S: sleep p:%ld.%03ld a:%ld.%03ld s:%ld.%03ld\n", prev_ts.tv_sec, prev_ts.tv_nsec / 1000000, a_ts.tv_sec, a_ts.tv_nsec / 1000000, sleep_ts.tv_sec, sleep_ts.tv_nsec / 1000000);
            if (sleep_ts.tv_sec >= 0 && sleep_ts.tv_nsec > 0)
            {
                while (nanosleep(&sleep_ts, &a_ts) < 0)
                {
                    assert(errno == EINTR);
                    sleep_ts = a_ts;
                }
            }

            DBG("S: frame show %lu/%d\n", stream.show_a_ms, stream.show_a_id);
            stream.show_msec = stream.show_a_ms + stream.show_a_id * STREAM_FPS_MSEC;
            stream_show_frame(frame);

            if (frame_wait > 0)
            {
                clock_gettime(CLOCK_REALTIME, &a_ts);

                prev_ts.tv_nsec += frame_wait;
                normalize_ts(&prev_ts);
                sleep_ts.tv_sec = prev_ts.tv_sec - a_ts.tv_sec;
                sleep_ts.tv_nsec = prev_ts.tv_nsec - a_ts.tv_nsec;
                normalize_ts(&sleep_ts);

                DBG("S: sleep loger p:%ld.%03ld a:%ld.%03ld s:%ld.%03ld\n", prev_ts.tv_sec, prev_ts.tv_nsec / 1000000, a_ts.tv_sec, a_ts.tv_nsec / 1000000, sleep_ts.tv_sec, sleep_ts.tv_nsec / 1000000);
                if (sleep_ts.tv_sec >= 0 && sleep_ts.tv_nsec > 0)
                {
                    while (nanosleep(&sleep_ts, &a_ts) < 0)
                    {
                        assert(errno == EINTR);
                        sleep_ts = a_ts;
                    }
                }
            }

            if (sleep_ts.tv_sec < 0 || sleep_ts.tv_nsec < -2 * NS_IN_MSEC)
            {
                prev_ts = a_ts;
                LOG("S: sync %ld.%03ld\n", sleep_ts.tv_sec, sleep_ts.tv_nsec / NS_IN_MSEC);
            }
        }
        else
        {
            DBG("S: wait 10ms\n");
            struct timespec a_ts, sleep_ts;
            sleep_ts.tv_sec = 0;
            sleep_ts.tv_nsec = 10 * NS_IN_MSEC;
            normalize_ts(&sleep_ts);
            while (nanosleep(&sleep_ts, &a_ts) < 0)
            {
                assert(errno == EINTR);
                sleep_ts = a_ts;
            }
        }
    }

    CAZ(pthread_mutex_lock(&stream.decoder_mutex));
    CAZ(pthread_cond_broadcast(&stream.decoder_cond));
    CAZ(pthread_mutex_unlock(&stream.decoder_mutex));

    LOG("SHOW THREAD END\n");
    return NULL;
}

// +++ COMMANDER

void *stream_cmd_thread(void *param)
{
    LOG("COMMANDER THREAD START\n");
    uint32_t speed_prev = ~0;
    uint32_t speed_bigskip = 4;

    stream.switching = true;

    while (!stream.stopping)
    {
        uint32_t wt = 0;

        CAZ(pthread_mutex_lock(&stream.cmd_mutex));

        if (stream.speed != speed_prev)
        {
            CAZ(pthread_mutex_lock(&stream.decoder_mutex));
            if (stream.show_skip != speed_params[abs(stream.speed)].skip)
                stream.show_id = stream.show_id / speed_params[abs(stream.speed)].skip * speed_params[abs(stream.speed)].skip;
            stream.show_skip = speed_params[abs(stream.speed)].skip;
            stream.show_wait = speed_params[abs(stream.speed)].wait;
            speed_bigskip = 4;
            LOG("C: change speed %d/%ld (new %d)\n", stream.show_skip, stream.show_wait / NS_IN_MSEC, stream.show_id);
            CAZ(pthread_mutex_unlock(&stream.decoder_mutex));
            speed_prev = stream.speed;
        }

        if (abs(stream.speed) == SPEED_SKIP)
        {
            CAZ(pthread_mutex_lock(&stream.decoder_mutex));
            if (stream.show_wait > STREAM_FPS_NSEC)
            {
                stream.show_wait -= SKIP_SPEEDUP;
                wt = stream.show_wait + STREAM_FPS_NSEC;
                DBG("C: boost skip %ld\n", stream.show_wait / NS_IN_MSEC);
            }
            else
            {
                speed_bigskip++;
                wt = stream.show_wait = STREAM_FPS_NSEC * 2;
                chunk_t _show_ms = {stream.show_ms, 0};
                chunk_t *pms = bsearch(&_show_ms, stream.ch, stream.chlen, sizeof(stream.ch[0]), compare_chunk);
                if (pms)
                {
                    if (stream.speed == -SPEED_SKIP)
                    {
                        pms -= speed_bigskip / 2;
                        if (pms < stream.ch)
                            pms = stream.ch;
                    }
                    else
                    {
                        pms += speed_bigskip / 2;
                        if (pms > stream.ch + stream.chlen)
                            pms = stream.ch + stream.chlen - 1;
                    }
                    stream.show_msec_seek = pms->ms;
                }
            }
            CAZ(pthread_mutex_unlock(&stream.decoder_mutex));
        }
        if (stream.camid != stream.camid_switch)
        {
            // switch stream
            if (!stream.switching)
            {
                stream.switching = true;
                CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

                CAZ(pthread_join(stream.show_tid, NULL));
                CAZ(pthread_mutex_lock(&stream.info_mutex));
                stream.info_time.tv_sec = 0;
                stream.info_time.tv_nsec = 0;
                CAZ(pthread_mutex_unlock(&stream.info_mutex));
                CAZ(pthread_join(stream.decoder_tid, NULL));
                CAZ(pthread_join(stream.loader_tid, NULL));

                CAZ(pthread_mutex_lock(&stream.cmd_mutex));
            }

            stream.camid = stream.camid_switch;
            if (!stream.camid)
                stream.show_msec_seek = stream.show_msec = 0;

            config_t *config = get_config(stream.camid);
            if (config)
            {
                LOG("C: switch stream mat %d cam %d day %s\n", config->mat, config->position, stream.day);
                if (stream.info_loadedmat != config->mat)
                {
                    CAZ(pthread_mutex_lock(&stream.info_mutex));
                    stream.info_loadedmat = stream.info_bookmarkslen = stream.info_medicalslen = 0;
                    CAZ(pthread_mutex_unlock(&stream.info_mutex));
                    mat_load(config->mat);
                }

                stream.switching = false;
                stream.show_id = stream.show_ms = stream.chlen = 0;
                CAZ(pthread_create(&stream.loader_tid, NULL, stream_loader_thread, NULL));
                CAZ(pthread_create(&stream.decoder_tid, NULL, stream_decoder_thread, NULL));
                CAZ(pthread_create(&stream.show_tid, NULL, stream_show_thread, NULL));
            }
            else
                disp_plane_hide(stream.vi);
        }

        if (wt)
        {
            struct timespec tv;
            clock_gettime(CLOCK_REALTIME, &tv);
            tv.tv_nsec += wt;
            normalize_ts(&tv);
            pthread_cond_timedwait(&stream.cmd_cond, &stream.cmd_mutex, &tv);
        }
        else
            CAZ(pthread_cond_wait(&stream.cmd_cond, &stream.cmd_mutex));

        CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
    }

    if (!stream.switching)
    {
        stream.switching = true;
        CAZ(pthread_join(stream.show_tid, NULL));
        CAZ(pthread_join(stream.decoder_tid, NULL));
        CAZ(pthread_join(stream.loader_tid, NULL));
    }

    LOG("COMMANDER THREAD END\n");
    return NULL;
}

void sig_handler(int signum)
{
    LOG("SIGNAL %d\n", signum);
    stream.stopping++;
    exit(1);
}

int main(int argc, char **argv)
{

    assert(SPEED_PAUSE == 0);

    ////////////////////////////////// PARAMETER SETUP

    printf("VERSION %s\n", VERSION);

    if (argc < 3 || argc > 4)
    {
        printf("usage: %s data_path master_uri [year-month-day]\n", argv[0]);
        return 1;
    }

    LOG("BUILDTIME %s\n", __TIMESTAMP_ISO__);

    ////////////////////////////////// SETUP

    signal(SIGINT, sig_handler);
    signal(SIGPIPE, sig_handler);
    signal(SIGALRM, sig_handler);

    // initial params
    strncpy(stream.path, argv[1], sizeof(stream.path) - 1);
    strncpy(stream.masteruri, argv[2], sizeof(stream.masteruri) - 1);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    strftime(stream.actualday, sizeof(stream.actualday), "%Y-%m-%d", localtime(&ts.tv_sec));
    if (argc == 4)
        strncpy(stream.day, argv[3], sizeof(stream.day) - 1);
    else
        strcpy(stream.day, stream.actualday);

    if (strcmp(stream.day, stream.actualday))
        stream.config_gui = GUI_BROWSE;
    else
        stream.config_gui = GUI_CONFIG;

    char hn[HOST_NAME_MAX];
    CAZ(gethostname(hn, sizeof(hn)));
    CAZ(strncmp(hn, "player", sizeof("player") - 1));
    strncat(stream.playerid, hn + sizeof("player") - 1, sizeof(stream.playerid) - 1);

    // initicialization
    CAZ(pthread_mutex_init(&stream.rest_mutex, NULL));
    CAZ(pthread_mutex_init(&stream.cmd_mutex, NULL));
    CAZ(pthread_cond_init(&stream.cmd_cond, NULL));
    CAZ(pthread_mutex_init(&stream.info_mutex, NULL));
    CAZ(pthread_mutex_init(&stream.decoder_mutex, NULL));
    CAZ(pthread_cond_init(&stream.decoder_cond, NULL));
    CAZ(pthread_mutex_init(&stream.scale_mutex, NULL));
    CAZ(pthread_cond_init(&stream.scale_cond, NULL));

    disp_setup(NULL, &stream.vi, &stream.ui, &stream.crtc_width, &stream.crtc_height);
    hid_setup(NULL);
    if (INFO_DRAW_FINGER)
        for (int i = 0; i < MT_FINGERS; i++)
            stream.hid_touch[i].track = -1;
    info_setup();

    CAZ(pthread_create(&stream.info_tid, NULL, stream_info_thread, NULL));
    CAZ(pthread_create(&stream.cmd_tid, NULL, stream_cmd_thread, NULL));
    CAZ(pthread_create(&stream.scale_tid, NULL, stream_scale_thread, NULL));

    srand((unsigned int)time(NULL));

    CAZ(pthread_mutex_lock(&stream.cmd_mutex));
    get_player_cam();
    CAZ(pthread_cond_signal(&stream.cmd_cond));
    CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

    while (!stream.stopping)
    {
        if ((stream.gui == GUI_PLAYER || stream.gui == GUI_CONFIG || stream.gui == GUI_BOOKMARKS) && stream.camid == stream.camid_switch)
        {
            CAZ(pthread_mutex_lock(&stream.cmd_mutex));

            info_cfg_load();
            config_t *config = get_config(stream.camid);
            if (!config)
            {
                stream.camid_switch = 0;
                stream.info_changed = true;
            }
            else if (stream.gui == GUI_PLAYER || stream.gui == GUI_BOOKMARKS)
                mat_load(config->mat);

            CAZ(pthread_cond_signal(&stream.cmd_cond));
            CAZ(pthread_mutex_unlock(&stream.cmd_mutex));
        }

        if (stream.gui == GUI_CONFIG)
        {
            struct _u_request request;
            struct _u_response response;
            json_t *json;
            ulfius_init_request(&request);
            ulfius_init_response(&response);
            CAZ(ulfius_set_request_properties(&request,
                                              U_OPT_HTTP_VERB, "GET",
                                              U_OPT_HTTP_URL, stream.masteruri,
                                              U_OPT_HTTP_URL_APPEND, "/recording",
                                              U_OPT_TIMEOUT, 10ul,
                                              U_OPT_NONE));
            CAZ(ulfius_send_http_request(&request, &response));
            CAVNZ(json, ulfius_get_json_body_response(&response, NULL));
            ulfius_clean_response(&response);
            ulfius_clean_request(&request);

            int recording;
            CAZ(json_unpack(json, "{s:b}", "recording", &recording));
            json_decref(json);

            if (stream.info_paused != !recording)
            {
                stream.info_paused = !recording;
                stream.info_changed = true;
            }
        }

        stream.info_touch = hid_ping();
        if (!stream.info_touch)
        {
            stream.hid_absfd = 0;
        }
        usleep(LOOP_USLEEP);
    }

    stream.stopping++;

    CAZ(pthread_mutex_lock(&stream.cmd_mutex));
    CAZ(pthread_cond_signal(&stream.cmd_cond));
    CAZ(pthread_mutex_unlock(&stream.cmd_mutex));

    CAZ(pthread_mutex_lock(&stream.scale_mutex));
    CAZ(pthread_cond_signal(&stream.scale_cond));
    CAZ(pthread_mutex_unlock(&stream.scale_mutex));

    DBG("JOIN\n");
    CAZ(pthread_join(stream.scale_tid, NULL));
    CAZ(pthread_join(stream.cmd_tid, NULL));
    CAZ(pthread_join(stream.info_tid, NULL));

    stream_cleanup();
    hid_cleanup();
    info_cleanup();
    disp_cleanup();
}
