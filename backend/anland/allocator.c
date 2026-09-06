#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/backend/anland.h>
#include <wlr/backend/multi.h>
#include "backend.h"

struct anland_buffer { struct wlr_buffer base; struct wlr_dmabuf_attributes dmabuf; };
struct anland_allocator { struct wlr_allocator base; struct wlr_anland_backend *backend; unsigned next; };
struct anland_find { struct wlr_anland_backend *backend; };
static void find_backend(struct wlr_backend *candidate, void *data) {
	struct anland_find *find = data;
	if (!find->backend && wlr_backend_is_anland(candidate)) find->backend = anland_backend_from_backend(candidate);
}
static const struct wlr_buffer_impl buffer_impl;

static void buffer_destroy(struct wlr_buffer *b) {
	struct anland_buffer *ab = wl_container_of(b, ab, base);
	wlr_dmabuf_attributes_finish(&ab->dmabuf); free(ab);
}
static bool buffer_get_dmabuf(struct wlr_buffer *b, struct wlr_dmabuf_attributes *a) {
	struct anland_buffer *ab = wl_container_of(b, ab, base);
	return wlr_dmabuf_attributes_copy(a, &ab->dmabuf);
}
static const struct wlr_buffer_impl buffer_impl = { .destroy = buffer_destroy, .get_dmabuf = buffer_get_dmabuf };

static struct wlr_buffer *create_buffer(struct wlr_allocator *a, int width, int height,
		const struct wlr_drm_format *format) {
	struct anland_allocator *alloc = wl_container_of(a, alloc, base);
	struct wlr_anland_backend *backend = alloc->backend;
	for (int i = 0; i < 25 && is_fallback(backend->ctx); ++i) {
		if (try_exit_fallback(backend->ctx) == 0) break;
		usleep(20000);
	}
	if (is_fallback(backend->ctx)) return NULL;
	int count = get_buf_count(backend->ctx);
	if (count <= 0) return NULL;
	int index = get_selected_idx(backend->ctx);
	if (index < 0 || index >= count) index = (int)(alloc->next++ % (unsigned)count);
	struct buf_info info;
	if (get_dmabuf_info_at(backend->ctx, index, &info) < 0) return NULL;
	int fd = get_dmabuf_fd_at(backend->ctx, index);
	struct anland_buffer *buffer = calloc(1, sizeof(*buffer));
	if (!buffer || fd < 0) { free(buffer); return NULL; }
	wlr_buffer_init(&buffer->base, &buffer_impl, width, height);
	buffer->dmabuf.width = info.width; buffer->dmabuf.height = info.height;
	buffer->dmabuf.format = info.format ? info.format : format->format;
	buffer->dmabuf.modifier = info.modifier; buffer->dmabuf.n_planes = 1;
	buffer->dmabuf.offset[0] = info.offset; buffer->dmabuf.stride[0] = info.stride;
	buffer->dmabuf.fd[0] = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	if (buffer->dmabuf.fd[0] < 0) { wlr_buffer_drop(&buffer->base); return NULL; }
	return &buffer->base;
}
static void allocator_destroy(struct wlr_allocator *a) { free(a); }
static const struct wlr_allocator_interface allocator_impl = {
	.create_buffer = create_buffer, .destroy = allocator_destroy,
};
struct wlr_allocator *wlr_anland_allocator_create(struct wlr_backend *backend,
		struct wlr_renderer *renderer) {
	struct wlr_anland_backend *anland = anland_backend_from_backend(backend);
	if (!anland && wlr_backend_is_multi(backend)) {
		struct anland_find find = {0};
		wlr_multi_for_each_backend(backend, find_backend, &find); anland = find.backend;
	}
	if (!anland) return NULL;
	struct anland_allocator *alloc = calloc(1, sizeof(*alloc));
	if (!alloc) return NULL;
	alloc->backend = anland;
	wlr_allocator_init(&alloc->base, &allocator_impl, WLR_BUFFER_CAP_DMABUF);
	return &alloc->base;
}
