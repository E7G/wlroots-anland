#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <wlr/interfaces/wlr_output.h>
#include "render/swapchain.h"
#include <wlr/types/wlr_buffer.h>
#include <wlr/util/log.h>
#include "output.h"

static const uint32_t SUPPORTED_OUTPUT_STATE =
	WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
	WLR_OUTPUT_STATE_BUFFER |
	WLR_OUTPUT_STATE_MODE |
	WLR_OUTPUT_STATE_ENABLED;

static bool output_test(struct wlr_output *wlr_output) {
	uint32_t unsupported = wlr_output->pending.committed & ~SUPPORTED_OUTPUT_STATE;
	if (unsupported) {
		wlr_log(WLR_DEBUG, "Anland output: unsupported state 0x%"PRIx32,
			unsupported);
		return false;
	}
	return true;
}

static bool output_commit(struct wlr_output *wlr_output) {
	struct wlr_anland_output *output = anland_output_from_output(wlr_output);
	if (!output_test(wlr_output)) {
		return false;
	}

	if (wlr_output->pending.committed & WLR_OUTPUT_STATE_MODE) {
		if (wlr_output->pending.mode_type != WLR_OUTPUT_STATE_MODE_CUSTOM) {
			return false;
		}
		wlr_output_update_custom_mode(wlr_output,
			wlr_output->pending.custom_mode.width,
			wlr_output->pending.custom_mode.height,
			wlr_output->pending.custom_mode.refresh);
	}

	if (wlr_output->pending.committed & WLR_OUTPUT_STATE_BUFFER) {
		struct wlr_buffer *buffer = wlr_output->pending.buffer;
		/* Keep one backend reference until the consumer signals that Android has
		 * finished displaying this buffer. This prevents swapchain reuse/tearing. */
		wlr_buffer_lock(buffer);
		if (trigger_refresh(output->backend->display_ctx) < 0) {
			wlr_buffer_unlock(buffer);
			return false;
		}
		output->submitted = buffer;
		output->commit_seq = wlr_output->commit_seq + 1;
	}
	return true;
}

static void output_destroy(struct wlr_output *wlr_output) {
	struct wlr_anland_output *output = anland_output_from_output(wlr_output);
	if (output->submitted) {
		wlr_buffer_unlock(output->submitted);
		output->submitted = NULL;
	}
	wl_list_remove(&output->link);
	if (output->frame_timer) {
		wl_event_source_remove(output->frame_timer);
	}
	free(output);
}

static const struct wlr_output_impl output_impl = {
	.destroy = output_destroy,
	.test = output_test,
	.commit = output_commit,
};

static int signal_frame(void *data) {
	struct wlr_anland_output *output = data;
	wlr_output_send_frame(&output->output);
	return 0;
}

struct wlr_anland_output *anland_output_create(
		struct wlr_anland_backend *backend) {
	struct wlr_anland_output *output = calloc(1, sizeof(*output));
	if (!output) {
		return NULL;
	}
	output->backend = backend;
	wlr_output_init(&output->output, &backend->backend, &output_impl,
		backend->display);
	wlr_output_update_custom_mode(&output->output, backend->width,
		backend->height, backend->refresh ? (int32_t)backend->refresh : 60000);
	strncpy(output->output.make, "Anland", sizeof(output->output.make));
	strncpy(output->output.model, "Android", sizeof(output->output.model));
	strncpy(output->output.name, "ANLAND-1", sizeof(output->output.name));
	wlr_output_set_description(&output->output,
		"Anland Android display");

	struct wl_event_loop *loop = wl_display_get_event_loop(backend->display);
	int refresh = backend->refresh ? (int)backend->refresh : 60000;
	int delay_ms = refresh > 0 ? (1000000 / refresh) / 1000 : 16;
	if (delay_ms < 1) delay_ms = 1;
	output->frame_timer = wl_event_loop_add_timer(loop, signal_frame, output);
	if (!output->frame_timer) {
		wlr_output_destroy(&output->output);
		return NULL;
	}
	wl_list_insert(&backend->outputs, &output->link);
	/* The timer is armed after the backend emits new_output and the output is
	 * enabled, matching the headless backend's frame semantics. */
	wl_event_source_timer_update(output->frame_timer, delay_ms);
	return output;
}

void anland_output_handle_ready(struct wlr_anland_output *output) {
	if (!output) return;
	if (output->submitted) {
		wlr_buffer_unlock(output->submitted);
		output->submitted = NULL;
		struct wlr_output_event_present event = {
			.commit_seq = output->commit_seq,
			.presented = true,
			.flags = WLR_OUTPUT_PRESENT_VSYNC | WLR_OUTPUT_PRESENT_ZERO_COPY,
		};
		wlr_output_send_present(&output->output, &event);
		/* Anland's consumer selects the next pool slot before each frame. The
		 * stock swapchain reuses its first released slot, so rotate it here to
		 * force allocator_create_buffer() to import the newly selected dma-buf. */
		wlr_swapchain_destroy(output->output.swapchain);
		output->output.swapchain = NULL;
	}
	wlr_output_send_frame(&output->output);
}
