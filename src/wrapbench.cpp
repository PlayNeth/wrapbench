// wrapbench - a benchmark for DirectDraw / Direct3D 1-7 wrappers
// Copyright (C) 2026 Neth
//
// This program is free software: you can redistribute it and/or modify it under the terms of
// the GNU General Public License as published by the Free Software Foundation, either version
// 3 of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with this program.
// If not, see <https://www.gnu.org/licenses/>.
//
// wrapbench: a benchmark for DirectDraw wrappers (dd2d9, dgVoodoo, DDrawCompat, cnc-ddraw,
// Wine's own, Windows' own ...). Every test uses only the public DirectDraw / Direct3D 1-7
// interfaces, the way games of the era did, so the same .exe measures whichever ddraw.dll
// sits next to it.
//
//   wrapbench [-s seconds] [-w width] [-h height] [-bpp 16|32] [-vsync] [-only name,name...]
//
// Each test runs fullscreen (exclusive mode, a flip chain) for `seconds` (default 10) after a
// two-second warm-up, with an on-screen frame counter drawn through the API under test (Blt
// colour fills for the 2D tests, transformed triangles for the 3D ones, so every wrapper pays
// the same for it). Esc skips a test, Q stops. The results are appended to wrapbench.txt next
// to the .exe, with the ddraw.dll that was loaded, and shown when it ends.
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <ddraw.h>
#include <d3d.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

constexpr char kVersion[] = "1.0.2";

// ---------------------------------------------------------------- settings and results

struct Settings {
    double seconds = 10.0;
    int w = 640, h = 480, bpp = 16;
    bool vsync = false;
    std::vector<std::string> only;
} g_set;

HWND g_hwnd;
bool g_quit = false;  // Q: stop everything
bool g_skip = false;  // Esc: next test
FILE* g_out;
std::string g_summary;

void Out(const char* fmt, ...) {
    char b[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(b, sizeof(b), fmt, a);
    va_end(a);
    if (g_out) {
        fprintf(g_out, "%s\n", b);
        fflush(g_out);
    }
    g_summary += b;
    g_summary += "\n";
}

// The first setup step that failed, for the results ("why").
std::string g_why;
bool Ok(const char* step, HRESULT hr) {
    if (SUCCEEDED(hr)) return true;
    if (g_why.empty()) {
        char b[160];
        snprintf(b, sizeof(b), "%s: 0x%08lx", step, static_cast<unsigned long>(hr));
        g_why = b;
    }
    return false;
}

// A drawing call of a frame that failed: the test stops and says which (Run).
bool g_frameFailed = false;
void Ck(const char* step, HRESULT hr) {
    if (FAILED(hr)) {
        if (!g_frameFailed) Ok(step, hr);
        g_frameFailed = true;
    }
}
// The last Flip went without DDFLIP_NOVSYNC, which the interface refused (FlipChain).
bool g_vsyncForced = false;

double Now() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        return q;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(f.QuadPart);
}

// How often the clock advances (seconds). QueryPerformanceCounter normally ticks in well
// under a microsecond, but not everywhere (some Wine setups: 3.9 ms steps); single frames
// can't be timed then, only runs of them.
double g_tick = 0.0;
double ClockTick() {
    // The typical step (the median): a clock that mostly jumps 3.9 ms may now and then move
    // less, which the smallest step would take for its resolution.
    std::vector<double> steps;
    const double start = Now();
    double last = start;
    while (Now() - start < 0.2) {
        const double t = Now();
        if (t != last) {
            steps.push_back(t - last);
            last = t;
        }
    }
    if (steps.empty()) return 0.2;
    std::sort(steps.begin(), steps.end());
    return steps[steps.size() / 2];
}

// The clock's step as the frame times of a test show it, or 0 if they don't: with a coarse
// clock every frame measures a whole number of steps, so a few values, far apart, hold nearly
// all the frames (frames of 4.5 ms on a 3.9 ms clock measure 3.9 or 7.8 ms, never between).
// A fine clock gives either values that hardly repeat or, for very short frames, repeated
// values next to each other.
double SteppedClock(std::vector<double> times) {
    if (times.size() < 50) return 0.0;
    std::sort(times.begin(), times.end());
    // The values at least 0.5% of the frames (and two of them) measured.
    const size_t need = std::max<size_t>(2, times.size() / 200);
    double step = 1e9, last = 0.0;  // (0: a step is also what the smallest value is above none)
    size_t held = 0;
    for (size_t i = 0; i < times.size();) {
        size_t j = i;
        while (j < times.size() && times[j] - times[i] < 1e-9) j++;
        if (j - i >= need) {
            held += j - i;
            if (times[i] > 1e-9) step = std::min(step, times[i] - last);
            last = times[i];
        }
        i = j;
    }
    return held >= times.size() / 10 * 9 && step < 1e9 && step > 0.00005 ? step : 0.0;
}

bool Wanted(const char* name) {
    if (g_set.only.empty()) return true;
    for (const std::string& s : g_set.only)
        if (s == name) return true;
    return false;
}

