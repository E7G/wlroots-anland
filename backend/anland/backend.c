#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wlr/backend/interface.h>
#include <wlr/backend/anland.h>
#include "interfaces/wlr_input_device.h"
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/util/log.h>
#include "backend.h"

static const struct wlr_backend_impl backend_impl;
struct wlr_anland_backend *anland_backend_from_backend(struct wlr_backend *b) {
	if (!wlr_backend_is_anland(b)) return NULL;
	struct wlr_anland_backend *backend;
	return wl_container_of(b, backend, backend);
}
struct wlr_anland_output *anland_output_from_output(struct wlr_output *o) {
	return wl_container_of(o, (struct wlr_anland_output *)0, output);
}
static void destroy_output(struct wlr_anland_output *o) { if (o) wlr_output_destroy(&o->output); }
static void fallback_cb(void *data) {
	struct wlr_anland_backend *b = data;
	if (b->ready_source) { wl_event_source_remove(b->ready_source); b->ready_source = NULL; }
	if (b->data_source) { wl_event_source_remove(b->data_source); b->data_source = NULL; }
	if (b->input_source) { wl_event_source_remove(b->input_source); b->input_source = NULL; }
	if (b->pointer) { wlr_pointer_finish(b->pointer); free(b->pointer); b->pointer = NULL; }
	if (b->keyboard) { wlr_keyboard_finish(b->keyboard); free(b->keyboard); b->keyboard = NULL; }
	struct wlr_anland_output *o, *tmp; wl_list_for_each_safe(o, tmp, &b->outputs, link) destroy_output(o);
	b->output_created = false;
}
static int ready_cb(int fd, uint32_t mask, void *data) {
	struct wlr_anland_backend *b = data; uint64_t n;
	if (read(fd, &n, sizeof(n)) < 0 && errno != EAGAIN) return 0;
	struct wlr_anland_output *o; wl_list_for_each(o, &b->outputs, link) anland_output_handle_ready(o); return 0;
}
static int input_cb(int fd, uint32_t mask, void *data) {
	struct wlr_anland_backend *b = data;
	struct InputEvent event;
	while (poll_input_event(b->ctx, &event, 0) > 0) {
		struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
		uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
		if (event.type == INPUT_TYPE_KEY && b->keyboard) {
			struct wlr_keyboard_key_event key = {
				.time_msec = now, .keycode = (uint32_t)event.key.keycode,
				.update_state = true,
				.state = event.key.action == INPUT_ACTION_UP ?
					WL_KEYBOARD_KEY_STATE_RELEASED : WL_KEYBOARD_KEY_STATE_PRESSED,
			};
			wlr_keyboard_notify_key(b->keyboard, &key);
		} else if (event.type == INPUT_TYPE_TOUCH && b->pointer) {
			struct wlr_pointer_motion_absolute_event motion = {
				.pointer = b->pointer, .time_msec = now,
				.x = event.touch.x / (double)b->width,
				.y = event.touch.y / (double)b->height,
			};
			wl_signal_emit_mutable(&b->pointer->events.motion_absolute, &motion);
			if (event.touch.action != INPUT_ACTION_MOVE) {
				struct wlr_pointer_button_event button = {
					.pointer = b->pointer, .time_msec = now, .button = BTN_LEFT,
					.state = event.touch.action == INPUT_ACTION_DOWN ?
						WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
				};
				wlr_pointer_notify_button(b->pointer, &button);
			}
		} else if (event.type == INPUT_TYPE_POINTER_MOTION && b->pointer) {
			struct wlr_pointer_motion_event motion = {
				.pointer = b->pointer, .time_msec = now,
				.delta_x = event.pointer_motion.dx, .delta_y = event.pointer_motion.dy,
				.unaccel_dx = event.pointer_motion.dx, .unaccel_dy = event.pointer_motion.dy,
			};
			wl_signal_emit_mutable(&b->pointer->events.motion, &motion);
		} else if (event.type == INPUT_TYPE_POINTER_BUTTON && b->pointer) {
			struct wlr_pointer_button_event button = {
				.pointer = b->pointer, .time_msec = now, .button = event.pointer_button.button,
				.state = event.pointer_button.pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED,
			};
			wlr_pointer_notify_button(b->pointer, &button);
		}
	}
	return 0;
}
static void create_input_devices(struct wlr_anland_backend *b) {
	if (b->pointer || b->keyboard) return;
	b->pointer = calloc(1, sizeof(*b->pointer));
	b->keyboard = calloc(1, sizeof(*b->keyboard));
	if (!b->pointer || !b->keyboard) return;
	wlr_pointer_init(b->pointer, NULL, "Anland Touch");
	wlr_keyboard_init(b->keyboard, NULL, "Anland Keyboard");
	wl_signal_emit_mutable(&b->backend.events.new_input, &b->pointer->base);
	wl_signal_emit_mutable(&b->backend.events.new_input, &b->keyboard->base);
}
static int reconnect_cb(void *data) {
	struct wlr_anland_backend *b = data;
	if (is_fallback(b->ctx) && try_exit_fallback(b->ctx) == 0) {
		anland_backend_emit_output(b);
	}
	if (b->reconnect_timer) {
		wl_event_source_timer_update(b->reconnect_timer, 200);
	}
	return 0;
}
static bool backend_start(struct wlr_backend *wlr) {
	struct wlr_anland_backend *b = anland_backend_from_backend(wlr); b->started = true;
	if (try_exit_fallback(b->ctx) == 0) {
		anland_backend_emit_output(b);
	}
	if (b->reconnect_timer) {
		wl_event_source_timer_update(b->reconnect_timer, 200);
	}
	return true;
}
static void backend_destroy(struct wlr_backend *wlr) {
	struct wlr_anland_backend *b = anland_backend_from_backend(wlr); if (!b) return;
	if (b->reconnect_timer) wl_event_source_remove(b->reconnect_timer);
	if (b->ready_source) wl_event_source_remove(b->ready_source);
	if (b->data_source) wl_event_source_remove(b->data_source);
	if (b->input_source) wl_event_source_remove(b->input_source);
	if (b->pointer) { wlr_pointer_finish(b->pointer); free(b->pointer); }
	if (b->keyboard) { wlr_keyboard_finish(b->keyboard); free(b->keyboard); }
	struct wlr_anland_output *o, *tmp; wl_list_for_each_safe(o, tmp, &b->outputs, link) destroy_output(o);
	if (b->ctx) {
		disconnect(b->ctx);
	}
	wlr_backend_finish(wlr);
	wl_list_remove(&b->loop_destroy.link);
	if (b->drm_fd >= 0) {
		close(b->drm_fd);
	}
	free(b);
}
static int backend_get_drm_fd(struct wlr_backend *wlr) {
	return anland_backend_from_backend(wlr)->drm_fd;
}
static const struct wlr_backend_impl backend_impl = { .start = backend_start, .destroy = backend_destroy,
	.get_drm_fd = backend_get_drm_fd };
