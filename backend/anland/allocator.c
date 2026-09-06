#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/anland.h>
#include <wlr/util/log.h>
#include "render/allocator/allocator.h"
#include "allocator.h"

struct anland_buffer {
	struct wlr_buffer base;
	struct wlr_anland_backend *backend;
	struct wlr_dmabuf_attributes dmabuf;
	int index;
};

struct anland_allocator {
	struct wlr_allocator base;
	struct wlr_anland_backend *backend;
	struct wlr_renderer *renderer;
	unsigned next;
};

struct anland_lookup {
	struct wlr_anland_backend *backend;
};

static void find_anland_backend(struct wlr_backend *candidate, void *data) {
	struct anland_lookup *lookup = data;
	if (lookup->backend == NULL && wlr_backend_is_anland(candidate)) {
		lookup->backend = anland_backend_from_backend(candidate);
	}
}

static const struct wlr_buffer_impl buffer_impl;

static struct anland_buffer *buffer_from_wlr(struct wlr_buffer *buffer) {
	assert(buffer->impl == &buffer_impl);
	return (struct anland_buffer *)buffer;
}

static void buffer_destroy(struct wlr_buffer *buffer) {
	struct anland_buffer *ab = buffer_from_wlr(buffer);
	wlr_dmabuf_attributes_finish(&ab->dmabuf);
	free(ab);
}

static bool buffer_get_dmabuf(struct wlr_buffer *buffer,
		struct wlr_dmabuf_attributes *attribs) {
	struct anland_buffer *ab = buffer_from_wlr(buffer);
	return wlr_dmabuf_attributes_copy(attribs, &ab->dmabuf);
}

static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
};

static struct wlr_buffer *allocator_create_buffer(
		struct wlr_allocator *allocator, int width, int height,
		const struct wlr_drm_format *format) {
	struct anland_allocator *alloc = (struct anland_allocator *)allocator;
	struct wlr_anland_backend *backend = alloc->backend;

	/* The consumer may be started after the compositor. Keep allocator creation
	 * deterministic, but give the Android app a short reconnect window. */
	for (int attempt = 0; attempt < 25 && is_fallback(backend->display_ctx); ++attempt) {
		if (try_exit_fallback(backend->display_ctx) == 0)
			break;
		usleep(20000);
	}
	if (is_fallback(backend->display_ctx)) {
		wlr_log(WLR_ERROR, "Anland consumer is not connected");
		return NULL;
	}

	int count = get_buf_count(backend->display_ctx);
	if (count <= 0) {
		return NULL;
	}
	int index = get_selected_idx(backend->display_ctx);
	if (index < 0 || index >= count) {
		index = (int)(alloc->next++ % (unsigned)count);
	}
	struct buf_info info;
	if (get_dmabuf_info_at(backend->display_ctx, index, &info) < 0) {
		return NULL;
	}
	int fd = get_dmabuf_fd_at(backend->display_ctx, index);
	if (fd < 0) {
		return NULL;
	}

	struct anland_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer) {
		return NULL;
	}
	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
	buffer->backend = backend;
	buffer->index = index;
	buffer->dmabuf.width = info.width;
	buffer->dmabuf.height = info.height;
	buffer->dmabuf.format = info.format ? info.format : format->format;
	buffer->dmabuf.modifier = info.modifier;
	buffer->dmabuf.n_planes = 1;
	buffer->dmabuf.offset[0] = info.offset;
	buffer->dmabuf.stride[0] = info.stride;
	buffer->dmabuf.fd[0] = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (buffer->dmabuf.fd[0] < 0) {
		wlr_buffer_drop(&buffer->base);
		return NULL;
	}
	return &buffer->base;
}

static void allocator_destroy(struct wlr_allocator *allocator) {
	free(allocator);
}

static const struct wlr_allocator_interface allocator_impl = {
	.destroy = allocator_destroy,
	.create_buffer = allocator_create_buffer,
};

struct wlr_allocator *anland_allocator_create(struct wlr_anland_backend *backend,
		struct wlr_renderer *renderer) {
	struct anland_allocator *alloc = calloc(1, sizeof(*alloc));
	if (!alloc) {
		return NULL;
	}
	alloc->backend = backend;
	alloc->renderer = renderer;
	wlr_allocator_init(&alloc->base, &allocator_impl, WLR_BUFFER_CAP_DMABUF);
	return &alloc->base;
}

struct wlr_allocator *wlr_anland_allocator_create(struct wlr_backend *backend,
		struct wlr_renderer *renderer) {
	struct wlr_anland_backend *anland = anland_backend_from_backend(backend);
	if (anland == NULL && wlr_backend_is_multi(backend)) {
		struct anland_lookup lookup = {0};
		wlr_multi_for_each_backend(backend, find_anland_backend, &lookup);
		anland = lookup.backend;
	}
	return anland ? anland_allocator_create(anland, renderer) : NULL;
}
