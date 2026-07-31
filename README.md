# pogopoOS2.0 — Graphics STEP2

This archive is based on the exact Sharp CS diagnostic build that displayed
correctly on the LS027B7DH01.

## What changed

- `components/pogopo_graphics/` replaces the temporary `pogopo_sharp` component.
- Working manual active-HIGH SCS timing is preserved.
- 400x240 1-bit framebuffer remains in PSRAM.
- DMA transfer buffer remains in internal RAM.
- Dedicated FreeRTOS VCOM task runs every 500 ms.
- Dirty rows are packed into one Sharp write transaction, even when they are
  non-contiguous.
- Full refresh, row-range refresh, and dirty refresh are available.
- `Canvas` API adds pixel, line, rectangle, filled rectangle, circle, filled
  circle, 1-bit bitmap, and 5x7 text.
- Refresh statistics track time, row count, bytes, total traffic, and VCOM
  toggles.
- The demo animates a small square at about 30 FPS and updates a live benchmark.

## Expected screen

- `pogopoOS2.0 STEP2`
- primitive graphics examples
- a moving black square in the animation lane
- live FPS, refresh time, rows, and bytes

## Expected logs

A line similar to this appears once per second:

```text
I (...) gfx_demo: FPS=30 last=...us rows=18 bytes=... total_refresh=... vcom=...
```

The square should move without full-screen flashing. Most animation refreshes
should report around 18 dirty rows rather than 240.