void Pump() {
    MSG m;
    while (PeekMessageA(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_KEYDOWN) {
            if (m.wParam == VK_ESCAPE) g_skip = true;
            if (m.wParam == 'Q') g_quit = g_skip = true;
        }
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
}

// ---------------------------------------------------------------- the frame counter
// Seven-segment digits as rectangles, drawn by whatever the test draws with.

struct Rc {
    int x0, y0, x1, y1;
    bool white;
};

// Segments a..g of each digit (bit 0 = a, top; then clockwise; g the middle).
const uint8_t kSeg[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

void Digit(std::vector<Rc>* r, int x, int y, int s, int d) {
    const int t = std::max(2, s / 5), w = s, h = s * 2;
    const uint8_t m = kSeg[d];
    if (m & 0x01) r->push_back({x, y, x + w, y + t, true});
    if (m & 0x02) r->push_back({x + w - t, y, x + w, y + h / 2, true});
    if (m & 0x04) r->push_back({x + w - t, y + h / 2, x + w, y + h, true});
    if (m & 0x08) r->push_back({x, y + h - t, x + w, y + h, true});
    if (m & 0x10) r->push_back({x, y + h / 2, x + t, y + h, true});
    if (m & 0x20) r->push_back({x, y, x + t, y + h / 2, true});
    if (m & 0x40) r->push_back({x, y + h / 2 - t / 2, x + w, y + h / 2 + t - t / 2, true});
}

// The counter: the test's number, the frame rate of the last half second (one decimal), and
// a bar for the time gone. Black box behind it.
std::vector<Rc> Counter(int test, double fps, double progress) {
    std::vector<Rc> r;
    const int s = 12, x0 = 8, y0 = 8;
    char text[32];
    snprintf(text, sizeof(text), "%02d %6.1f", test, std::min(fps, 99999.9));
    const int n = int(strlen(text));
    r.push_back({x0 - 4, y0 - 4, x0 + n * (s + 6) + 4, y0 + 2 * s + 14, false});
    int x = x0;
    for (int i = 0; i < n; i++, x += s + 6) {
        if (text[i] >= '0' && text[i] <= '9') Digit(&r, x, y0, s, text[i] - '0');
        else if (text[i] == '.') r.push_back({x + s / 2 - 1, y0 + 2 * s - 3, x + s / 2 + 2, y0 + 2 * s, true});
    }
    const int bw = n * (s + 6);
    r.push_back({x0, y0 + 2 * s + 5, x0 + int(bw * std::clamp(progress, 0.0, 1.0)), y0 + 2 * s + 9, true});
    return r;
}

std::vector<D3DTLVERTEX> CounterTris(const std::vector<Rc>& rects) {
    std::vector<D3DTLVERTEX> v;
    for (const Rc& r : rects) {
        const D3DCOLOR c = r.white ? 0xFFFFFFFF : 0xFF000000;
        auto at = [&](int x, int y) {
            D3DTLVERTEX p = {};
            p.sx = float(x);
            p.sy = float(y);
            p.sz = 0.0f;
            p.rhw = 1.0f;
            p.color = c;
            p.specular = 0xFF000000;
            return p;
        };
        v.push_back(at(r.x0, r.y0));
        v.push_back(at(r.x1, r.y0));
        v.push_back(at(r.x0, r.y1));
        v.push_back(at(r.x1, r.y0));
        v.push_back(at(r.x1, r.y1));
        v.push_back(at(r.x0, r.y1));
    }
    return v;
}

// ---------------------------------------------------------------- the timing loop

struct Loop {
    std::function<bool(int frame, double t)> frame;         // draws a frame (false: failed)
    std::function<void(const std::vector<Rc>&)> counter;   // draws the counter
    std::function<HRESULT()> flip;
};

void Run(int number, const char* name, const char* what, Loop& l) {
    g_skip = false;
    std::vector<double> times;
    times.reserve(100000);
    const double warm = 2.0;  // (shaders made, textures uploaded, caches filled: not counted)
    const double t0 = Now();
    double last = t0, shownAt = t0, shownFps = 0.0;
    int shownFrames = 0, frame = 0;
    bool failed = false;
    HRESULT flipHr = DD_OK;
    g_frameFailed = false;
    g_why.clear();
    bool vsyncForced = false;
    while (!g_skip) {
        Pump();
        const double t = Now() - t0;
        if (t >= warm + g_set.seconds) break;
        if (!l.frame(frame, t)) {
            failed = true;
            break;
        }
        l.counter(Counter(number, shownFps, (t - warm) / g_set.seconds));
        flipHr = l.flip();
        vsyncForced |= g_vsyncForced;
        if (FAILED(flipHr) || g_frameFailed) {
            failed = true;
            break;
        }
        const double now = Now();
        if (now - t0 >= warm) times.push_back(now - last);
        last = now;
        frame++;
        shownFrames++;
        if (now - shownAt >= 0.5) {
            shownFps = shownFrames / (now - shownAt);
            shownAt = now;
            shownFrames = 0;
        }
    }
    if (failed && times.empty()) {
        if (FAILED(flipHr)) Out("%02d %-12s FAILED (Flip: 0x%08lx)   %s", number, name, static_cast<unsigned long>(flipHr), what);
        else Out("%02d %-12s FAILED (%s)   %s", number, name, g_why.empty() ? "a call of the frame failed" : g_why.c_str(), what);
        return;
    }
    if (times.empty()) {
        Out("%02d %-12s skipped", number, name);
        return;
    }
    double sum = 0.0;
    for (double d : times) sum += d;
    // The rates the median and the 1% low are taken over: each frame's, or with a coarse
    // clock, those of runs of frames at least 20 clock ticks (and 10 ms) long, timed whole.
    std::vector<double> spans;  // seconds a frame, one entry per frame or run
    std::vector<double> weight;  // frames in it
    // The clock moved in steps during this test: frames that took no time at all (the step
    // is then as large as the smallest time a frame did measure), or frame times that are
    // all a few values a step apart (SteppedClock).
    double tick = g_tick, smallest = 1e9;
    size_t zeros = 0;
    for (double d : times) {
        if (d <= 0.0) zeros++;
        else smallest = std::min(smallest, d);
    }
    const double stepped = SteppedClock(times);
    const bool steps = (zeros && smallest < 1e9) || stepped > 0.0;
    const bool coarseHere = steps && g_tick <= 0.00005;
    if (zeros && smallest < 1e9) tick = std::max(tick, smallest);
    tick = std::max(tick, stepped);
    // (A coarse clock found from the frames: runs of at least 0.1 s too, the smallest step
    // seen being possibly an odd short one.)
    const double minRun = tick > 0.00005 ? std::max({20 * tick, 0.01, steps ? 0.1 : 0.0}) : 0.0;
    double runTime = 0.0;
    int runFrames = 0;
    for (double d : times) {
        runTime += d;
        runFrames++;
        if (runTime >= minRun) {
            spans.push_back(runTime / runFrames);
            weight.push_back(runFrames);
            runTime = 0.0;
            runFrames = 0;
        }
    }
    if (runFrames) spans.push_back(runTime / runFrames), weight.push_back(runFrames);  // (the last, shorter run)
    std::vector<size_t> order(spans.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return spans[a] < spans[b]; });
    // Median: the frame time half the frames are faster than; 1% low: the rate over the
    // slowest 1% of the frames (at least one frame or run).
    double total = 0.0, acc = 0.0, median = spans[order.back()];
    for (double w : weight) total += w;
    for (size_t i : order) {
        acc += weight[i];
        if (acc >= total / 2) {
            median = spans[i];
            break;
        }
    }
    double slowFrames = 0.0, slowTime = 0.0;
    for (size_t k = order.size(); k-- > 0 && (slowFrames < total / 100 || slowFrames == 0);) {
        slowFrames += weight[order[k]];
        slowTime += weight[order[k]] * spans[order[k]];
    }
    const double worst = *std::max_element(times.begin(), times.end());
    if (failed && FAILED(flipHr) && g_why.empty()) {
        char b[48];
        snprintf(b, sizeof(b), "Flip: 0x%08lx", static_cast<unsigned long>(flipHr));
        g_why = b;
    }
    std::string note = failed ? "  (stopped: " + (g_why.empty() ? std::string("error") : g_why) + ")" : g_skip ? "  (skipped)" : "";
    if (vsyncForced) note += "  (Flip refused DDFLIP_NOVSYNC: flipped with vsync)";
    if (coarseHere) note += "  (the clock moved in steps: median and 1% low over runs of frames)";
    Out("%02d %-12s %8.1f fps avg  %8.1f median  %8.1f 1%% low  %7.2f ms worst  %6zu frames%s   %s", number, name,
        double(times.size()) / sum, 1.0 / median, slowFrames / slowTime, 1000.0 * worst, times.size(), note.c_str(),
        what);
}

DWORD FlipFlags() { return DDFLIP_WAIT | (g_set.vsync ? 0 : DDFLIP_NOVSYNC); }

// Some interfaces before DirectX 7 refuse DDFLIP_NOVSYNC: then without it.
template <class S> HRESULT FlipChain(S* primary) {
    static bool noVsyncRefused = false;
    HRESULT hr = primary->Flip(nullptr, noVsyncRefused ? DDFLIP_WAIT : FlipFlags());
    if (hr == DDERR_INVALIDPARAMS && !noVsyncRefused && !g_set.vsync) {
        noVsyncRefused = true;
        hr = primary->Flip(nullptr, DDFLIP_WAIT);
    }
    g_vsyncForced = noVsyncRefused && !g_set.vsync;
    return hr;
}

// ---------------------------------------------------------------- shared geometry

D3DMATRIX Identity() {
    D3DMATRIX m = {};
    m._11 = m._22 = m._33 = m._44 = 1.0f;
    return m;
}

D3DMATRIX Perspective(float fov, float aspect, float zn, float zf) {
    D3DMATRIX m = {};
    const float h = 1.0f / tanf(fov * 0.5f), w = h / aspect, q = zf / (zf - zn);
    m._11 = w;
    m._22 = h;
    m._33 = q;
    m._34 = 1.0f;
    m._43 = -q * zn;
    return m;
}

// Rotation about Y then a translation.
D3DMATRIX World(float angle, float x, float y, float z) {
    D3DMATRIX m = Identity();
    m._11 = cosf(angle), m._13 = -sinf(angle), m._31 = sinf(angle), m._33 = cosf(angle);
    m._41 = x, m._42 = y, m._43 = z;
    return m;
}

// A sphere of radius r: (g+1)^2 vertices, 2 g^2 triangles.
void Sphere(int g, float r, std::vector<D3DVERTEX>* v, std::vector<WORD>* idx) {
    v->clear();
    idx->clear();
    for (int i = 0; i <= g; i++) {
        const float th = 3.14159265f * float(i) / float(g);
        for (int j = 0; j <= g; j++) {
            const float ph = 2.0f * 3.14159265f * float(j) / float(g);
            D3DVERTEX p = {};
            p.nx = sinf(th) * cosf(ph);
            p.ny = cosf(th);
            p.nz = sinf(th) * sinf(ph);
            p.x = r * p.nx;
            p.y = r * p.ny;
            p.z = r * p.nz;
            p.tu = 4.0f * float(j) / float(g);
            p.tv = 2.0f * float(i) / float(g);
            v->push_back(p);
        }
    }
    for (int i = 0; i < g; i++)
        for (int j = 0; j < g; j++) {
            const WORD a = WORD(i * (g + 1) + j), b = WORD(a + 1), c = WORD(a + g + 1), d = WORD(c + 1);
            idx->insert(idx->end(), {a, b, c, b, d, c});
        }
}

// The mesh tests: kSpheres spheres of kGrid (2 kGrid^2 triangles each) in a grid, turning.
constexpr int kSpheres = 16, kGrid = 32;
D3DMATRIX SphereWorld(int i, double t) {
    const float x = float(i % 4) * 1.6f - 2.4f, y = float(i / 4) * 1.2f - 1.8f;
    return World(float(t) * 0.8f + float(i), x, y, 6.0f);
}

// A 128x128 texture's texels in RGB 565: a coloured checker with a gradient (variant: hue).
void TextureTexels(uint8_t* bits, LONG pitch, int variant) {
    for (int y = 0; y < 128; y++) {
        uint16_t* row = reinterpret_cast<uint16_t*>(bits + y * pitch);
        for (int x = 0; x < 128; x++) {
            const bool c = ((x >> 4) ^ (y >> 4)) & 1;
            const int r = c ? 31 : (x >> 2), g = c ? (y >> 1) : 20 + variant * 5, b = c ? (variant * 4) & 31 : 31 - (y >> 2);
            row[x] = uint16_t(r << 11 | (g & 63) << 5 | (b & 31));
        }
    }
}

template <class D> void Rgb565(D* pf) {
    pf->dwSize = sizeof(*pf);
    pf->dwFlags = DDPF_RGB;
    pf->dwRGBBitCount = 16;
    pf->dwRBitMask = 0xF800;
    pf->dwGBitMask = 0x07E0;
    pf->dwBBitMask = 0x001F;
}

// The light setup every 3D test uses: one directional light, one point light.
void LightValues(D3DCOLORVALUE* c0, D3DVECTOR* d0, D3DCOLORVALUE* c1, D3DVECTOR* p1) {
    *c0 = {0.9f, 0.85f, 0.8f, 1.0f};
    *d0 = {0.4f, -0.7f, 0.6f};
    *c1 = {0.3f, 0.6f, 1.0f, 1.0f};
    *p1 = {-2.0f, 2.0f, 3.0f};
}

// ---------------------------------------------------------------- DirectDraw 7: setup

struct DD7 {
    IDirectDraw7* dd = nullptr;
    IDirectDrawSurface7* primary = nullptr;
    IDirectDrawSurface7* back = nullptr;
    IDirectDrawSurface7* z = nullptr;
    IDirectDrawPalette* pal = nullptr;
    DDPIXELFORMAT pf = {};
    std::vector<IDirectDrawSurface7*> owned;  // the test's own surfaces: released before the object
    ~DD7() {
        for (IDirectDrawSurface7* s : owned) s->Release();
        if (pal) pal->Release();
        if (z) z->Release();
        if (back) back->Release();
        if (primary) primary->Release();
        if (dd) {
            dd->RestoreDisplayMode();
            dd->SetCooperativeLevel(g_hwnd, DDSCL_NORMAL);
            dd->Release();
        }
    }
    bool Open(int bpp, bool threeD) {
        if (!Ok("DirectDrawCreateEx", DirectDrawCreateEx(nullptr, reinterpret_cast<void**>(&dd), IID_IDirectDraw7, nullptr))) return false;
        if (!Ok("SetCooperativeLevel", dd->SetCooperativeLevel(g_hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_FPUSETUP))) return false;
        if (!Ok("SetDisplayMode", dd->SetDisplayMode(g_set.w, g_set.h, bpp, 0, 0))) return false;
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        d.dwBackBufferCount = 1;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | (threeD ? DDSCAPS_3DDEVICE : 0);
        if (!Ok("CreateSurface", dd->CreateSurface(&d, &primary, nullptr))) return false;
        DDSCAPS2 caps = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
        if (!Ok("GetAttachedSurface", primary->GetAttachedSurface(&caps, &back))) return false;
        pf.dwSize = sizeof(pf);
        primary->GetPixelFormat(&pf);
        if (bpp == 8) {
            PALETTEENTRY e[256];
            for (int i = 0; i < 256; i++) e[i] = {BYTE(i), BYTE(255 - i), BYTE((i * 3) & 255), 0};
            e[0] = {0, 0, 0, 0};
            e[255] = {255, 255, 255, 0};
            if (!Ok("CreatePalette", dd->CreatePalette(DDPCAPS_8BIT | DDPCAPS_ALLOW256, e, &pal, nullptr))) return false;
            primary->SetPalette(pal);
        }
        if (threeD) {
            DDSURFACEDESC2 zd = {};
            zd.dwSize = sizeof(zd);
            zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            zd.dwWidth = g_set.w;
            zd.dwHeight = g_set.h;
            zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
            zd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            zd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
            zd.ddpfPixelFormat.dwZBufferBitDepth = 16;
            zd.ddpfPixelFormat.dwZBitMask = 0xFFFF;
            if (!Ok("CreateSurface(Z)", dd->CreateSurface(&zd, &z, nullptr)) || !Ok("AddAttachedSurface(Z)", back->AddAttachedSurface(z)))
                return false;
        }
        return true;
    }
    // A colour of the screen's format from 8-bit RGB (8-bit: the palette's black or white).
    DWORD Color(int r, int g, int b) const {
        if (pf.dwRGBBitCount == 8) return (r + g + b) > 384 ? 255 : 0;
        auto put = [](int v, DWORD mask) {
            if (!mask) return DWORD(0);
            int shift = 0;
            while (!((mask >> shift) & 1)) shift++;
            const DWORD max = mask >> shift;
            return (DWORD(v) * max / 255) << shift;
        };
        return put(r, pf.dwRBitMask) | put(g, pf.dwGBitMask) | put(b, pf.dwBBitMask);
    }
    void Fill(const RECT& r, DWORD color) {
        DDBLTFX fx = {};
        fx.dwSize = sizeof(fx);
        fx.dwFillColor = color;
        RECT c = r;
        Ck("Blt (colour fill)", back->Blt(&c, nullptr, nullptr, DDBLT_COLORFILL | DDBLT_WAIT, &fx));
    }
    void Counter2D(const std::vector<Rc>& rects) {
        const DWORD white = Color(255, 255, 255), black = Color(0, 0, 0);
        for (const Rc& r : rects)
            if (r.x1 > r.x0 && r.y1 > r.y0) Fill(RECT{r.x0, r.y0, r.x1, r.y1}, r.white ? white : black);
    }
    IDirectDrawSurface7* Offscreen(int w, int h) {
        DDSURFACEDESC2 d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        d.dwWidth = w;
        d.dwHeight = h;
        d.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN;
        IDirectDrawSurface7* s = nullptr;
        if (!Ok("CreateSurface(offscreen)", dd->CreateSurface(&d, &s, nullptr))) return nullptr;
        owned.push_back(s);
        return s;
    }
};

// Writes a sprite into a surface (any depth): a ball on the key colour (0), lit from the left.
void SpriteTexels(IDirectDrawSurface7* s, const DD7& d, int size) {
    DDSURFACEDESC2 sd = {};
    sd.dwSize = sizeof(sd);
    if (FAILED(s->Lock(nullptr, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) return;
    const int bytes = int(sd.ddpfPixelFormat.dwRGBBitCount / 8);
    for (int y = 0; y < size; y++) {
        uint8_t* row = static_cast<uint8_t*>(sd.lpSurface) + y * sd.lPitch;
        for (int x = 0; x < size; x++) {
            const float dx = (x - size / 2 + 0.5f) / (size / 2), dy = (y - size / 2 + 0.5f) / (size / 2);
            const float r2 = dx * dx + dy * dy;
            DWORD c = 0;
            if (r2 < 1.0f) {
                const int l = int(255 * std::clamp(0.3f + 0.7f * (1.0f - r2) - 0.3f * dx, 0.05f, 1.0f));
                c = bytes == 1 ? DWORD(std::max(1, l)) : d.Color(l, l / 2 + 60, 255 - l / 2);
                if (!c) c = 1;
            }
            memcpy(row + x * bytes, &c, bytes);
        }
    }
    s->Unlock(nullptr);
    DDCOLORKEY key = {0, 0};
    s->SetColorKey(DDCKEY_SRCBLT, &key);
}

// The tests' own arithmetic is kept small against what they measure: where a frame moves
// thousands of things along sines, each angle is a sum of a part that changes with time (a
// few values a frame) and a part that doesn't (a table made once), and the sine of the sum
// comes from the sines and cosines of the parts. (1.0.1's d3d7-tl took 160,000 sines and
// cosines a frame, most of its frame time with a fast wrapper.)
struct Angle {
    float s, c;
    Angle() : s(0.0f), c(1.0f) {}
    explicit Angle(double a) : s(float(sin(a))), c(float(cos(a))) {}
};
inline float SinSum(const Angle& a, const Angle& b) { return a.s * b.c + a.c * b.s; }
inline float CosSum(const Angle& a, const Angle& b) { return a.c * b.c - a.s * b.s; }

// ---------------------------------------------------------------- 2D tests

uint32_t g_rng = 12345;
int Rand(int n) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return int((g_rng >> 8) % uint32_t(n));
}

void Test2D(int number, const char* name, int bpp, const char* what,
            const std::function<bool(DD7&, int, double)>& body, const std::function<bool(DD7&)>& setup = nullptr) {
    if (!Wanted(name) || g_quit) return;
    g_why.clear();
    DD7 d;
    if (!d.Open(bpp, false) || (setup && !setup(d))) {
        Out("%02d %-12s FAILED to set up %dx%dx%d (%s)   %s", number, name, g_set.w, g_set.h, bpp, g_why.empty() ? "a step without an error code" : g_why.c_str(), what);
        return;
    }
    Loop l;
    l.frame = [&](int f, double t) { return body(d, f, t); };
    l.counter = [&](const std::vector<Rc>& r) { d.Counter2D(r); };
    l.flip = [&] { return FlipChain(d.primary); };
    Run(number, name, what, l);
}

void Tests2D() {
    const int W = g_set.w, H = g_set.h, bpp = g_set.bpp;
    char what[128];

    snprintf(what, sizeof(what), "clear + 500 colour-fill Blts of 64x64 a frame");
    Test2D(1, "2d-fill", bpp, what, [&](DD7& d, int, double) {
        d.Fill(RECT{0, 0, W, H}, d.Color(0, 0, 40));
        for (int i = 0; i < 500; i++) {
            const int x = Rand(W - 64), y = Rand(H - 64);
            d.Fill(RECT{x, y, x + 64, y + 64}, d.Color(Rand(256), Rand(256), Rand(256)));
        }
        return true;
    });

    IDirectDrawSurface7* sprite = nullptr;
    auto makeSprite = [&](DD7& d) {
        sprite = d.Offscreen(32, 32);
        if (sprite) SpriteTexels(sprite, d, 32);
        return sprite != nullptr;
    };
    // Sprite i moves along sin(a * 1.3 + i * 0.37) and cos(a * 0.9 + i * 0.61), with
    // a = t * (0.5 + (i % 7) * 0.1) + i: seven speeds (the parts with t), and each sprite's
    // own phase (the parts with i).
    std::vector<Angle> spriteX(2000), spriteY(2000);
    for (int i = 0; i < 2000; i++) spriteX[i] = Angle(i * (1.3 + 0.37)), spriteY[i] = Angle(i * (0.9 + 0.61));
    auto sprites = [&](DD7& d, int count, double t) {
        Angle speedX[7], speedY[7];
        for (int m = 0; m < 7; m++) speedX[m] = Angle(t * (0.5 + m * 0.1) * 1.3), speedY[m] = Angle(t * (0.5 + m * 0.1) * 0.9);
        for (int i = 0; i < count; i++) {
            const int x = int(float(W - 32) * (0.5f + 0.5f * SinSum(speedX[i % 7], spriteX[i])));
            const int y = int(float(H - 32) * (0.5f + 0.5f * CosSum(speedY[i % 7], spriteY[i])));
            RECT src = {0, 0, 32, 32};
            Ck("BltFast (colour-keyed)", d.back->BltFast(x, y, sprite, &src, DDBLTFAST_SRCCOLORKEY | DDBLTFAST_WAIT));
        }
    };
    Test2D(2, "2d-sprites", bpp, "clear + 2000 colour-keyed 32x32 BltFast sprites a frame",
           [&](DD7& d, int, double t) {
               d.Fill(RECT{0, 0, W, H}, d.Color(20, 40, 20));
               sprites(d, 2000, t);
               return true;
           },
           makeSprite);
    sprite = nullptr;  // (released with its DirectDraw object)

    IDirectDrawSurface7* pic = nullptr;
    Test2D(3, "2d-stretch", bpp, "40 stretched Blts (64x64 to up to the screen's size) a frame",
           [&](DD7& d, int, double t) {
               d.Fill(RECT{0, 0, W, H}, d.Color(0, 0, 0));
               for (int i = 0; i < 40; i++) {
                   const double s = 0.2 + 0.8 * (0.5 + 0.5 * sin(t + i * 0.4));
                   const int w = std::max(8, int(W * s)), h = std::max(8, int(H * s));
                   const int x = Rand(W - w + 1), y = Rand(H - h + 1);
                   RECT dst = {x, y, x + w, y + h};
                   Ck("Blt (stretched)", d.back->Blt(&dst, pic, nullptr, DDBLT_WAIT, nullptr));
               }
               return true;
           },
           [&](DD7& d) {
               pic = d.Offscreen(64, 64);
               if (pic) SpriteTexels(pic, d, 64);
               return pic != nullptr;
           });
    pic = nullptr;

    Test2D(4, "2d-lock", bpp, "the whole back buffer written through Lock a frame (software rendering)",
           [&](DD7& d, int f, double) {
               DDSURFACEDESC2 sd = {};
               sd.dwSize = sizeof(sd);
               if (!Ok("Lock", d.back->Lock(nullptr, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) return false;
               const int bytes = int(sd.ddpfPixelFormat.dwRGBBitCount / 8);
               for (int y = 0; y < H; y++) {
                   uint8_t* row = static_cast<uint8_t*>(sd.lpSurface) + y * sd.lPitch;
                   if (bytes == 2) {
                       uint16_t* p = reinterpret_cast<uint16_t*>(row);
                       for (int x = 0; x < W; x++) p[x] = uint16_t((x + f) ^ (y * 3));
                   } else if (bytes == 4) {
                       uint32_t* p = reinterpret_cast<uint32_t*>(row);
                       for (int x = 0; x < W; x++) p[x] = uint32_t(((x + f) & 255) << 16 | (y & 255) << 8 | ((x ^ y) & 255));
                   } else {
                       for (int x = 0; x < W * bytes; x++) row[x] = uint8_t(x + y + f);
                   }
               }
               d.back->Unlock(nullptr);
               return true;
           });

    Test2D(5, "2d-gdi", bpp, "clear + 40 lines of text through GetDC/TextOut on the back buffer a frame",
           [&](DD7& d, int f, double) {
               d.Fill(RECT{0, 0, W, H}, d.Color(30, 30, 60));
               HDC dc = nullptr;
               if (!Ok("GetDC", d.back->GetDC(&dc))) return false;
               SetBkMode(dc, TRANSPARENT);
               SetTextColor(dc, RGB(255, 255, 160));
               char line[96];
               for (int i = 0; i < 40; i++) {
                   const int n = snprintf(line, sizeof(line), "Line %02d of the GDI test, frame %d: the quick brown fox", i, f);
                   TextOutA(dc, 20 + (i % 3) * 8, 60 + i * (H - 80) / 40, line, n);
               }
               d.back->ReleaseDC(dc);
               return true;
           });

    // An 8-bit mode as most DirectDraw games of the era ran: a palette animated every frame,
    // the screen written through Lock, sprites on top.
    Test2D(6, "2d-8bit", 8, "8-bit mode: palette animated + whole screen through Lock + 500 sprites a frame",
           [&](DD7& d, int f, double t) {
               PALETTEENTRY e[254];
               for (int i = 0; i < 254; i++) {
                   const int v = (i + f) & 255;
                   e[i] = {BYTE(v), BYTE((v * 2) & 255), BYTE(255 - v), 0};
               }
               Ck("SetEntries", d.pal->SetEntries(0, 1, 254, e));
               DDSURFACEDESC2 sd = {};
               sd.dwSize = sizeof(sd);
               if (!Ok("Lock", d.back->Lock(nullptr, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) return false;
               for (int y = 0; y < H; y++) {
                   uint8_t* row = static_cast<uint8_t*>(sd.lpSurface) + y * sd.lPitch;
                   for (int x = 0; x < W; x++) row[x] = uint8_t(1 + ((x / 4 + y / 4) % 254));
               }
               d.back->Unlock(nullptr);
               sprites(d, 500, t);
               return true;
           },
           makeSprite);
    sprite = nullptr;  // (released with its DirectDraw object)
}

// ---------------------------------------------------------------- Direct3D 7

struct Mesh {
    std::vector<D3DVERTEX> v;
    std::vector<WORD> idx;
    Mesh() { Sphere(kGrid, 0.55f, &v, &idx); }
};

IDirectDrawSurface7* Texture7(IDirectDraw7* dd, int variant) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = 128;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    d.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    Rgb565(&d.ddpfPixelFormat);
    IDirectDrawSurface7* t = nullptr;
    if (!Ok("CreateSurface(texture)", dd->CreateSurface(&d, &t, nullptr))) return nullptr;
    DDSURFACEDESC2 l = {};
    l.dwSize = sizeof(l);
    if (SUCCEEDED(t->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) {
        TextureTexels(static_cast<uint8_t*>(l.lpSurface), l.lPitch, variant);
        t->Unlock(nullptr);
    }
    return t;
}

struct D3D7Ctx {
    DD7 d;
    IDirect3D7* d3d = nullptr;
    IDirect3DDevice7* dev = nullptr;
    IDirectDrawSurface7* tex[8] = {};
    const char* kind = "HAL";
    std::vector<IUnknown*> owned;  // the test's own objects: released before the device
    ~D3D7Ctx() {
        for (IUnknown* o : owned) o->Release();
        for (auto* t : tex)
            if (t) t->Release();
        if (dev) dev->Release();
        if (d3d) d3d->Release();
    }
    bool Open() {
        if (!d.Open(g_set.bpp, true)) return false;
        if (!Ok("QueryInterface", d.dd->QueryInterface(IID_IDirect3D7, reinterpret_cast<void**>(&d3d)))) return false;
        if (SUCCEEDED(d3d->CreateDevice(IID_IDirect3DTnLHalDevice, d.back, &dev))) kind = "T&L HAL";
        else if (!Ok("CreateDevice(HAL)", d3d->CreateDevice(IID_IDirect3DHALDevice, d.back, &dev))) return false;
        D3DVIEWPORT7 vp = {0, 0, DWORD(g_set.w), DWORD(g_set.h), 0.0f, 1.0f};
        dev->SetViewport(&vp);
        for (int i = 0; i < 8; i++)
            if (!(tex[i] = Texture7(d.dd, i))) return false;
        D3DMATRIX view = Identity(), proj = Perspective(1.0f, float(g_set.w) / float(g_set.h), 0.5f, 50.0f);
        dev->SetTransform(D3DTRANSFORMSTATE_VIEW, &view);
        dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &proj);
        D3DLIGHT7 l[2] = {};
        LightValues(&l[0].dcvDiffuse, &l[0].dvDirection, &l[1].dcvDiffuse, &l[1].dvPosition);
        l[0].dltType = D3DLIGHT_DIRECTIONAL;
        l[1].dltType = D3DLIGHT_POINT;
        l[1].dvRange = 20.0f;
        l[1].dvAttenuation0 = 1.0f;
        for (int i = 0; i < 2; i++) {
            dev->SetLight(i, &l[i]);
            dev->LightEnable(i, TRUE);
        }
        D3DMATERIAL7 m = {};
        m.dcvDiffuse = {1.0f, 1.0f, 1.0f, 1.0f};
        m.dcvAmbient = {1.0f, 1.0f, 1.0f, 1.0f};
        dev->SetMaterial(&m);
        dev->SetRenderState(D3DRENDERSTATE_AMBIENT, 0x00303030);
        return true;
    }
    void Begin(DWORD clear) {
        Ck("Clear", dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clear, 1.0f, 0));
        Ck("BeginScene", dev->BeginScene());
        dev->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_LIGHTING, TRUE);
        dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_CCW);
        dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        dev->SetTexture(0, tex[0]);
        dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
        dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
    }
    void Counter(const std::vector<Rc>& r) {
        std::vector<D3DTLVERTEX> v = CounterTris(r);
        dev->SetTexture(0, nullptr);
        dev->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
        dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
        Ck("DrawPrimitive (counter)", dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v.data(), DWORD(v.size()), 0));
        Ck("EndScene", dev->EndScene());
    }
};

void Test3D7(int number, const char* name, const char* what, const std::function<bool(D3D7Ctx&)>& setup,
             const std::function<void(D3D7Ctx&, int, double)>& body) {
    if (!Wanted(name) || g_quit) return;
    g_why.clear();
    D3D7Ctx c;
    if (!c.Open() || (setup && !setup(c))) {
        Out("%02d %-12s FAILED to set up Direct3D 7 (%s)   %s", number, name, g_why.empty() ? "a step without an error code" : g_why.c_str(), what);
        return;
    }
    std::string full = std::string(what) + " (" + c.kind + " device)";
    Loop l;
    l.frame = [&](int f, double t) {
        body(c, f, t);
        return true;
    };
    l.counter = [&](const std::vector<Rc>& r) { c.Counter(r); };
    l.flip = [&] { return FlipChain(c.d.primary); };
    Run(number, name, full.c_str(), l);
}

void TestsD3D7() {
    const int W = g_set.w, H = g_set.h;
    // Transformed and lit triangles (what 2D-on-3D games and many D3D 3-6 games send).
    std::vector<D3DTLVERTEX> tl(300);
    // Triangle k of call `call` is centred at sin(t * 0.7 + call * 0.13 + k * 0.071) and
    // cos(t * 0.5 + call * 0.29 + k * 0.053), its corner j at the angle j * 2.094 + t + k.
    Angle triX[100], triY[100], corner[100][3];
    for (int k = 0; k < 100; k++) {
        triX[k] = Angle(k * 0.071), triY[k] = Angle(k * 0.053);
        for (int j = 0; j < 3; j++) corner[k][j] = Angle(double(float(j) * 2.094f + float(k)));
    }
    Test3D7(7, "d3d7-tl", "20000 transformed textured triangles a frame, 100 a DrawPrimitive", nullptr,
            [&](D3D7Ctx& c, int, double t) {
                c.Begin(0xFF102030);
                c.dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
                c.dev->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
                const Angle spin(t);
                for (int call = 0; call < 200; call++) {
                    const Angle callX(t * 0.7 + call * 0.13), callY(t * 0.5 + call * 0.29);
                    for (int k = 0; k < 100; k++) {
                        const float cx = float(W) * (0.5f + 0.45f * SinSum(callX, triX[k]));
                        const float cy = float(H) * (0.5f + 0.45f * CosSum(callY, triY[k]));
                        for (int j = 0; j < 3; j++) {
                            D3DTLVERTEX& p = tl[k * 3 + j];
                            p.sx = cx + 16.0f * CosSum(spin, corner[k][j]);
                            p.sy = cy + 16.0f * SinSum(spin, corner[k][j]);
                            p.sz = 0.5f;
                            p.rhw = 1.0f;
                            p.color = 0xFF000000 | (DWORD(call * 5) & 0xFF) << 16 | DWORD(k * 2) << 8 | 0xC0;
                            p.specular = 0xFF000000;
                            p.tu = float(j & 1);
                            p.tv = float(j >> 1);
                        }
                    }
                    Ck("DrawPrimitive", c.dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, tl.data(), 300, 0));
                }
            });

    Mesh mesh;
    const DWORD nv = DWORD(mesh.v.size()), ni = DWORD(mesh.idx.size());
    auto spheres = [&](D3D7Ctx& c, double t, IDirect3DVertexBuffer7* vb) {
        c.Begin(0xFF203040);
        for (int i = 0; i < kSpheres; i++) {
            D3DMATRIX w = SphereWorld(i, t);
            c.dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &w);
            c.dev->SetTexture(0, c.tex[i & 7]);
            if (vb) Ck("DrawIndexedPrimitiveVB", c.dev->DrawIndexedPrimitiveVB(D3DPT_TRIANGLELIST, vb, 0, nv, mesh.idx.data(), ni, 0));
            else
                Ck("DrawIndexedPrimitive",
                   c.dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_VERTEX, mesh.v.data(), nv, mesh.idx.data(), ni, 0));
        }
    };
    char what[160];
    snprintf(what, sizeof(what), "%d lit textured spheres (%d triangles), DrawIndexedPrimitive from memory", kSpheres,
             kSpheres * 2 * kGrid * kGrid);
    Test3D7(8, "d3d7-mesh", what, nullptr, [&](D3D7Ctx& c, int, double t) { spheres(c, t, nullptr); });

    IDirect3DVertexBuffer7* vb = nullptr;
    snprintf(what, sizeof(what), "the same spheres from a static vertex buffer (DrawIndexedPrimitiveVB)");
    Test3D7(9, "d3d7-vb", what,
            [&](D3D7Ctx& c) {
                D3DVERTEXBUFFERDESC vd = {sizeof(vd), 0, D3DFVF_VERTEX, nv};
                if (!Ok("CreateVertexBuffer", c.d3d->CreateVertexBuffer(&vd, &vb, 0))) return false;
                c.owned.push_back(vb);
                void* p = nullptr;
                if (!Ok("Lock", vb->Lock(DDLOCK_WAIT | DDLOCK_WRITEONLY, &p, nullptr))) return false;
                memcpy(p, mesh.v.data(), nv * sizeof(D3DVERTEX));
                vb->Unlock();
                vb->Optimize(c.dev, 0);
                return true;
            },
            [&](D3D7Ctx& c, int, double t) { spheres(c, t, vb); });
    vb = nullptr;  // (released with its device)

    // Many small draws with state changes between them (the cost per call and per change).
    D3DVERTEX quad[4] = {};
    std::vector<Angle> quadZ(3000);  // quad i's depth moves along sin(t + i * 0.01)
    for (int i = 0; i < 3000; i++) quadZ[i] = Angle(i * 0.01);
    Test3D7(10, "d3d7-states", "3000 two-triangle draws a frame, the texture and three blend states set before each", nullptr,
            [&](D3D7Ctx& c, int, double t) {
                c.Begin(0xFF000000);
                c.dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
                D3DMATRIX w = Identity();
                c.dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &w);
                const Angle now(t);
                for (int i = 0; i < 3000; i++) {
                    const float x = float(i % 60) * 0.12f - 3.6f, y = float(i / 60) * 0.1f - 2.5f;
                    const float z = 5.0f + 0.5f * SinSum(now, quadZ[i]);
                    const float s = 0.05f;
                    const float px[4] = {x - s, x + s, x - s, x + s}, py[4] = {y + s, y + s, y - s, y - s};
                    for (int k = 0; k < 4; k++) {
                        quad[k].x = px[k], quad[k].y = py[k], quad[k].z = z;
                        quad[k].nz = -1.0f;
                        quad[k].tu = float(k & 1), quad[k].tv = float(k >> 1);
                    }
                    c.dev->SetTexture(0, c.tex[i & 7]);
                    c.dev->SetRenderState(D3DRENDERSTATE_ALPHABLENDENABLE, (i & 8) ? TRUE : FALSE);
                    c.dev->SetRenderState(D3DRENDERSTATE_SRCBLEND, (i & 16) ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
                    c.dev->SetRenderState(D3DRENDERSTATE_DESTBLEND, (i & 16) ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
                    Ck("DrawPrimitive", c.dev->DrawPrimitive(D3DPT_TRIANGLESTRIP, D3DFVF_VERTEX, quad, 4, 0));
                }
            });
}

