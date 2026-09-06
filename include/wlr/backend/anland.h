#ifndef WLR_BACKEND_ANLAND_H
#define WLR_BACKEND_ANLAND_H
#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
struct wlr_renderer;
struct wlr_allocator;
struct wlr_backend *wlr_anland_backend_create(struct wl_event_loop *loop,
	const char *socket_path);
struct wlr_allocator *wlr_anland_allocator_create(struct wlr_backend *backend,
	struct wlr_renderer *renderer);
bool wlr_backend_is_anland(struct wlr_backend *backend);
#endif
