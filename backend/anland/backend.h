#ifndef WLR_BACKEND_ANLAND_PRIVATE_H
#define WLR_BACKEND_ANLAND_PRIVATE_H
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include "../display_producer.h"
struct wlr_anland_backend {
	struct wlr_backend backend;
	struct wl_event_loop *loop;
	struct wl_list outputs;
	struct display_ctx *ctx;
	struct wl_event_source *reconnect_timer;
	struct wl_event_source *ready_source;
	struct wl_event_source *data_source;
	struct wl_event_source *input_source;
	struct wl_listener loop_destroy;
	struct wlr_pointer *pointer;
	struct wlr_keyboard *keyboard;
	uint32_t width, height, format, refresh;
	int drm_fd;
	bool started, output_created;
};
struct wlr_anland_output {
	struct wlr_output output;
	struct wlr_anland_backend *backend;
	struct wl_list link;
	struct wl_event_source *frame_timer;
	struct wlr_buffer *submitted;
	uint32_t commit_seq;
};
struct wlr_anland_backend *anland_backend_from_backend(struct wlr_backend *backend);
struct wlr_anland_output *anland_output_from_output(struct wlr_output *output);
void anland_backend_emit_output(struct wlr_anland_backend *backend);
void anland_output_handle_ready(struct wlr_anland_output *output);
struct wlr_anland_output *anland_output_create(struct wlr_anland_backend *backend);
#endif