// ---------------------------------------------------------------- DirectDraw 1-6 setup (Direct3D 2-6)

// A flip chain through IDirectDraw (DirectX 1-5) or IDirectDraw4 (DirectX 6) with a 16-bit Z
// buffer, the device and its viewport made by the test.
struct DDOld {
    IDirectDraw* dd1 = nullptr;
    IDirectDraw4* dd4 = nullptr;
    IDirectDrawSurface* primary1 = nullptr;
    IDirectDrawSurface* back1 = nullptr;
    IDirectDrawSurface4* primary4 = nullptr;
    IDirectDrawSurface4* back4 = nullptr;
    ~DDOld() {
        if (back4) back4->Release();
        if (primary4) primary4->Release();
        if (back1) back1->Release();
        if (primary1) primary1->Release();
        if (dd4) dd4->Release();
        if (dd1) {
            dd1->RestoreDisplayMode();
            dd1->SetCooperativeLevel(g_hwnd, DDSCL_NORMAL);
            dd1->Release();
        }
    }
    bool Open(bool four) {
        if (!Ok("DirectDrawCreate", DirectDrawCreate(nullptr, &dd1, nullptr))) return false;
        if (!Ok("SetCooperativeLevel", dd1->SetCooperativeLevel(g_hwnd, DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN))) return false;
        if (four) {
            if (!Ok("QueryInterface", dd1->QueryInterface(IID_IDirectDraw4, reinterpret_cast<void**>(&dd4)))) return false;
            if (!Ok("SetDisplayMode", dd4->SetDisplayMode(g_set.w, g_set.h, g_set.bpp, 0, 0))) return false;
            DDSURFACEDESC2 d = {};
            d.dwSize = sizeof(d);
            d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
            d.dwBackBufferCount = 1;
            d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
            if (!Ok("CreateSurface", dd4->CreateSurface(&d, &primary4, nullptr))) return false;
            DDSCAPS2 caps = {DDSCAPS_BACKBUFFER, 0, 0, {0}};
            if (!Ok("GetAttachedSurface", primary4->GetAttachedSurface(&caps, &back4))) return false;
            DDSURFACEDESC2 zd = {};
            zd.dwSize = sizeof(zd);
            zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
            zd.dwWidth = g_set.w;
            zd.dwHeight = g_set.h;
            zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
            zd.ddpfPixelFormat.dwSize = sizeof(DDPIXELFORMAT);
            zd.ddpfPixelFormat.dwFlags = DDPF_ZBUFFER;
            zd.ddpfPixelFormat.dwZBufferBitDepth = 16;
            zd.ddpfPixelFormat.dwZBitMask = 0xFFFF;
            IDirectDrawSurface4* z = nullptr;
            if (!Ok("CreateSurface", dd4->CreateSurface(&zd, &z, nullptr))) return false;
            const HRESULT hr = back4->AddAttachedSurface(z);
            z->Release();
            return SUCCEEDED(hr);
        }
        if (!Ok("SetDisplayMode", dd1->SetDisplayMode(g_set.w, g_set.h, g_set.bpp))) return false;
        DDSURFACEDESC d = {};
        d.dwSize = sizeof(d);
        d.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        d.dwBackBufferCount = 1;
        d.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_3DDEVICE;
        if (!Ok("CreateSurface", dd1->CreateSurface(&d, &primary1, nullptr))) return false;
        DDSCAPS caps = {DDSCAPS_BACKBUFFER};
        if (!Ok("GetAttachedSurface", primary1->GetAttachedSurface(&caps, &back1))) return false;
        DDSURFACEDESC zd = {};
        zd.dwSize = sizeof(zd);
        zd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_ZBUFFERBITDEPTH;
        zd.dwWidth = g_set.w;
        zd.dwHeight = g_set.h;
        zd.dwZBufferBitDepth = 16;
        zd.ddsCaps.dwCaps = DDSCAPS_ZBUFFER | DDSCAPS_VIDEOMEMORY;
        IDirectDrawSurface* z = nullptr;
        if (!Ok("CreateSurface", dd1->CreateSurface(&zd, &z, nullptr))) return false;
        const HRESULT hr = back1->AddAttachedSurface(z);
        z->Release();
        return SUCCEEDED(hr);
    }
};

