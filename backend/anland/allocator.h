#ifndef WLR_BACKEND_ANLAND_ALLOCATOR_PRIVATE_H
#define WLR_BACKEND_ANLAND_ALLOCATOR_PRIVATE_H

#include "render/allocator/allocator.h"
#include "backend.h"

struct wlr_allocator *anland_allocator_create(struct wlr_anland_backend *backend,
	struct wlr_renderer *renderer);

#endif
