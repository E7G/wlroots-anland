#ifndef WLR_BACKEND_ANLAND_OUTPUT_PRIVATE_H
#define WLR_BACKEND_ANLAND_OUTPUT_PRIVATE_H
#include "backend.h"
struct wlr_anland_output *anland_output_create(
	struct wlr_anland_backend *backend);
void anland_output_handle_ready(struct wlr_anland_output *output);
#endif