IDirectDrawSurface* Texture1(IDirectDraw* dd, int variant) {
    DDSURFACEDESC d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = 128;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE | DDSCAPS_VIDEOMEMORY;
    Rgb565(&d.ddpfPixelFormat);
    IDirectDrawSurface* t = nullptr;
    if (!Ok("CreateSurface(texture)", dd->CreateSurface(&d, &t, nullptr))) return nullptr;
    DDSURFACEDESC l = {};
    l.dwSize = sizeof(l);
    if (SUCCEEDED(t->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) {
        TextureTexels(static_cast<uint8_t*>(l.lpSurface), l.lPitch, variant);
        t->Unlock(nullptr);
    }
    return t;
}

IDirectDrawSurface4* Texture4(IDirectDraw4* dd, int variant) {
    DDSURFACEDESC2 d = {};
    d.dwSize = sizeof(d);
    d.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT | DDSD_PIXELFORMAT;
    d.dwWidth = d.dwHeight = 128;
    d.ddsCaps.dwCaps = DDSCAPS_TEXTURE;
    d.ddsCaps.dwCaps2 = DDSCAPS2_TEXTUREMANAGE;
    Rgb565(&d.ddpfPixelFormat);
    IDirectDrawSurface4* t = nullptr;
    if (!Ok("CreateSurface(texture)", dd->CreateSurface(&d, &t, nullptr))) return nullptr;
    DDSURFACEDESC2 l = {};
    l.dwSize = sizeof(l);
    if (SUCCEEDED(t->Lock(nullptr, &l, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) {
        TextureTexels(static_cast<uint8_t*>(l.lpSurface), l.lPitch, variant);
        t->Unlock(nullptr);
    }
    return t;
}

// The material every older test uses (handles: Direct3D 2-6).
D3DMATERIAL WhiteMaterial() {
    D3DMATERIAL m = {};
    m.dwSize = sizeof(m);
    m.diffuse = {1.0f, 1.0f, 1.0f, 1.0f};
    m.ambient = {1.0f, 1.0f, 1.0f, 1.0f};
    return m;
}

void OldLights(D3DLIGHT* l) {
    for (int i = 0; i < 2; i++) {
        l[i] = {};
        l[i].dwSize = sizeof(D3DLIGHT);
    }
    LightValues(&l[0].dcvColor, &l[0].dvDirection, &l[1].dcvColor, &l[1].dvPosition);
    l[0].dltType = D3DLIGHT_DIRECTIONAL;
    l[1].dltType = D3DLIGHT_POINT;
    l[1].dvRange = 20.0f;
    l[1].dvAttenuation0 = 1.0f;
}

// ---------------------------------------------------------------- Direct3D 6 (IDirect3DDevice3)

void TestD3D6(int number) {
    const char* name = "d3d6-mesh";
    if (!Wanted(name) || g_quit) return;
    g_why.clear();
    char what[160];
    snprintf(what, sizeof(what), "the %d spheres through IDirect3DDevice3::DrawIndexedPrimitive (DirectX 6)", kSpheres);
    DDOld d;
    IDirect3D3* d3d = nullptr;
    IDirect3DDevice3* dev = nullptr;
    IDirect3DViewport3* vp = nullptr;
    IDirect3DMaterial3* mat = nullptr;
    IDirect3DLight* lights[2] = {};
    IDirectDrawSurface4* surf[8] = {};
    IDirect3DTexture2* tex[8] = {};
    bool ok = d.Open(true) && Ok("QueryInterface(IDirect3D3)", d.dd4->QueryInterface(IID_IDirect3D3, reinterpret_cast<void**>(&d3d))) &&
              Ok("CreateDevice(HAL)", d3d->CreateDevice(IID_IDirect3DHALDevice, d.back4, &dev, nullptr)) &&
              Ok("CreateViewport", d3d->CreateViewport(&vp, nullptr)) && Ok("AddViewport", dev->AddViewport(vp));
    for (int i = 0; ok && i < 8; i++)
        ok = (surf[i] = Texture4(d.dd4, i)) &&
             SUCCEEDED(surf[i]->QueryInterface(IID_IDirect3DTexture2, reinterpret_cast<void**>(&tex[i])));
    D3DMATERIALHANDLE mh = 0;
    if (ok) {
        D3DVIEWPORT2 v2 = {sizeof(v2), 0, 0, DWORD(g_set.w), DWORD(g_set.h), -1.0f, 1.0f, 2.0f, 2.0f, 0.0f, 1.0f};
        vp->SetViewport2(&v2);
        dev->SetCurrentViewport(vp);
        D3DLIGHT l[2];
        OldLights(l);
        for (int i = 0; ok && i < 2; i++) {
            ok = SUCCEEDED(d3d->CreateLight(&lights[i], nullptr));
            if (ok) lights[i]->SetLight(&l[i]), vp->AddLight(lights[i]);
        }
        ok = ok && SUCCEEDED(d3d->CreateMaterial(&mat, nullptr));
        if (ok) {
            D3DMATERIAL m = WhiteMaterial();
            mat->SetMaterial(&m);
            mat->GetHandle(dev, &mh);
        }
    }
    if (!ok) {
        Out("%02d %-12s FAILED to set up Direct3D 6 (%s)   %s", number, name, g_why.empty() ? "a step without an error code" : g_why.c_str(), what);
    } else {
        Mesh mesh;
        const DWORD nv = DWORD(mesh.v.size()), ni = DWORD(mesh.idx.size());
        D3DMATRIX view = Identity(), proj = Perspective(1.0f, float(g_set.w) / float(g_set.h), 0.5f, 50.0f);
        dev->SetTransform(D3DTRANSFORMSTATE_VIEW, &view);
        dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &proj);
        Loop lp;
        lp.frame = [&](int, double t) {
            D3DRECT r = {0, 0, g_set.w, g_set.h};
            Ck("Clear2", vp->Clear2(1, &r, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF302010, 1.0f, 0));
            Ck("BeginScene", dev->BeginScene());
            dev->SetLightState(D3DLIGHTSTATE_MATERIAL, mh);
            dev->SetLightState(D3DLIGHTSTATE_AMBIENT, 0x00303030);
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
            dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_CCW);
            dev->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTFG_LINEAR);
            dev->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTFN_LINEAR);
            for (int i = 0; i < kSpheres; i++) {
                D3DMATRIX w = SphereWorld(i, t);
                dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &w);
                dev->SetTexture(0, tex[i & 7]);
                Ck("DrawIndexedPrimitive",
                   dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DFVF_VERTEX, mesh.v.data(), nv, mesh.idx.data(), ni, 0));
            }
            return true;
        };
        lp.counter = [&](const std::vector<Rc>& r) {
            std::vector<D3DTLVERTEX> v = CounterTris(r);
            dev->SetTexture(0, nullptr);
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
            dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
            Ck("DrawPrimitive (counter)", dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DFVF_TLVERTEX, v.data(), DWORD(v.size()), 0));
            Ck("EndScene", dev->EndScene());
        };
        lp.flip = [&] { return FlipChain(d.primary4); };
        Run(number, name, what, lp);
    }
    for (auto* t : tex)
        if (t) t->Release();
    for (auto* s : surf)
        if (s) s->Release();
    for (auto* l : lights)
        if (l) l->Release();
    if (mat) mat->Release();
    if (vp) vp->Release();
    if (dev) dev->Release();
    if (d3d) d3d->Release();
}

