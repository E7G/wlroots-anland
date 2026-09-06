#ifndef WLR_BACKEND_ANLAND_PRIVATE_H
#define WLR_BACKEND_ANLAND_PRIVATE_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include "../display_producer.h"

struct wlr_anland_backend {
	struct wlr_backend backend;
	struct wl_display *display;
	struct wl_list outputs;
	struct display_ctx *display_ctx;
	struct wl_event_source *reconnect_timer;
	struct wl_event_source *input_source;
	struct wl_event_source *data_source;
	struct wl_listener display_destroy;
	struct wlr_input_device *pointer_device;
	struct wlr_input_device *keyboard_device;
	int drm_fd;
	uint32_t width, height, format, refresh;
	bool started;
	bool output_created;
};

struct wlr_anland_output {
	struct wlr_output output;
	struct wlr_anland_backend *backend;
	struct wl_list link;
	struct wl_event_source *frame_timer;
	struct wlr_buffer *submitted;
	uint32_t commit_seq;
};

struct wlr_anland_backend *anland_backend_from_backend(
	struct wlr_backend *backend);
struct wlr_anland_output *anland_output_from_output(
	struct wlr_output *output);
void anland_backend_emit_output(struct wlr_anland_backend *backend);
void anland_backend_release_frame(struct wlr_anland_backend *backend);

#endif
