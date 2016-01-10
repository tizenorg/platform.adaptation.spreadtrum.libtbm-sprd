/**************************************************************************

libtbm

Copyright 2012 Samsung Electronics co., Ltd. All Rights Reserved.

Contact: SooChan Lim <sc1.lim@samsung.com>, Sangjin Lee <lsj119@samsung.com>
Boram Park <boram1288.park@samsung.com>, Changyeon Lee <cyeon.lee@samsung.com>

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

#include "config.h"

#include <xf86drm.h>

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <wayland-client.h>

#include "wayland-util.h"

extern const struct wl_interface wl_buffer_interface;

static const struct wl_interface *types[] = {
	NULL,
	NULL,
	NULL,
	&wl_buffer_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	&wl_buffer_interface,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

static const struct wl_message wl_tbm_requests[] = {
	{"create_buffer", "niiuiiiiiiiiiiuiuuu", types + 3},
	{"create_buffer_with_fd", "niiuiiiiiiiiiiuihhh", types + 22},
	{"get_authentication_info", "", types + 0},
};

static const struct wl_message wl_tbm_events[] = {
	{"authentication_info", "suh", types + 0},
};

WL_EXPORT const struct wl_interface wl_tbm_interface = {
	"wl_tbm", 1,
	3, wl_tbm_requests,
	1, wl_tbm_events,
};

struct wl_buffer;
struct wl_tbm;

extern const struct wl_interface wl_tbm_interface;

#ifndef WL_TBM_ERROR_ENUM
#define WL_TBM_ERROR_ENUM
enum wl_tbm_error {
	WL_TBM_ERROR_AUTHENTICATE_FAIL = 0,
	WL_TBM_ERROR_INVALID_FORMAT = 1,
	WL_TBM_ERROR_INVALID_NAME = 2,
};
#endif							/* WL_TBM_ERROR_ENUM */

struct wl_tbm_listener {
	/**
     * authentication_info - (none)
     * @device_name: (none)
     * @capabilities: (none)
     * @auth_fd: (none)
     */
	void (*authentication_info) (void *data, struct wl_tbm * wl_tbm, const char *device_name, uint32_t capabilities, int32_t auth_fd);
};

static inline int wl_tbm_add_listener(struct wl_tbm *wl_tbm, const struct wl_tbm_listener *listener, void *data)
{
	return wl_proxy_add_listener((struct wl_proxy *)wl_tbm, (void (**)(void))listener, data);
}

#define WL_TBM_CREATE_BUFFER    0
#define WL_TBM_CREATE_BUFFER_WITH_FD    1
#define WL_TBM_GET_AUTHENTICATION_INFO  2

static inline void wl_tbm_set_user_data(struct wl_tbm *wl_tbm, void *user_data)
{
	wl_proxy_set_user_data((struct wl_proxy *)wl_tbm, user_data);
}

static inline void *wl_tbm_get_user_data(struct wl_tbm *wl_tbm)
{
	return wl_proxy_get_user_data((struct wl_proxy *)wl_tbm);
}

static inline void wl_tbm_destroy(struct wl_tbm *wl_tbm)
{
	wl_proxy_destroy((struct wl_proxy *)wl_tbm);
}

static inline void wl_tbm_get_authentication_info(struct wl_tbm *wl_tbm)
{
	wl_proxy_marshal((struct wl_proxy *)wl_tbm, WL_TBM_GET_AUTHENTICATION_INFO);
}

struct wl_tbm_info {
	struct wl_display *dpy;
	struct wl_event_queue *wl_queue;
	struct wl_tbm *wl_tbm;

	uint32_t capabilities;
	char *device;
	int32_t fd;
};

static void handle_tbm_authentication_info(void *data, struct wl_tbm *wl_tbm, const char *device_name, uint32_t capabilities, int32_t auth_fd)
{
	struct wl_tbm_info *info = (struct wl_tbm_info *)data;

	info->fd = auth_fd;
	info->capabilities = capabilities;
	if (device_name)
		info->device = strndup(device_name, 256);
}

static const struct wl_tbm_listener wl_tbm_client_listener = {
	handle_tbm_authentication_info
};

static void wl_client_registry_handle_global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
	struct wl_tbm_info *info = (struct wl_tbm_info *)data;

	if (!strcmp(interface, "wl_tbm")) {
		info->wl_tbm = wl_registry_bind(registry, name, &wl_tbm_interface, version);
		if (!info->wl_tbm) {
			printf("Failed to bind wl_tbm\n");
			return;
		}

		wl_tbm_add_listener(info->wl_tbm, &wl_tbm_client_listener, info);
		wl_proxy_set_queue((struct wl_proxy *)info->wl_tbm, info->wl_queue);
	}
}

static int tbm_util_get_drm_fd(void *dpy, int *fd)
{
	struct wl_display *disp = NULL;
	struct wl_registry *wl_registry;
	struct wl_tbm_info info = {
		.dpy = NULL,
		.wl_queue = NULL,
		.wl_tbm = NULL,
		.capabilities = 0,
		.device = NULL,
		.fd = 0,
	};

	static const struct wl_registry_listener registry_listener = {
		wl_client_registry_handle_global,
		NULL
	};

	if (!fd)
		return -1;

	if (!dpy) {
		disp = wl_display_connect(NULL);
		if (!disp) {
			printf("Failed to create a new display connection\n");
			return -1;
		}
		dpy = disp;
	}

	info.dpy = dpy;
	info.wl_queue = wl_display_create_queue(dpy);
	if (!info.wl_queue) {
		printf("Failed to create a WL Queue\n");
		if (disp == dpy)
			wl_display_disconnect(disp);

		return -1;
	}

	wl_registry = wl_display_get_registry(dpy);
	if (!wl_registry) {
		printf("Failed to get registry\n");
		wl_event_queue_destroy(info.wl_queue);
		if (disp == dpy)
			wl_display_disconnect(disp);

		return -1;
	}
	wl_proxy_set_queue((struct wl_proxy *)wl_registry, info.wl_queue);
	wl_registry_add_listener(wl_registry, &registry_listener, &info);
	wl_display_roundtrip_queue(dpy, info.wl_queue);

	wl_tbm_get_authentication_info(info.wl_tbm);
	wl_display_roundtrip_queue(dpy, info.wl_queue);

	*fd = info.fd;

	wl_event_queue_destroy(info.wl_queue);
	wl_registry_destroy(wl_registry);

	free(info.device);
	wl_tbm_set_user_data(info.wl_tbm, NULL);
	wl_tbm_destroy(info.wl_tbm);

	if (disp == dpy)
		wl_display_disconnect(disp);

	return *fd >= 0 ? 0 : -1;
}

int tbm_bufmgr_get_drm_fd_wayland()
{
	int fd = -1;

	if (tbm_util_get_drm_fd(NULL, &fd))
		printf("Failed to get drm_fd\n");

	return fd;
}
