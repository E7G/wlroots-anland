#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_input_device.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_output.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/backend/anland.h>
#include <wlr/util/log.h>
#include "util/signal.h"
#include "render/wlr_renderer.h"
#include "backend.h"
#include "allocator.h"
#include "output.h"

static const struct wlr_backend_impl backend_impl;

struct wlr_anland_backend *anland_backend_from_backend(
		struct wlr_backend *backend) {
	if (!wlr_backend_is_anland(backend)) {
		return NULL;
	}
	return (struct wlr_anland_backend *)backend;
}

struct wlr_anland_output *anland_output_from_output(
		struct wlr_output *output) {
	return (struct wlr_anland_output *)output;
}

static void destroy_output(struct wlr_anland_output *output) {
	if (output) {
		wlr_output_destroy(&output->output);
	}
}

static void handle_fallback(void *data) {
	struct wlr_anland_backend *backend = data;
	if (backend->input_source) {
		wl_event_source_remove(backend->input_source);
		backend->input_source = NULL;
	}
	if (backend->data_source) {
		wl_event_source_remove(backend->data_source);
		backend->data_source = NULL;
	}
	if (backend->pointer_device) {
		wlr_input_device_destroy(backend->pointer_device);
		backend->pointer_device = NULL;
	}
	if (backend->keyboard_device) {
		wlr_input_device_destroy(backend->keyboard_device);
		backend->keyboard_device = NULL;
	}
	struct wlr_anland_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &backend->outputs, link) {
		destroy_output(output);
	}
	backend->output_created = false;
	wlr_log(WLR_INFO, "Anland consumer disconnected; waiting for reconnect");
}

static int handle_buffer_ready(int fd, uint32_t mask, void *data) {
	struct wlr_anland_backend *backend = data;
	uint64_t count;
	if (read(fd, &count, sizeof(count)) < 0 && errno != EAGAIN) {
		return 0;
	}
	struct wlr_anland_output *output;
	wl_list_for_each(output, &backend->outputs, link) {
		anland_output_handle_ready(output);
	}
	return 0;
}

static int handle_input(int fd, uint32_t mask, void *data) {
	struct wlr_anland_backend *backend = data;
	struct InputEvent event;
	while (poll_input_event(backend->display_ctx, &event, 0) > 0) {
		uint32_t now = 0;
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
		if (event.type == INPUT_TYPE_KEY && backend->keyboard_device) {
			struct wlr_event_keyboard_key key = {
				.time_msec = now,
				.keycode = (uint32_t)event.key.keycode,
				.update_state = true,
				.state = event.key.action == INPUT_ACTION_UP ?
					WL_KEYBOARD_KEY_STATE_RELEASED : WL_KEYBOARD_KEY_STATE_PRESSED,
			};
			wlr_keyboard_notify_key(backend->keyboard_device->keyboard, &key);
		} else if (event.type == INPUT_TYPE_TOUCH && backend->pointer_device) {
			struct wlr_event_pointer_motion_absolute motion = {
				.device = backend->pointer_device,
				.time_msec = now,
				.x = event.touch.x / (double)backend->width,
				.y = event.touch.y / (double)backend->height,
			};
			wlr_signal_emit_safe(&backend->pointer_device->pointer->events.motion_absolute,
				&motion);
			if (event.touch.action != INPUT_ACTION_MOVE) {
				struct wlr_event_pointer_button button = {
					.device = backend->pointer_device,
					.time_msec = now,
					.button = BTN_LEFT,
					.state = event.touch.action == INPUT_ACTION_DOWN ?
						WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED,
				};
				wlr_signal_emit_safe(&backend->pointer_device->pointer->events.button,
					&button);
			}
		}
	}
	return 0;
}

static void create_input_devices(struct wlr_anland_backend *backend) {
	static const struct wlr_input_device_impl input_impl = {0};
	backend->pointer_device = calloc(1, sizeof(*backend->pointer_device));
	backend->keyboard_device = calloc(1, sizeof(*backend->keyboard_device));
	if (!backend->pointer_device || !backend->keyboard_device) return;
	wlr_input_device_init(backend->pointer_device, WLR_INPUT_DEVICE_POINTER,
		&input_impl, "Anland Touch", 0, 0);
	backend->pointer_device->pointer = calloc(1, sizeof(struct wlr_pointer));
	wlr_pointer_init(backend->pointer_device->pointer, NULL);
	wlr_input_device_init(backend->keyboard_device, WLR_INPUT_DEVICE_KEYBOARD,
		&input_impl, "Anland Keyboard", 0, 0);
	backend->keyboard_device->keyboard = calloc(1, sizeof(struct wlr_keyboard));
	wlr_keyboard_init(backend->keyboard_device->keyboard, NULL);
	wlr_signal_emit_safe(&backend->backend.events.new_input, backend->pointer_device);
	wlr_signal_emit_safe(&backend->backend.events.new_input, backend->keyboard_device);
}

static int reconnect_tick(void *data) {
	struct wlr_anland_backend *backend = data;
	if (is_fallback(backend->display_ctx) &&
		try_exit_fallback(backend->display_ctx) == 0) {
		anland_backend_emit_output(backend);
		int fd = get_buffer_ready_fd(backend->display_ctx);
		if (fd >= 0 && backend->input_source == NULL) {
			struct wl_event_loop *loop = wl_display_get_event_loop(backend->display);
			backend->input_source = wl_event_loop_add_fd(loop, fd,
				WL_EVENT_READABLE, handle_buffer_ready, backend);
		}
	}
	wl_event_source_timer_update(backend->reconnect_timer, 200);
	return 0;
}