// ---------------------------------------------------------------- Direct3D 5 (IDirect3DDevice2)

void TestD3D5(int number) {
    const char* name = "d3d5-mesh";
    if (!Wanted(name) || g_quit) return;
    g_why.clear();
    char what[160];
    snprintf(what, sizeof(what), "the %d spheres through IDirect3DDevice2::DrawIndexedPrimitive, texture handles (DirectX 5)",
             kSpheres);
    DDOld d;
    IDirect3D2* d3d = nullptr;
    IDirect3DDevice2* dev = nullptr;
    IDirect3DViewport2* vp = nullptr;
    IDirect3DMaterial2* mat = nullptr;
    IDirect3DLight* lights[2] = {};
    IDirectDrawSurface* surf[8] = {};
    IDirect3DTexture2* tex[8] = {};
    D3DTEXTUREHANDLE th[8] = {};
    bool ok = d.Open(false) && Ok("QueryInterface(IDirect3D2)", d.dd1->QueryInterface(IID_IDirect3D2, reinterpret_cast<void**>(&d3d))) &&
              Ok("CreateDevice(HAL)", d3d->CreateDevice(IID_IDirect3DHALDevice, d.back1, &dev)) &&
              Ok("CreateViewport", d3d->CreateViewport(&vp, nullptr)) && Ok("AddViewport", dev->AddViewport(vp));
    for (int i = 0; ok && i < 8; i++)
        ok = (surf[i] = Texture1(d.dd1, i)) &&
             SUCCEEDED(surf[i]->QueryInterface(IID_IDirect3DTexture2, reinterpret_cast<void**>(&tex[i]))) &&
             SUCCEEDED(tex[i]->GetHandle(dev, &th[i]));
    D3DMATERIALHANDLE mh = 0;
    if (ok) {
        D3DVIEWPORT2 v2 = {sizeof(v2), 0, 0, DWORD(g_set.w), DWORD(g_set.h), -1.0f, 1.0f, 2.0f, 2.0f, 0.0f, 1.0f};
        vp->SetViewport2(&v2);
        dev->SetCurrentViewport(vp);
        D3DLIGHT l[2];
        OldLights(l);
        for (int i = 0; ok && i < 2; i++) {
            ok = SUCCEEDED(d3d->CreateLight(&lights[i], nullptr));
            if (ok) lights[i]->SetLight(&l[i]), vp->AddLight(lights[i]);
        }
        ok = ok && SUCCEEDED(d3d->CreateMaterial(&mat, nullptr));
        if (ok) {
            D3DMATERIAL m = WhiteMaterial();
            mat->SetMaterial(&m);
            mat->GetHandle(dev, &mh);
        }
    }
    if (!ok) {
        Out("%02d %-12s FAILED to set up Direct3D 5 (%s)   %s", number, name, g_why.empty() ? "a step without an error code" : g_why.c_str(), what);
    } else {
        Mesh mesh;
        const DWORD nv = DWORD(mesh.v.size()), ni = DWORD(mesh.idx.size());
        D3DMATRIX view = Identity(), proj = Perspective(1.0f, float(g_set.w) / float(g_set.h), 0.5f, 50.0f);
        dev->SetTransform(D3DTRANSFORMSTATE_VIEW, &view);
        dev->SetTransform(D3DTRANSFORMSTATE_PROJECTION, &proj);
        Loop lp;
        lp.frame = [&](int, double t) {
            D3DRECT r = {0, 0, g_set.w, g_set.h};
            Ck("Clear", vp->Clear(1, &r, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER));
            Ck("BeginScene", dev->BeginScene());
            dev->SetLightState(D3DLIGHTSTATE_MATERIAL, mh);
            dev->SetLightState(D3DLIGHTSTATE_AMBIENT, 0x00303030);
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, TRUE);
            dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_CCW);
            dev->SetRenderState(D3DRENDERSTATE_TEXTUREMAG, D3DFILTER_LINEAR);
            dev->SetRenderState(D3DRENDERSTATE_TEXTUREMIN, D3DFILTER_LINEAR);
            for (int i = 0; i < kSpheres; i++) {
                D3DMATRIX w = SphereWorld(i, t);
                dev->SetTransform(D3DTRANSFORMSTATE_WORLD, &w);
                dev->SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, th[i & 7]);
                Ck("DrawIndexedPrimitive",
                   dev->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, D3DVT_VERTEX, mesh.v.data(), nv, mesh.idx.data(), ni, 0));
            }
            return true;
        };
        lp.counter = [&](const std::vector<Rc>& r) {
            std::vector<D3DTLVERTEX> v = CounterTris(r);
            dev->SetRenderState(D3DRENDERSTATE_TEXTUREHANDLE, 0);
            dev->SetRenderState(D3DRENDERSTATE_ZENABLE, FALSE);
            dev->SetRenderState(D3DRENDERSTATE_CULLMODE, D3DCULL_NONE);
            Ck("DrawPrimitive (counter)", dev->DrawPrimitive(D3DPT_TRIANGLELIST, D3DVT_TLVERTEX, v.data(), DWORD(v.size()), 0));
            Ck("EndScene", dev->EndScene());
        };
        lp.flip = [&] { return FlipChain(d.primary1); };
        Run(number, name, what, lp);
    }
    for (auto* t : tex)
        if (t) t->Release();
    for (auto* s : surf)
        if (s) s->Release();
    for (auto* l : lights)
        if (l) l->Release();
    if (mat) mat->Release();
    if (vp) vp->Release();
    if (dev) dev->Release();
    if (d3d) d3d->Release();
}

