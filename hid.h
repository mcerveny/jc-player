/*
SPDX-License-Identifier: MPL-2.0
SPDX-FileCopyrightText: 2023 Martin Cerveny <martin@c-home.cz>
*/

#ifndef _HID_H_
#define _HID_H_

#include <linux/input.h>

int hid_setup(char *cmd_param);
void hid_cleanup(void);
bool hid_ping(void);

void hid_event(int fd, struct input_event *ev);

#endif