static bool backend_start(struct wlr_backend *wlr_backend) {
	struct wlr_anland_backend *backend = anland_backend_from_backend(wlr_backend);
	backend->started = true;
	if (try_exit_fallback(backend->display_ctx) == 0) {
		anland_backend_emit_output(backend);
	}
	if (backend->reconnect_timer) {
		wl_event_source_timer_update(backend->reconnect_timer, 200);
	}
	return true;
}

static void backend_destroy(struct wlr_backend *wlr_backend) {
	struct wlr_anland_backend *backend = anland_backend_from_backend(wlr_backend);
	if (!backend) return;
	if (backend->reconnect_timer) wl_event_source_remove(backend->reconnect_timer);
	if (backend->input_source) wl_event_source_remove(backend->input_source);
	if (backend->data_source) wl_event_source_remove(backend->data_source);
	if (backend->pointer_device) wlr_input_device_destroy(backend->pointer_device);
	if (backend->keyboard_device) wlr_input_device_destroy(backend->keyboard_device);
	struct wlr_anland_output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &backend->outputs, link) {
		destroy_output(output);
	}
	if (backend->display_ctx) disconnect(backend->display_ctx);
	wlr_backend_finish(wlr_backend);
	wl_list_remove(&backend->display_destroy.link);
	if (backend->drm_fd >= 0) close(backend->drm_fd);
	free(backend);
}

static struct wlr_renderer *backend_get_renderer(struct wlr_backend *wlr_backend) {
	struct wlr_anland_backend *backend = anland_backend_from_backend(wlr_backend);
	if (!backend->backend.renderer) {
		backend->backend.renderer = renderer_autocreate_with_drm_fd(backend->drm_fd);
		if (!backend->backend.renderer) return NULL;
		backend->backend.has_own_renderer = true;
	}
	if (!backend->backend.allocator) {
		backend->backend.allocator = anland_allocator_create(backend,
			backend->backend.renderer);
	}
	return backend->backend.renderer;
}

static int backend_get_drm_fd(struct wlr_backend *wlr_backend) {
	return anland_backend_from_backend(wlr_backend)->drm_fd;
}

static uint32_t backend_get_buffer_caps(struct wlr_backend *wlr_backend) {
	return WLR_BUFFER_CAP_DMABUF;
}

static const struct wlr_backend_impl backend_impl = {
	.start = backend_start,
	.destroy = backend_destroy,
	.get_renderer = backend_get_renderer,
	.get_drm_fd = backend_get_drm_fd,
	.get_buffer_caps = backend_get_buffer_caps,
};

static void handle_display_destroy(struct wl_listener *listener, void *data) {
	struct wlr_anland_backend *backend =
		wl_container_of(listener, backend, display_destroy);
	backend_destroy(&backend->backend);
}

void anland_backend_emit_output(struct wlr_anland_backend *backend) {
	if (!backend->started || backend->output_created ||
		is_fallback(backend->display_ctx)) return;
	struct wlr_anland_output *output = anland_output_create(backend);
	if (!output) return;
	backend->output_created = true;
	wlr_output_update_enabled(&output->output, true);
	wlr_signal_emit_safe(&backend->backend.events.new_output, &output->output);
		int fd = get_buffer_ready_fd(backend->display_ctx);
	if (fd >= 0 && backend->input_source == NULL) {
		struct wl_event_loop *loop = wl_display_get_event_loop(backend->display);
		backend->input_source = wl_event_loop_add_fd(loop, fd,
			WL_EVENT_READABLE, handle_buffer_ready, backend);
		}
		if (backend->data_source == NULL && get_data_fd(backend->display_ctx) >= 0) {
			struct wl_event_loop *loop = wl_display_get_event_loop(backend->display);
			backend->data_source = wl_event_loop_add_fd(loop,
				get_data_fd(backend->display_ctx), WL_EVENT_READABLE,
				handle_input, backend);
			create_input_devices(backend);
		}
}

struct wlr_backend *wlr_anland_backend_create(struct wl_display *display,
		const char *socket_path) {
	struct wlr_anland_backend *backend = calloc(1, sizeof(*backend));
	if (!backend) return NULL;
	wlr_backend_init(&backend->backend, &backend_impl);
	backend->display = display;
	backend->drm_fd = -1;
	wl_list_init(&backend->outputs);
	const char *path = socket_path && socket_path[0] ? socket_path :
		getenv("ANLAND_SOCKET");
	if (!path || !path[0]) path = "/tmp/display_daemon.sock";
	if (connect_to_deamon(&backend->display_ctx, path) < 0) {
		wlr_log(WLR_ERROR, "Anland: cannot connect to %s", path);
		free(backend);
		return NULL;
	}
	get_screen_info(backend->display_ctx, &backend->width, &backend->height,
		&backend->format, &backend->refresh);
	const char *drm = getenv("ANLAND_DRM_DEVICE");
	if (!drm || !drm[0]) drm = "/dev/dri/renderD128";
	backend->drm_fd = open(drm, O_RDWR | O_CLOEXEC);
	if (backend->drm_fd < 0) {
		wlr_log(WLR_INFO, "Anland: no DRM render node (%s), using Pixman", drm);
	}
	set_fallback_callback(backend->display_ctx, handle_fallback, backend);
	struct wl_event_loop *loop = wl_display_get_event_loop(display);
	backend->reconnect_timer = wl_event_loop_add_timer(loop, reconnect_tick, backend);
	backend->display_destroy.notify = handle_display_destroy;
	wl_display_add_destroy_listener(display, &backend->display_destroy);
	return &backend->backend;
}

bool wlr_backend_is_anland(struct wlr_backend *backend) {
	return backend && backend->impl == &backend_impl;
}
