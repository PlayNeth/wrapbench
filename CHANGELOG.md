# Changelog

## 1.0.2
- **The tests' own arithmetic no longer takes most of a frame.** d3d7-tl moved its 20,000
  triangles with 160,000 sines and cosines a frame: about 4 ms on a 2.7 GHz Xeon under Wine,
  of frames that took 4 to 4.5 ms with the wrappers compared, so the test measured itself
  and little of the wrapper. 2d-sprites and 2d-8bit spent 4,000 and 1,000 a frame the same way
  (about a tenth of a fast wrapper's 2d-sprites frame), d3d7-states 3,000. Each angle is now
  the sum of a part that changes with time (a few sines a frame) and a part from a table made
  once: d3d7-tl's vertices take 0.2 ms a frame on the same machine. The pictures move as
  before (within 0.0002 of a pixel). **d3d7-tl's results can't be compared with 1.0.1's**
  (they are much higher); 2d-sprites, 2d-8bit and d3d7-states rise a little with a fast
  wrapper.

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
