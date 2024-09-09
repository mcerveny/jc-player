/*
SPDX-License-Identifier: MPL-2.0
SPDX-FileCopyrightText: 2023 Martin Cerveny <martin@c-home.cz>
*/


#ifndef _DISP_H_
#define _DISP_H_

#define DISP_MAX_PLANES 4
#define DISP_PICTURE_HANDLES 64

typedef struct plane plane_t;

void disp_setup(char *cmd_param, plane_t **vi, plane_t **ui, uint32_t *crtc_width, uint32_t *crtc_height);
void disp_cleanup(void);
void disp_wait(void);

void disp_plane_setup(plane_t *plane, uint32_t format, uint32_t width, uint32_t height, uint32_t pitches[DISP_MAX_PLANES], uint32_t offsets[DISP_MAX_PLANES], uint32_t zpos);
void disp_plane_scale(plane_t *plane, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t fb_x, uint32_t fb_y, uint32_t fb_width, uint32_t fb_height);
void disp_plane_show_pic(plane_t *plane, uint32_t prime_fd);
void disp_plane_drop_pic(plane_t *plane, uint32_t prime_fd);
void disp_plane_create(plane_t *plane, uint32_t format, uint32_t width, uint32_t height, uint32_t bpp, int *prime_fd, uint32_t *pitch, uint32_t *size, uint32_t **map);
void disp_plane_hide(plane_t *plane);

#endif
