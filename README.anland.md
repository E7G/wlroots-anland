# wlroots Anland backend

This branch adds a native `backend-anland` to wlroots and is based on the
protocol in [E7G/anland](https://github.com/E7G/anland). It is intended for
`labwc`, sway-derived compositors, and small wlroots sessions running inside
Android chroot/Droidspaces.

## Design

* Connects to the Anland display daemon (`ANLAND_SOCKET`, default
  `/tmp/display_daemon.sock`).
* Imports the consumer-owned dma-buf pool directly into the wlroots GLES2
  renderer (zero-copy; no screenshot/readback path).
* Holds a backend reference on the submitted buffer until the consumer's
  buffer-ready event, preventing swapchain reuse and the black-frame race.
* Reconnects every 200 ms when the Android consumer or daemon is restarted.
* Exposes Anland touch and keyboard events as wlroots input devices.
* Keeps the renderer/allocator behind the regular wlroots backend interfaces,
  so applications do not need Anland-specific rendering code.

## Build

Build wlroots with the Anland backend enabled, then build labwc against the
resulting wlroots:

```sh
meson setup build -Dbackends=anland -Dexamples=false
meson compile -C build
meson install -C build
```

Start labwc with:

```sh
export ANLAND_SOCKET=/tmp/display_daemon.sock
export WLR_BACKENDS=anland
export WLR_RENDERER=gles2
labwc
```

`ANLAND_DRM_DEVICE` can override the render node (for example
`/dev/dri/renderD128`). If no render node is available, wlroots falls back to
Pixman; dma-buf import then fails and the backend remains waiting for a usable
renderer rather than presenting a black surface.

## labwc integration

Recent labwc versions use `wlr_backend_autocreate`, so `WLR_BACKENDS=anland`
is sufficient. For labwc versions which explicitly create the allocator,
apply [`patches/labwc-anland.patch`](patches/labwc-anland.patch); it selects
`wlr_anland_allocator_create()` when `ANLAND_SOCKET` is set.

The maintained labwc integration is also published at
[`E7G/labwc`](https://github.com/E7G/labwc/tree/anland-backend), branch
`anland-backend`.

## Protocol source

`backend/display_producer.[ch]`, `backend/socket_utils.[ch]`, and
`backend/protocol.h` are copied from the E7G Anland producer library.
They retain the reconnect/fallback state machine and fd-passing semantics.
