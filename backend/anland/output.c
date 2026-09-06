#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/types/wlr_output_layer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>
#include <wlr/render/swapchain.h>
#include "types/wlr_output.h"
#include "backend.h"

static const uint32_t supported = WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
	WLR_OUTPUT_STATE_BUFFER | WLR_OUTPUT_STATE_MODE | WLR_OUTPUT_STATE_ENABLED;
static bool output_test(struct wlr_output *o, const struct wlr_output_state *s) {
	if (s->committed & ~supported) return false;
	if (s->committed & WLR_OUTPUT_STATE_LAYERS)
		for (size_t i = 0; i < s->layers_len; ++i) s->layers[i].accepted = true;
	return true;
}
static bool output_commit(struct wlr_output *o, const struct wlr_output_state *s) {
	struct wlr_anland_output *out = anland_output_from_output(o);
	if (!output_test(o, s)) return false;
	if (s->committed & WLR_OUTPUT_STATE_MODE) {
		if (s->mode_type != WLR_OUTPUT_STATE_MODE_CUSTOM) return false;
		int refresh = s->custom_mode.refresh > 0 ? s->custom_mode.refresh : 60000;
		out->backend->refresh = (uint32_t)refresh;
	}
	if (s->committed & WLR_OUTPUT_STATE_BUFFER) {
		wlr_buffer_lock(s->buffer);
		if (trigger_refresh(out->backend->ctx) < 0) { wlr_buffer_unlock(s->buffer); return false; }
		out->submitted = s->buffer; out->commit_seq = o->commit_seq + 1;
	}
	return true;
}
static void output_destroy(struct wlr_output *o) {
	struct wlr_anland_output *out = anland_output_from_output(o);
	if (out->submitted) wlr_buffer_unlock(out->submitted);
	wlr_output_finish(o); wl_list_remove(&out->link);
	if (out->frame_timer) {
		wl_event_source_remove(out->frame_timer);
	}
	free(out);
}
static bool output_cursor(struct wlr_output *o, struct wlr_buffer *b, int x, int y) { return true; }
static bool output_move_cursor(struct wlr_output *o, int x, int y) { return true; }
static const struct wlr_output_impl output_impl = {
	.test = output_test, .commit = output_commit, .destroy = output_destroy,
	.set_cursor = output_cursor, .move_cursor = output_move_cursor,
};
static int signal_frame(void *data) { wlr_output_send_frame(data); return 0; }

struct wlr_anland_output *anland_output_create(struct wlr_anland_backend *backend) {
	struct wlr_anland_output *out = calloc(1, sizeof(*out)); if (!out) return NULL;
	struct wlr_output_state state; wlr_output_state_init(&state);
	wlr_output_state_set_custom_mode(&state, backend->width, backend->height,
		backend->refresh ? (int32_t)backend->refresh : 60000);
	wlr_output_init(&out->output, &backend->backend, &output_impl, backend->loop, &state);
	wlr_output_state_finish(&state); out->backend = backend;
	wlr_output_set_name(&out->output, "ANLAND-1");
	wlr_output_set_description(&out->output, "Anland Android display");
	struct wl_event_loop *loop = backend->loop;
	out->frame_timer = wl_event_loop_add_timer(loop, signal_frame, &out->output);
	if (!out->frame_timer) { wlr_output_destroy(&out->output); return NULL; }
	wl_list_insert(&backend->outputs, &out->link); return out;
}
void anland_output_handle_ready(struct wlr_anland_output *out) {
	if (!out) return;
	if (out->submitted) {
		wlr_buffer_unlock(out->submitted); out->submitted = NULL;
		struct wlr_output_event_present event = { .commit_seq = out->commit_seq,
			.presented = true, .flags = WLR_OUTPUT_PRESENT_VSYNC | WLR_OUTPUT_PRESENT_ZERO_COPY };
		wlr_output_send_present(&out->output, &event);
		wlr_swapchain_destroy(out->output.swapchain); out->output.swapchain = NULL;
	}
	wlr_output_send_frame(&out->output);
}
