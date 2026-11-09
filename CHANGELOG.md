# Changelog

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
