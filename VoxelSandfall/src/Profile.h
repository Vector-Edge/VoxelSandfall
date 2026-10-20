// Tiny frame profiler: scoped CPU timers + GPU timestamp slots, smoothed for the HUD (F4) and the script `prof` command.
#pragma once
#include <chrono>

namespace prof {

enum Slot {
    P_STREAM, P_PLAYER, P_TOOLS, P_PHYS, P_OVERLAY, P_SIM, P_PARTICLES, P_FIRE,   // Game::update
    P_MESH, P_UPLOAD, P_BODIES, P_DRAW, P_PRESENT,             // Renderer::render (CPU side)
    P_COUNT
};
enum GpuSlot { G_SHADOW, G_MAIN, G_WATER, G_POST, G_TOTAL, G_COUNT };

inline const char* kNames[P_COUNT] = {"chunk loading", "player", "tools(paint/fire)", "rigid bodies", "body overlay", "cell sim", "particles", "fire & smoke",
                                      "chunk meshing", "gpu uploads", "body meshes", "draw submit", "present"};
inline const char* kGpuNames[G_COUNT] = {"shadow map", "sky/opaque", "water+brush+gun", "resolve+hud", "GPU total"};

inline double g_cur[P_COUNT] = {};       // this frame
inline double g_avg[P_COUNT] = {};       // exponential moving average
inline double g_max[P_COUNT] = {};       // worst frame since reset
inline double g_gpuAvg[G_COUNT] = {};
inline double g_gpuMax[G_COUNT] = {};
inline int g_frames = 0;

inline double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

struct Scope {
    Slot slot;
    double t0;
    explicit Scope(Slot s) : slot(s), t0(nowMs()) {}
    ~Scope() { g_cur[slot] += nowMs() - t0; }
};

inline void endFrame(double smoothing = 0.1) {
    for (int i = 0; i < P_COUNT; i++) {
        g_avg[i] += (g_cur[i] - g_avg[i]) * smoothing;
        if (g_cur[i] > g_max[i]) g_max[i] = g_cur[i];
        g_cur[i] = 0;
    }
    g_frames++;
}

inline void setGpu(GpuSlot s, double ms) {
    g_gpuAvg[s] += (ms - g_gpuAvg[s]) * 0.1;
    if (ms > g_gpuMax[s]) g_gpuMax[s] = ms;
}

inline void reset() {
    for (int i = 0; i < P_COUNT; i++) g_avg[i] = g_max[i] = g_cur[i] = 0;
    for (int i = 0; i < G_COUNT; i++) g_gpuAvg[i] = g_gpuMax[i] = 0;
    g_frames = 0;
}

}  // namespace prof
