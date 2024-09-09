/*
SPDX-License-Identifier: MPL-2.0
SPDX-FileCopyrightText: 2023 Martin Cerveny <martin@c-home.cz>
*/

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <strings.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netdb.h>
#include <dirent.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <sys/time.h>
#include <pthread.h>
#include <assert.h>

#include "globals.h"
#include "hid.h"
#include "event_defs.h"

#undef DBG
#define DBG(...)

typedef struct hid_mon
{
    struct hid_mon *next;
    pthread_t tid;
    int fd;
    char name[512];
} hid_mon_t;

struct
{
    bool run;
    pthread_mutex_t mutex;

    hid_mon_t *hid_mons;
} hid;

static void bit_to_str(uint32_t bits, char *out, int out_size, const char *(*fn)(uint8_t))
{
    int i;
    uint8_t first = 1;
    const char *str;
    *out = 0;

    for (i = 0; i < 32; i++)
    {
        if (bits & (1 << i))
        {
            if (first)
                first = 0;
            else
                strncat(out, " ", out_size);
            str = fn(i);
            strncat(out, str == NULL ? "?" : str, out_size);
        }
    }
}

static const char *get_hid_name(uint8_t index)
{
    if (index > sizeof(event_name) / sizeof(event_name[0]))
        return NULL;
    return event_name[index];
}

static void *hid_mon_thread(void *data)
{
    hid_mon_t *hid_mon = (hid_mon_t *)data;

    char name[256];
    char phys[256];

    if (ioctl(hid_mon->fd, EVIOCGNAME(sizeof(name)), name) < 0)
    {
        ERR("evdev ioctl %s\n", strerror(errno));
    }
    LOG("event[%d] name %s\n", hid_mon->fd, name);

    if (ioctl(hid_mon->fd, EVIOCGPHYS(sizeof(phys)), phys) < 0)
    {
        ERR("event ioctl %s\n", strerror(errno));
    }
    LOG("event[%d] phys %s\n", hid_mon->fd, phys);

    char evtype_b[8];
    memset(evtype_b, 0, sizeof(evtype_b));
    if (ioctl(hid_mon->fd, EVIOCGBIT(0, EV_MAX), evtype_b) < 0)
    {
        ERR("evdev ioctl %s\n", strerror(errno));
    }
    char events[256];
    bit_to_str(*((int *)(&evtype_b)), events, sizeof(events) - 1, get_hid_name);
    LOG("event[%d] event %s\n", hid_mon->fd, events);

    struct input_event ev;
    int ret;
    while ((ret = (read(hid_mon->fd, &ev, sizeof(ev)))) != 0)
    {
        if (ret == -1)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        switch (ev.type)
        {
        case EV_KEY:
            DBG("EV_KEY: %d (%s) %d\n", ev.code, (key_name[ev.code] ? key_name[ev.code] : ""), ev.value);
            break;
        case EV_REL:
            DBG("EV_REL: %d %d\n", ev.code, ev.value);
            break;
        case EV_ABS:
            DBG("EV_ABS: %s %d\n", abs_code(ev.code), ev.value);
            break;
        default:
            DBG("%s: %d %d\n", event_name[ev.type], ev.code, ev.value);
            break;
        };
        hid_event(hid_mon->fd, &ev);
    }
    LOG("event[%d] exit\n", (int)hid_mon->tid);

    pthread_mutex_lock(&hid.mutex);
    if (hid_mon == hid.hid_mons)
        hid.hid_mons = hid_mon->next;
    else
    {
        hid_mon_t *_hid_mon = hid.hid_mons, *_prev_hid_mon = NULL;
        while (_hid_mon)
        {
            if (_hid_mon == hid_mon)
                break;
            _prev_hid_mon = _hid_mon;
            _hid_mon = _hid_mon->next;
        }
        assert(_hid_mon);
        _prev_hid_mon->next = _hid_mon->next;
    }
    pthread_mutex_unlock(&hid.mutex);

    close(hid_mon->fd);
    free(hid_mon);

    return NULL;
}

static void hid_check_and_open()
{
    if (!hid.run)
        return;

    DIR *dir = opendir("/dev/input");
    if (dir != NULL)
    {
        struct dirent *dent;

        pthread_mutex_lock(&hid.mutex);
        while ((dent = readdir(dir)) != NULL)
        {
            if (!strncmp("event", dent->d_name, sizeof("event") - 1))
            {
                char name[512];
                snprintf(name, sizeof(name) - 1, "/dev/input/%s", dent->d_name);

                // check if exists
                hid_mon_t *hid_mon = hid.hid_mons;
                while (hid_mon)
                {
                    if (!strcmp(name, hid_mon->name))
                        break;
                    hid_mon = hid_mon->next;
                }
                if (hid_mon)
                    continue;

                // add new
                hid_mon = malloc(sizeof(hid_mon_t));
                strncpy(hid_mon->name, name, sizeof(hid_mon->name) - 1);
               
                hid_mon->fd = open(hid_mon->name, O_RDWR);
                if (hid_mon->fd < 0)
                {
                    ERR("open() %s\n", hid_mon->name);
                    free(hid_mon);
                    continue;
                }
                if (pthread_create(&hid_mon->tid, NULL, hid_mon_thread, hid_mon))
                {
                    close(hid_mon->fd);
                    free(hid_mon);
                    ERR("pthread_create() %s\n", name);
                }
                hid_mon->next = hid.hid_mons;
                hid.hid_mons = hid_mon;
                LOG("START event[%d] %s\n", hid_mon->fd, name);
            }
        }
        pthread_mutex_unlock(&hid.mutex);
        closedir(dir);
    }
}

int hid_setup(char *cmd_param)
{
    hid.run = true;
    pthread_mutex_init(&hid.mutex, NULL);
    hid_check_and_open();

    return 0;
}

void hid_cleanup(void)
{
    hid.run = false;

    pthread_mutex_lock(&hid.mutex);
    while (hid.hid_mons)
    {
        hid_mon_t *hid_mon = hid.hid_mons;
        hid.hid_mons = hid_mon->next;
        LOG("STOP event[%d]\n", hid_mon->fd);
        pthread_cancel(hid_mon->tid);
        close(hid_mon->fd);
        free(hid_mon);
    }
    pthread_mutex_unlock(&hid.mutex);

    pthread_mutex_destroy(&hid.mutex);
}

void hid_ping(void)
{
    hid_check_and_open();
}
