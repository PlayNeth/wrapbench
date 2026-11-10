# Changelog

## 1.0.1
- **A coarse clock is also found from frames longer than its step.** 1.0.0 noticed a clock
  moving in steps (3.9 ms on some Wine setups) only where frames measured no time at all. A
  test whose frames took longer than a step (d3d7-tl: 4.5 ms a frame) measured each frame
  as one or two whole steps, so its median and 1% low were the clock's steps (256.0 and
  128.0 fps). Now a test whose frame times are nearly all a few values, a step apart, takes
  its median and 1% low over runs of frames too, and says so. The average was right and is
  unchanged.

## 1.0.0
The first release.

- **13 fullscreen tests:**
  - 2D: colour fills, colour-keyed sprites, stretched blits, Lock, GDI text, an 8-bit mode
    with an animated palette.
  - Direct3D 7: transformed triangles, lit meshes from memory and from a vertex buffer, state
    changes.
  - The same meshes through Direct3D 6 (IDirect3DDevice3), Direct3D 5 (IDirect3DDevice2) and
    Direct3D 1–3 execute buffers.
- **A frame counter** in each test, drawn through the API under test.
- **Results:** average, median, 1% low and worst frame. The median and 1% low are taken over
  runs of frames where the clock is coarse. Results are appended to `wrapbench.txt` with the
  `ddraw.dll` that was loaded.
- **Failures are named:** a setup step or drawing call that fails is named with its error
  code, and so is a Flip that refuses `DDFLIP_NOVSYNC`.