// ---------------------------------------------------------------- Direct3D 1-3 (execute buffers)

uint8_t* Ins(uint8_t* at, BYTE op, BYTE size, WORD count) {
    D3DINSTRUCTION* i = reinterpret_cast<D3DINSTRUCTION*>(at);
    i->bOpcode = op;
    i->bSize = size;
    i->wCount = count;
    return at + sizeof(D3DINSTRUCTION);
}

// One execute buffer: `nv` vertices (copied as given, of D3DVERTEX or D3DTLVERTEX size),
// then the instructions `build` writes. Recreated when too small.
struct ExecBuf {
    IDirect3DExecuteBuffer* eb = nullptr;
    DWORD size = 0;
    ~ExecBuf() {
        if (eb) eb->Release();
    }
    bool Fill(IDirect3DDevice* dev, const void* verts, DWORD nv, DWORD vertBytes, DWORD insBytes,
              const std::function<uint8_t*(uint8_t*)>& build) {
        const DWORD need = nv * vertBytes + insBytes + 64;
        if (!eb || size < need) {
            if (eb) eb->Release(), eb = nullptr;
            D3DEXECUTEBUFFERDESC ed = {sizeof(ed), D3DDEB_BUFSIZE, 0, need, nullptr};
            if (!Ok("CreateExecuteBuffer", dev->CreateExecuteBuffer(&ed, &eb, nullptr))) return false;
            size = need;
        }
        D3DEXECUTEBUFFERDESC ed = {};
        ed.dwSize = sizeof(ed);
        if (!Ok("Lock", eb->Lock(&ed))) return false;
        uint8_t* base = static_cast<uint8_t*>(ed.lpData);
        memcpy(base, verts, nv * vertBytes);
        uint8_t* ins = base + nv * vertBytes;
        uint8_t* end = build(ins);
        end = Ins(end, D3DOP_EXIT, 0, 0);
        eb->Unlock();
        D3DEXECUTEDATA ex = {};
        ex.dwSize = sizeof(ex);
        ex.dwVertexCount = nv;
        ex.dwInstructionOffset = DWORD(ins - base);
        ex.dwInstructionLength = DWORD(end - ins);
        return SUCCEEDED(eb->SetExecuteData(&ex));
    }
};

