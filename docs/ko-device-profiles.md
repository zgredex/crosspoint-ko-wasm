# X4 and X3 profile audit

This note records exactly what was copied from the Korean fork, what was inferred,
and what the converter verifies. The reference is
`crosspoint-reader-ko/crosspoint-reader-ko`, branch `release/korean`, commit
`84a39194dfce1ebd772ac9163df0a59daa0d72dc`.

## Confirmed facts

1. The Korean fork selects X3 before display initialization by calling
   `setDisplayX3()` in
   [`lib/hal/HalDisplay.cpp`](https://github.com/crosspoint-reader-ko/crosspoint-reader-ko/blob/84a39194dfce1ebd772ac9163df0a59daa0d72dc/lib/hal/HalDisplay.cpp).
2. Its renderer obtains width, height, byte stride, and buffer size from runtime
   display getters in
   [`lib/GfxRenderer/GfxRenderer.cpp`](https://github.com/crosspoint-reader-ko/crosspoint-reader-ko/blob/84a39194dfce1ebd772ac9163df0a59daa0d72dc/lib/GfxRenderer/GfxRenderer.cpp).
3. The fork explicitly describes X3 portrait as 528 pixels wide versus X4's 480
   in
   [`BaseTheme.cpp`](https://github.com/crosspoint-reader-ko/crosspoint-reader-ko/blob/84a39194dfce1ebd772ac9163df0a59daa0d72dc/src/components/themes/BaseTheme.cpp).
4. FreeInk's device table gives X4 `800×480`/SSD1677 and X3 `792×528`/UC8253,
   both with four-level grayscale, and documents the runtime X3 selector:
   [`freeink-sdk`](https://github.com/Free-Ink/freeink-sdk#supported-devices).
5. The Korean XTC parser computes XTG and XTH payload sizes from each page header's
   width and height in
   [`XtcParser.cpp`](https://github.com/crosspoint-reader-ko/crosspoint-reader-ko/blob/84a39194dfce1ebd772ac9163df0a59daa0d72dc/lib/Xtc/Xtc/XtcParser.cpp).
   `XtcReaderActivity` likewise derives `planeSize`, `colBytes`, and row bytes from
   runtime page dimensions.

These facts produce the converter constants without approximation:

| Profile | Physical | Portrait page | Stride | One plane | XTG payload | XTH payload |
|---|---:|---:|---:|---:|---:|---:|
| X4 | 800×480 | 480×800 | 100 B | 48,000 B | 48,000 B | 96,000 B |
| X3 | 792×528 | 528×792 | 99 B | 52,272 B | 52,272 B | 104,544 B |

## What “based on the Korean fork” means here

- Device selection occurs before the renderer initializes, matching the fork's order.
- Pagination receives the actual profile-sized viewport. X3 is not an X4 render scaled
  or cropped after layout.
- Both profiles use the same pinned Korean text engine, KoPub Batang face, settings,
  safe margins, orientation transforms, and 4-level page semantics.
- XTC record and index dimensions are profile-specific and validated. X3 pages are
  528×792; no 480×800 constant is reused for their payload.
- Panel controllers and waveform LUTs differ on hardware. Those refresh-time details
  do not exist in an XTC page record; the converter preserves the shared pixel-level
  semantics that `XtcReaderActivity` decodes.

The statement that there is no separate X3 text-metric preset is an audit result:
the pinned EPUB layout/render sources contain no X3 branch that changes font metrics,
line compression, wrapping, or paragraph rules. X3-specific reader branches handle
framebuffer stride and panel-refresh memory paths. Therefore changing runtime geometry—
not inventing typography constants—is the fork-faithful implementation.

## Executable checks

- Native and WASM builds compile the same runtime-profile implementation.
- The X3 host smoke file is checked for 528×792 index entries and record headers,
  52,272-byte planes, and exact record lengths.
- Oracle conformance compiles the pinned fork and the port separately and renders the
  same X3 fixture through both.
- The preview/file gate checks every pixel for X3 and X4 in four orientations and both
  one-bit and two-bit formats.

See [`ko-landscape-modes.md`](ko-landscape-modes.md) for coordinate transforms,
margins, viewports, and the pre-rotated landscape record contract.
