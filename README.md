# wrapbench

A benchmark for DirectDraw / Direct3D 1–7 wrappers: dd2d9, dgVoodoo, DDrawCompat, cnc-ddraw,
Wine's own `ddraw.dll`, Windows' own.

Put `wrapbench.exe` next to the `ddraw.dll` you want to measure, or in a folder without one to
measure Windows' or Wine's own, and run it. It goes fullscreen, runs 13 tests one after
another and shows the results at the end. They are also appended to `wrapbench.txt` next to the
.exe, together with the `ddraw.dll` that was loaded (its path, and the loaded module's version,
product name and image size in memory). Runs with different wrappers end up in one file, ready
to compare.

Every test uses only the public DirectDraw / Direct3D interfaces, the way the games did, so the
same .exe measures any wrapper.

Keys: Esc skips the current test, Q stops.

## Command line

```
wrapbench [-s seconds] [-w width] [-h height] [-bpp 16|32] [-vsync] [-only name,name...]
```

| Option | |
|---|---|
| `-s` | seconds measured per test (default 10), after a 2-second warm-up that isn't counted |
| `-w`, `-h` | the display mode (default 640x480) |
| `-bpp` | 16 (default) or 32; the 2d-8bit test always uses 8 |
| `-vsync` | flip with vsync (default: `DDFLIP_NOVSYNC`, so the frame rate isn't capped) |
| `-only` | just these tests, e.g. `-only d3d7-mesh,d3d7-vb` |

```
wrapbench -s 20 -w 1024 -h 768 -bpp 32
```

## The frame counter

Top left, in every test: the test's number, the frame rate of the last half second, and a bar
for the time gone. It is drawn through the API the test uses: colour-fill Blts in the 2D tests,
untextured transformed triangles in the 3D ones. It never goes through GDI, so every wrapper
pays the same small cost for it and nothing forces a read-back of the GPU's image.

## The tests

| # | Name | Each frame |
|---|---|---|
| 01 | 2d-fill | clear + 500 colour-fill Blts of 64x64 |
| 02 | 2d-sprites | clear + 2000 colour-keyed 32x32 BltFast sprites |
| 03 | 2d-stretch | 40 stretched Blts (64x64 up to the screen's size) |
| 04 | 2d-lock | the whole back buffer written through Lock (software rendering) |
| 05 | 2d-gdi | clear + 40 lines of text through GetDC/TextOut on the back buffer |
| 06 | 2d-8bit | 8-bit mode: palette animated + whole screen through Lock + 500 sprites |
| 07 | d3d7-tl | 20000 transformed (XYZRHW) triangles with one texture, unlit, 100 a DrawPrimitive |
| 08 | d3d7-mesh | 16 lit, textured spheres (32768 triangles), DrawIndexedPrimitive from memory |
| 09 | d3d7-vb | the same from a static (optimized) vertex buffer, DrawIndexedPrimitiveVB |
| 10 | d3d7-states | 3000 two-triangle draws, the texture and three blend states set before each |
| 11 | d3d6-mesh | the spheres through IDirect3DDevice3 (DirectX 6) |
| 12 | d3d5-mesh | the spheres through IDirect3DDevice2 with texture handles (DirectX 5) |
| 13 | d3d3-exec | the spheres through execute buffers, one Execute each (DirectX 1–3) |

The 3D tests use a 16-bit Z buffer, linear filtering and 8 textures of 128x128 RGB 565. The
sphere tests also use two lights (directional and point). Direct3D 7 gets a T&L HAL device
where there is one; the results line says which.

## Reading the results

```
01 2d-fill   3922.0 fps avg  4187.6 median  1436.5 1% low  0.99 ms worst  19609 frames  ...
```

| Column | Meaning |
|---|---|
| avg | frames / time: the main figure to compare |
| median | the typical frame's rate. A renderer that queues frames (Wine's wined3d, some wrappers) can return from Flip at once for many frames and then stall; the median is then meaningless and the average is what counts |
| 1% low | the rate over the slowest 1% of the frames: stutter |
| worst | the slowest frame |

Where the clock only ticks every few milliseconds (some Wine setups), the results say so. The
median and 1% low are then taken over runs of frames, and the worst frame is only as precise as
the clock.

A test whose drawing calls fail stops and names the call. A test whose Flip refused
`DDFLIP_NOVSYNC` says it flipped with vsync.

## Comparing fairly

- **Vsync:** let wrapbench decide it. It flips with `DDFLIP_NOVSYNC`, so a wrapper that follows
  the game's flags runs uncapped as it is. Turn off any vsync or frame limiter a wrapper forces
  on its own; the names vary by version, for example dgVoodoo's "Force vSync" and "FPS limit",
  DDrawCompat's `VSync` and `FpsLimiter`, cnc-ddraw's `vsync` and `maxfps`. For dd2d9 leave
  `vsync=auto`, the default. Or run them all with `-vsync` to compare at the refresh rate.
- **Same conditions:** use the same mode and the same scaling (window or fullscreen, resolution)
  for every wrapper, and close other programs.
- **Run each one twice:** the first run may still be compiling shaders.
- **FAILED tests:** a test that says FAILED names the first call that failed and its error code
  (a wrapper without Direct3D 1–3, a mode the display doesn't have, ...).

## Building

32-bit, as the games are, with mingw-w64 and meson:

```
meson setup build --cross-file build-win32.txt
ninja -C build
```

The source is one file, so the compiler alone works too:

```
i686-w64-mingw32-g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -mwindows \
    -o wrapbench.exe src/wrapbench.cpp -lddraw -lgdi32 -luser32 -lversion
```

## License

GPL-3.0-or-later (see `LICENSE`).