uint8_t* States(uint8_t* at, BYTE op, std::initializer_list<std::pair<DWORD, DWORD>> s) {
    at = Ins(at, op, sizeof(D3DSTATE), WORD(s.size()));
    for (const auto& p : s) {
        D3DSTATE* st = reinterpret_cast<D3DSTATE*>(at);
        st->drstRenderStateType = D3DRENDERSTATETYPE(p.first);
        st->dwArg[0] = p.second;
        at += sizeof(D3DSTATE);
    }
    return at;
}

uint8_t* Triangles(uint8_t* at, const WORD* idx, DWORD tris) {
    while (tris) {
        // D3DTRIANGLE records start QWORD-aligned, as the SDK's samples align them: an empty
        // TRIANGLE instruction first when the records would start on a 4-byte boundary.
        if (!(reinterpret_cast<uintptr_t>(at) & 7)) at = Ins(at, D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), 0);
        const DWORD n = std::min<DWORD>(tris, 0xFFFF);
        at = Ins(at, D3DOP_TRIANGLE, sizeof(D3DTRIANGLE), WORD(n));
        for (DWORD t = 0; t < n; t++) {
            D3DTRIANGLE* r = reinterpret_cast<D3DTRIANGLE*>(at);
            r->v1 = idx[3 * t], r->v2 = idx[3 * t + 1], r->v3 = idx[3 * t + 2];
            r->wFlags = D3DTRIFLAG_EDGEENABLETRIANGLE;
            at += sizeof(D3DTRIANGLE);
        }
        idx += 3 * n;
        tris -= n;
    }
    return at;
}