static void loop_destroy(struct wl_listener *l, void *data) { struct wlr_anland_backend *b = wl_container_of(l,b,loop_destroy); backend_destroy(&b->backend); }
void anland_backend_emit_output(struct wlr_anland_backend *b) {
	if (!b->started || b->output_created || is_fallback(b->ctx)) return;
	struct wlr_anland_output *o = anland_output_create(b); if (!o) return; b->output_created = true;
	wl_signal_emit_mutable(&b->backend.events.new_output, &o->output);
	int fd = get_buffer_ready_fd(b->ctx); if (fd >= 0 && !b->ready_source) b->ready_source = wl_event_loop_add_fd(b->loop, fd, WL_EVENT_READABLE, ready_cb, b);
	fd = get_data_fd(b->ctx);
	if (fd >= 0 && !b->input_source) {
		create_input_devices(b);
		b->input_source = wl_event_loop_add_fd(b->loop, fd, WL_EVENT_READABLE, input_cb, b);
	}
}
struct wlr_backend *wlr_anland_backend_create(struct wl_event_loop *loop, const char *socket_path) {
	struct wlr_anland_backend *b = calloc(1, sizeof(*b)); if (!b) return NULL; wlr_backend_init(&b->backend, &backend_impl);
	b->loop = loop; b->drm_fd = -1; b->backend.buffer_caps = WLR_BUFFER_CAP_DMABUF; wl_list_init(&b->outputs);
	const char *path = socket_path && socket_path[0] ? socket_path : getenv("ANLAND_SOCKET"); if (!path) path = "/tmp/display_daemon.sock";
	if (connect_to_deamon(&b->ctx, path) < 0) { free(b); return NULL; }
	get_screen_info(b->ctx, &b->width, &b->height, &b->format, &b->refresh);
	const char *drm = getenv("ANLAND_DRM_DEVICE"); if (!drm) drm = "/dev/dri/renderD128"; b->drm_fd = open(drm, O_RDWR | O_CLOEXEC);
	b->reconnect_timer = wl_event_loop_add_timer(loop, reconnect_cb, b); set_fallback_callback(b->ctx, fallback_cb, b);
	b->loop_destroy.notify = loop_destroy; wl_event_loop_add_destroy_listener(loop, &b->loop_destroy); return &b->backend;
}
bool wlr_backend_is_anland(struct wlr_backend *b) { return b && b->impl == &backend_impl; }