void TestExecute(int number) {
    const char* name = "d3d3-exec";
    if (!Wanted(name) || g_quit) return;
    g_why.clear();
    char what[160];
    snprintf(what, sizeof(what), "the %d spheres through execute buffers, one Execute each (DirectX 1-3)", kSpheres);
    DDOld d;
    IDirect3D* d3d = nullptr;
    IDirect3DDevice* dev = nullptr;
    IDirect3DViewport* vp = nullptr;
    IDirect3DMaterial* mat = nullptr;
    IDirect3DLight* lights[2] = {};
    IDirectDrawSurface* surf[8] = {};
    IDirect3DTexture* tex[8] = {};
    D3DTEXTUREHANDLE th[8] = {};
    D3DMATRIXHANDLE world[kSpheres] = {}, hv = 0, hp = 0;
    bool ok = d.Open(false) && Ok("QueryInterface(IDirect3D)", d.dd1->QueryInterface(IID_IDirect3D, reinterpret_cast<void**>(&d3d))) &&
              Ok("QueryInterface(IDirect3DHALDevice)", d.back1->QueryInterface(IID_IDirect3DHALDevice, reinterpret_cast<void**>(&dev))) &&
              Ok("CreateViewport", d3d->CreateViewport(&vp, nullptr)) && Ok("AddViewport", dev->AddViewport(vp));
    for (int i = 0; ok && i < 8; i++)
        ok = (surf[i] = Texture1(d.dd1, i)) &&
             SUCCEEDED(surf[i]->QueryInterface(IID_IDirect3DTexture, reinterpret_cast<void**>(&tex[i]))) &&
             SUCCEEDED(tex[i]->GetHandle(dev, &th[i]));
    D3DMATERIALHANDLE mh = 0;
    if (ok) {
        D3DVIEWPORT v1 = {};
        v1.dwSize = sizeof(v1);
        v1.dwWidth = g_set.w;
        v1.dwHeight = g_set.h;
        v1.dvScaleX = g_set.w / 2.0f;
        v1.dvScaleY = g_set.h / 2.0f;
        v1.dvMaxX = 1.0f;
        v1.dvMaxY = 1.0f;
        v1.dvMaxZ = 1.0f;
        vp->SetViewport(&v1);
        D3DLIGHT l[2];
        OldLights(l);
        for (int i = 0; ok && i < 2; i++) {
            ok = SUCCEEDED(d3d->CreateLight(&lights[i], nullptr));
            if (ok) lights[i]->SetLight(&l[i]), vp->AddLight(lights[i]);
        }
        ok = ok && SUCCEEDED(d3d->CreateMaterial(&mat, nullptr));
        if (ok) {
            D3DMATERIAL m = WhiteMaterial();
            mat->SetMaterial(&m);
            mat->GetHandle(dev, &mh);
        }
        for (int i = 0; ok && i < kSpheres; i++) ok = SUCCEEDED(dev->CreateMatrix(&world[i]));
        ok = ok && SUCCEEDED(dev->CreateMatrix(&hv)) && SUCCEEDED(dev->CreateMatrix(&hp));
    }
    if (!ok) {
        Out("%02d %-12s FAILED to set up Direct3D 1-3 (%s)   %s", number, name, g_why.empty() ? "a step without an error code" : g_why.c_str(), what);
    } else {
        Mesh mesh;
        const DWORD nv = DWORD(mesh.v.size()), tris = DWORD(mesh.idx.size() / 3);
        D3DMATRIX view = Identity(), proj = Perspective(1.0f, float(g_set.w) / float(g_set.h), 0.5f, 50.0f);
        dev->SetMatrix(hv, &view);
        dev->SetMatrix(hp, &proj);
        // One execute buffer per sphere, built once (the world matrix changes through its
        // handle every frame, as games did); the counter's own buffer rebuilt each frame.
        std::vector<ExecBuf> bufs(kSpheres);
        for (int i = 0; ok && i < kSpheres; i++) {
            ok = bufs[i].Fill(dev, mesh.v.data(), nv, sizeof(D3DVERTEX), 512 + tris * sizeof(D3DTRIANGLE), [&](uint8_t* at) {
                at = Ins(at, D3DOP_STATETRANSFORM, sizeof(D3DSTATE), 3);
                D3DSTATE* st = reinterpret_cast<D3DSTATE*>(at);
                st[0].dtstTransformStateType = D3DTRANSFORMSTATE_WORLD, st[0].dwArg[0] = world[i];
                st[1].dtstTransformStateType = D3DTRANSFORMSTATE_VIEW, st[1].dwArg[0] = hv;
                st[2].dtstTransformStateType = D3DTRANSFORMSTATE_PROJECTION, st[2].dwArg[0] = hp;
                at += 3 * sizeof(D3DSTATE);
                at = States(at, D3DOP_STATELIGHT, {{D3DLIGHTSTATE_MATERIAL, mh}, {D3DLIGHTSTATE_AMBIENT, 0x00303030}});
                at = States(at, D3DOP_STATERENDER,
                            {{D3DRENDERSTATE_TEXTUREHANDLE, th[i & 7]},
                             {D3DRENDERSTATE_ZENABLE, TRUE},
                             {D3DRENDERSTATE_CULLMODE, D3DCULL_CCW},
                             {D3DRENDERSTATE_TEXTUREMAG, D3DFILTER_LINEAR},
                             {D3DRENDERSTATE_TEXTUREMIN, D3DFILTER_LINEAR}});
                at = Ins(at, D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
                D3DPROCESSVERTICES* pv = reinterpret_cast<D3DPROCESSVERTICES*>(at);
                *pv = {D3DPROCESSVERTICES_TRANSFORMLIGHT, 0, 0, nv, 0};
                at += sizeof(D3DPROCESSVERTICES);
                return Triangles(at, mesh.idx.data(), tris);
            });
        }
        ExecBuf counter;
        if (!ok) {
            Out("%02d %-12s FAILED to build the execute buffers   %s", number, name, what);
        } else {
            Loop lp;
            lp.frame = [&](int, double t) {
                D3DRECT r = {0, 0, g_set.w, g_set.h};
                Ck("Clear", vp->Clear(1, &r, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER));
                Ck("BeginScene", dev->BeginScene());
                for (int i = 0; i < kSpheres; i++) {
                    D3DMATRIX w = SphereWorld(i, t);
                    dev->SetMatrix(world[i], &w);
                    Ck("Execute", dev->Execute(bufs[i].eb, vp, D3DEXECUTE_CLIPPED));
                }
                return true;
            };
            lp.counter = [&](const std::vector<Rc>& r) {
                std::vector<D3DTLVERTEX> v = CounterTris(r);
                std::vector<WORD> idx(v.size());
                for (size_t i = 0; i < idx.size(); i++) idx[i] = WORD(i);
                if (counter.Fill(dev, v.data(), DWORD(v.size()), sizeof(D3DTLVERTEX),
                                 256 + DWORD(v.size() / 3) * sizeof(D3DTRIANGLE), [&](uint8_t* at) {
                                     at = States(at, D3DOP_STATERENDER,
                                                 {{D3DRENDERSTATE_TEXTUREHANDLE, 0},
                                                  {D3DRENDERSTATE_ZENABLE, FALSE},
                                                  {D3DRENDERSTATE_CULLMODE, D3DCULL_NONE}});
                                     at = Ins(at, D3DOP_PROCESSVERTICES, sizeof(D3DPROCESSVERTICES), 1);
                                     D3DPROCESSVERTICES* pv = reinterpret_cast<D3DPROCESSVERTICES*>(at);
                                     *pv = {D3DPROCESSVERTICES_COPY, 0, 0, DWORD(v.size()), 0};
                                     at += sizeof(D3DPROCESSVERTICES);
                                     return Triangles(at, idx.data(), DWORD(v.size() / 3));
                                 }))
                    Ck("Execute (counter)", dev->Execute(counter.eb, vp, D3DEXECUTE_UNCLIPPED));
                else
                    Ck("the counter's execute buffer", E_FAIL);
                Ck("EndScene", dev->EndScene());
            };
            lp.flip = [&] { return FlipChain(d.primary1); };
            Run(number, name, what, lp);
        }
        bufs.clear();
    }
    for (D3DMATRIXHANDLE h : world)
        if (h) dev->DeleteMatrix(h);
    if (hv) dev->DeleteMatrix(hv);
    if (hp) dev->DeleteMatrix(hp);
    for (auto* t : tex)
        if (t) t->Release();
    for (auto* s : surf)
        if (s) s->Release();
    for (auto* l : lights)
        if (l) l->Release();
    if (mat) mat->Release();
    if (vp) vp->Release();
    if (dev) dev->Release();
    if (d3d) d3d->Release();
}

// ---------------------------------------------------------------- the ddraw.dll in use

std::string DdrawIdentity() {
    HMODULE m = GetModuleHandleA("ddraw.dll");
    if (!m) return "ddraw.dll not loaded";
    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, MAX_PATH);
    std::string s = path;
    // The loaded module's own version resource and image size: the file at that path may be
    // another (Wine loads its built-in ddraw.dll over a native one in system32 when told to).
    std::string version = "  (no version resource)";
    if (HRSRC r = FindResourceA(m, MAKEINTRESOURCEA(1), MAKEINTRESOURCEA(16))) {  // VS_VERSION_INFO, RT_VERSION
        const DWORD size = SizeofResource(m, r);
        const void* data = LockResource(LoadResource(m, r));
        if (data && size) {
            std::vector<uint8_t> info(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
            info.resize(size_t(size) * 3 + 16);  // (VerQueryValue may convert strings past the resource)
            VS_FIXEDFILEINFO* fi = nullptr;
            UINT len = 0;
            if (VerQueryValueA(info.data(), "\\", reinterpret_cast<void**>(&fi), &len) && fi) {
                char v[64];
                snprintf(v, sizeof(v), "  version %u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                         HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
                version = v;
            }
            struct Translation {
                WORD lang, cp;
            }* tr = nullptr;
            if (VerQueryValueA(info.data(), "\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &len) && tr &&
                len >= sizeof(*tr)) {
                for (const char* n : {"ProductName", "FileDescription"}) {
                    char key[96];
                    snprintf(key, sizeof(key), "\\StringFileInfo\\%04x%04x\\%s", tr->lang, tr->cp, n);
                    char* val = nullptr;
                    if (VerQueryValueA(info.data(), key, reinterpret_cast<void**>(&val), &len) && val && len > 1) {
                        version += "  ";
                        version += val;
                        break;
                    }
                }
            }
        }
    }
    s += version;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
    char b[64];
    snprintf(b, sizeof(b), "  image %lu bytes", static_cast<unsigned long>(nt->OptionalHeader.SizeOfImage));
    s += b;
    return s;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SETCURSOR) {
        SetCursor(nullptr);
        return TRUE;
    }
    return DefWindowProcA(h, m, w, l);
}

void ParseArgs(const char* cmd) {
    std::vector<std::string> a;
    std::string cur;
    for (const char* p = cmd; *p; p++) {
        if (*p == ' ' || *p == '\t') {
            if (!cur.empty()) a.push_back(cur), cur.clear();
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) a.push_back(cur);
    for (size_t i = 0; i < a.size(); i++) {
        const bool more = i + 1 < a.size();
        if (a[i] == "-s" && more) g_set.seconds = std::max(1.0, atof(a[++i].c_str()));
        else if (a[i] == "-w" && more) g_set.w = atoi(a[++i].c_str());
        else if (a[i] == "-h" && more) g_set.h = atoi(a[++i].c_str());
        else if (a[i] == "-bpp" && more) g_set.bpp = atoi(a[++i].c_str()) == 32 ? 32 : 16;
        else if (a[i] == "-vsync") g_set.vsync = true;
        else if (a[i] == "-only" && more) {
            std::string list = a[++i];
            size_t at = 0;
            while (at <= list.size()) {
                const size_t comma = list.find(',', at);
                const std::string one = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (!one.empty()) g_set.only.push_back(one);
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }
    }
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmd, int) {
    ParseArgs(cmd);
    char dir[MAX_PATH];
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    if (char* slash = strrchr(dir, '\\')) slash[1] = 0;
    const std::string outPath = std::string(dir) + "wrapbench.txt";
    g_out = fopen(outPath.c_str(), "a");

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = "wrapbench";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowExA(WS_EX_TOPMOST, "wrapbench", "wrapbench", WS_POPUP | WS_VISIBLE, 0, 0, g_set.w, g_set.h,
                             nullptr, nullptr, inst, nullptr);
    Pump();

    // (ddraw.dll is loaded at start-up: it is imported.)
    SYSTEMTIME st;
    GetLocalTime(&st);
    Out("== wrapbench %s  %04u-%02u-%02u %02u:%02u  %dx%dx%d, %.0f s a test, vsync %s", kVersion, st.wYear, st.wMonth,
        st.wDay, st.wHour, st.wMinute, g_set.w, g_set.h, g_set.bpp, g_set.seconds, g_set.vsync ? "on (asked)" : "off (asked)");
    Out("   ddraw.dll: %s", DdrawIdentity().c_str());
    g_tick = ClockTick();
    if (g_tick > 0.00005)
        Out("   the clock ticks every %.2f ms: the median and 1%% low are taken over runs of frames, not single ones",
            g_tick * 1000.0);

    Tests2D();
    TestsD3D7();
    TestD3D6(11);
    TestD3D5(12);
    TestExecute(13);
    Out("");

    ShowWindow(g_hwnd, SW_HIDE);
    if (g_out) fclose(g_out);
    const std::string msg = g_summary + "\n(appended to " + outPath + ")";
    MessageBoxA(nullptr, msg.c_str(), "wrapbench results", MB_OK | MB_SETFOREGROUND);
    DestroyWindow(g_hwnd);
    return 0;
}
