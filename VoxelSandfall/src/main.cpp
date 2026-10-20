// Voxel Sandfall 0.0.1 - prototype voxel engine. Win32 window, input, main loop, scripted test driver.
#include <windows.h>
#include <shellapi.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Game.h"
#include "Log.h"
#include "Png.h"
#include "Profile.h"
#include "Renderer.h"

int runSelfTest(const char* logPath);

// ------------------------------------------------------------------------------------------------
// Logging
// ------------------------------------------------------------------------------------------------
static std::string g_exeDir;

static std::string exeDirectory() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    return dir.substr(0, dir.find_last_of("\\/"));
}

void logLine(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (g_exeDir.empty()) g_exeDir = exeDirectory();
    std::string path = g_exeDir + "\\voxelsandfall.log";
    if (FILE* f = fopen(path.c_str(), "a")) {
        fprintf(f, "%s\n", buf);
        fclose(f);
    }
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

// ------------------------------------------------------------------------------------------------
// Application state
// ------------------------------------------------------------------------------------------------
namespace {

HWND g_hwnd = nullptr;
Game* g_game = nullptr;
Renderer* g_renderer = nullptr;
bool g_running = true;
bool g_captured = false;
bool g_skipMouse = true;
bool g_fullscreen = false;
bool g_scriptMode = false;
WINDOWPLACEMENT g_savedPlacement = {sizeof(WINDOWPLACEMENT)};
LONG g_savedStyle = 0;

bool g_keys[256] = {};
bool g_prevKeys[256] = {};
bool g_lmb = false, g_prevLmb = false;
int g_wheel = 0;

POINT clientCenterScreen() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    POINT p = {(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2};
    ClientToScreen(g_hwnd, &p);
    return p;
}

void setMouseCapture(bool on) {
    if (on == g_captured || g_scriptMode) return;
    g_captured = on;
    if (on) {
        RECT rc;
        GetClientRect(g_hwnd, &rc);
        POINT a = {rc.left, rc.top}, b = {rc.right, rc.bottom};
        ClientToScreen(g_hwnd, &a);
        ClientToScreen(g_hwnd, &b);
        RECT clip = {a.x, a.y, b.x, b.y};
        ClipCursor(&clip);
        while (ShowCursor(FALSE) >= 0) {}
        POINT c = clientCenterScreen();
        SetCursorPos(c.x, c.y);
        g_skipMouse = true;
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
    }
}

void toggleFullscreen() {
    if (!g_fullscreen) {
        g_savedStyle = GetWindowLong(g_hwnd, GWL_STYLE);
        GetWindowPlacement(g_hwnd, &g_savedPlacement);
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfo(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLong(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLong(g_hwnd, GWL_STYLE, g_savedStyle);
        SetWindowPlacement(g_hwnd, &g_savedPlacement);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
    if (g_captured) {
        g_captured = false;
        setMouseCapture(true);
    }
}

bool pressed(int vk) { return g_keys[vk] && !g_prevKeys[vk]; }

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CLOSE:
            g_running = false;
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            if (g_renderer && wp != SIZE_MINIMIZED) {
                g_renderer->resize((int)LOWORD(lp), (int)HIWORD(lp));
                if (g_captured) {
                    g_captured = false;
                    setMouseCapture(true);
                }
            }
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp < 256) g_keys[wp] = true;
            if (msg == WM_SYSKEYDOWN && wp == VK_F10) return 0;
            if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && (lp & (1 << 29))) { toggleFullscreen(); return 0; }
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wp < 256) g_keys[wp] = false;
            break;
        case WM_LBUTTONDOWN:
            if (!g_captured && !g_scriptMode) {
                setMouseCapture(true);  // the first click only re-captures the mouse
            } else {
                g_lmb = true;
            }
            return 0;
        case WM_LBUTTONUP:
            g_lmb = false;
            return 0;
        case WM_MOUSEWHEEL:
            g_wheel += GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
            return 0;
        case WM_KILLFOCUS:
            setMouseCapture(false);
            g_lmb = false;
            for (bool& k : g_keys) k = false;
            return 0;
        case WM_SETCURSOR:
            if (g_captured) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

HWND createMainWindow(HINSTANCE inst, int clientW, int clientH, bool show) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VoxelSandfallWnd";
    RegisterClassExW(&wc);
    RECT rc = {0, 0, clientW, clientH};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"Voxel Sandfall 0.0.1 - prototype engine", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, inst, nullptr);
    if (h && show) {
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
    }
    return h;
}

void pumpMessages() {
    MSG m;
    while (PeekMessage(&m, nullptr, 0, 0, PM_REMOVE)) {
        if (m.message == WM_QUIT) g_running = false;
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
}

std::string timestamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char b[64];
    snprintf(b, sizeof(b), "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

// ------------------------------------------------------------------------------------------------
// Scripted driver (used to verify the renderer/gameplay without a human at the controls)
// ------------------------------------------------------------------------------------------------
bool runScript(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        logLine("cannot open script %s", path.c_str());
        return false;
    }
    Game& game = *g_game;
    Renderer& r = *g_renderer;
    r.vsync = false;
    r.showHelp = false;
    std::string line;
    int lineNo = 0;
    auto wallMs = []() {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
    };
    auto stepFrames = [&](double seconds) {
        int n = std::max(1, (int)(seconds * 60.0 + 0.5));
        GameInput gi;
        double tUpdate = 0, tRender = 0, worstUpdate = 0;
        for (int i = 0; i < n && g_running; i++) {
            double a = wallMs();
            game.update(1.0f / 60.0f, gi);
            double b = wallMs();
            r.speedForBob = 0;
            r.render(game, 1.0f / 60.0f);
            double c = wallMs();
            tUpdate += b - a;
            tRender += c - b;
            worstUpdate = std::max(worstUpdate, b - a);
            if ((i & 15) == 0) pumpMessages();
        }
        logLine("   wait %.2fs: %d frames, avg update %.2f ms (worst %.2f), avg render %.2f ms", seconds, n, tUpdate / n, worstUpdate, tRender / n);
    };
    while (std::getline(in, line) && g_running) {
        lineNo++;
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::istringstream ss(line);
        std::string cmd;
        if (!(ss >> cmd)) continue;
        if (cmd == "wait") {
            double s = 1;
            ss >> s;
            stepFrames(s);
        } else if (cmd == "cam") {  // cam x y z yawDeg pitchDeg  (feet position)
            float x, y, z, yaw = 0, pitch = 0;
            ss >> x >> y >> z >> yaw >> pitch;
            game.player.pos = Vec3(x, y, z);
            game.player.yaw = deg2rad(yaw);
            game.player.pitch = deg2rad(pitch);
            game.player.vel = Vec3(0, 0, 0);
        } else if (cmd == "viewcam") {  // viewcam x y z yawDeg pitchDeg | viewcam off   (does not move the player)
            std::string first;
            ss >> first;
            if (first == "off") {
                r.camOverride = false;
            } else {
                float yw = 0, pt = 0;
                ss >> yw;  // first token was x; re-parse
                float x = (float)atof(first.c_str()), y = yw, z = 0;
                ss >> z >> yw >> pt;
                r.camOverride = true;
                r.camOverridePos = Vec3(x, y, z);
                r.camOverrideYaw = deg2rad(yw);
                r.camOverridePitch = deg2rad(pt);
            }
        } else if (cmd == "infinite") {  // infinite on|off   (resets the world)
            std::string v;
            ss >> v;
            game.setInfinite(v != "off");
        } else if (cmd == "tp") {  // tp globalX globalZ   (infinite world: walk there instantly; the window recentres on the next update)
            long long gx = 0, gz = 0;
            ss >> gx >> gz;
            game.player.pos.x = (float)(gx - game.world.originX);
            game.player.pos.z = (float)(gz - game.world.originZ);
            game.player.vel = Vec3(0, 0, 0);
            game.update(1.0f / 120.0f, GameInput());
        } else if (cmd == "look") {  // look yawDeg pitchDeg
            float yaw = 0, pitch = 0;
            ss >> yaw >> pitch;
            game.player.yaw = deg2rad(yaw);
            game.player.pitch = deg2rad(pitch);
        } else if (cmd == "fillsand") {  // fillsand x0 x1 y0 y1 z0 z1
            int x0, x1, y0, y1, z0, z1;
            ss >> x0 >> x1 >> y0 >> y1 >> z0 >> z1;
            for (int z = z0; z <= z1; z++)
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) game.world.setRaw(x, y, z, M_SAND);
            game.world.notifyBox(x0, y0, z0, x1, y1, z1);
        } else if (cmd == "fillwater") {  // fillwater x0 x1 y0 y1 z0 z1
            int x0, x1, y0, y1, z0, z1;
            ss >> x0 >> x1 >> y0 >> y1 >> z0 >> z1;
            for (int z = z0; z <= z1; z++)
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) game.world.setRaw(x, y, z, M_WATER);
            game.world.notifyBox(x0, y0, z0, x1, y1, z1);
        } else if (cmd == "dumpfield") {  // dumpfield file.png x0 x1 z0 z1   (shore = red, foam = green, ripple = blue; 4 px per voxel)
            std::string name;
            int x0 = 0, x1 = 0, z0 = 0, z1 = 0;
            ss >> name >> x0 >> x1 >> z0 >> z1;
            const int sc = 4, w = (x1 - x0 + 1) * sc, h = (z1 - z0 + 1) * sc;
            std::vector<uint8_t> img((size_t)w * h * 4, 255);
            auto b8 = [](float v) { return (uint8_t)std::clamp((int)(v * 255.0f), 0, 255); };
            for (int py = 0; py < h; py++)
                for (int px = 0; px < w; px++) {
                    const int x = x0 + px / sc, z = z0 + py / sc;
                    uint8_t* o = &img[((size_t)py * w + px) * 4];
                    o[0] = b8(game.waterField.shoreAt(x, z) * 2.0f);
                    o[1] = b8(game.waterField.foamAt(x, z));
                    o[2] = b8(std::fabs(game.waterField.rippleAt(x, z)) * 4.0f);
                    if (game.waterField.isWater(x, z)) o[3] = 255;
                }
            writePng(name.c_str(), w, h, img.data());
        } else if (cmd == "dropstone") {  // dropstone x y z halfSize   (a free stone cube: a rigid body that falls)
            float px, py, pz, hs;
            ss >> px >> py >> pz >> hs;
            std::vector<IVec3> cells;
            for (int z = (int)(pz - hs); z < (int)(pz + hs); z++)
                for (int y = (int)(py - hs); y < (int)(py + hs); y++)
                    for (int x = (int)(px - hs); x < (int)(px + hs); x++) cells.push_back(IVec3(x, y, z));
            game.physics.createFromWorldCells(cells);
        } else if (cmd == "dropball") {  // dropball x y z radius   (a free stone ball: a rigid body that falls and rolls)
            float px, py, pz, pr;
            ss >> px >> py >> pz >> pr;
            std::vector<IVec3> cells;
            for (int z = (int)(pz - pr) - 1; z <= (int)(pz + pr) + 1; z++)
                for (int y = (int)(py - pr) - 1; y <= (int)(py + pr) + 1; y++)
                    for (int x = (int)(px - pr) - 1; x <= (int)(px + pr) + 1; x++) {
                        const float dx = x + 0.5f - px, dy = y + 0.5f - py, dz = z + 0.5f - pz;
                        if (dx * dx + dy * dy + dz * dz <= pr * pr) cells.push_back(IVec3(x, y, z));
                    }
            if (!cells.empty()) game.physics.createFromWorldCells(cells);
        } else if (cmd == "bodies") {  // bodies   (logs every rigid body and the player)
            logLine("player pos=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f)", game.player.pos.x, game.player.pos.y, game.player.pos.z, game.player.vel.x, game.player.vel.y, game.player.vel.z);
            for (auto& up : game.physics.bodies)
                logLine("  body %d: %d voxels pos=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f) ang=%.2f sandF=(%.0f %.0f) %s", up->id, up->count, up->pos.x, up->pos.y, up->pos.z, up->vel.x, up->vel.y, up->vel.z, length(up->ang), up->sandF.x, up->sandF.z, up->asleep ? "asleep" : "awake");
        } else if (cmd == "fillcoal" || cmd == "filloil") {  // fillcoal|filloil x0 x1 y0 y1 z0 z1   (static, as if it had always been there)
            int x0, x1, y0, y1, z0, z1;
            ss >> x0 >> x1 >> y0 >> y1 >> z0 >> z1;
            const uint8_t m = cmd == "fillcoal" ? M_COAL : M_OIL;
            for (int z = z0; z <= z1; z++)
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) game.world.setRaw(x, y, z, m);
            game.world.notifyBox(x0, y0, z0, x1, y1, z1);
        } else if (cmd == "ignite") {  // ignite x y z   (sets the coal / oil at that cell on fire)
            int x, y, z;
            ss >> x >> y >> z;
            logLine("script %d: ignite %d %d %d -> %s", lineNo, x, y, z, game.igniteAt(x, y, z) ? "caught" : "nothing to burn");
        } else if (cmd == "flames" || cmd == "smoke") {  // flames|smoke x y z radius count
            float px, py, pz, pr;
            int n = 40;
            ss >> px >> py >> pz >> pr >> n;
            if (cmd == "flames") game.spawnFlames(Vec3(px, py, pz), pr, n);
            else game.spawnSmokeCloud(Vec3(px, py, pz), pr, n);
        } else if (cmd == "dropcoal") {  // dropcoal x y z radius   (paints coal in the air: it comes down as pebbles)
            float px, py, pz, pr;
            ss >> px >> py >> pz >> pr;
            const Vec3 nowhere(0, 0, 0), nowhere2(1, 1, 1);
            const PaintResult res = paintSphere(game.world, game.physics, Vec3(px, py, pz), pr, M_COAL, nowhere, nowhere2);
            logLine("script %d: dropcoal placed %d, %d new bodies", lineNo, res.placed, res.newBodies);
        } else if (cmd == "firestat") {  // firestat   (what is burning)
            long long bb = 0;
            for (auto& up : game.physics.bodies) bb += (long long)up->burn.size();
            logLine("script %d: fire: %lld burning coal, %lld burning oil, %lld burning in rocks; %zu flames, %zu smoke, %zu lights; coal %lld oil %lld; %zu bodies", lineNo,
                    game.world.counts[M_BCOAL], game.world.counts[M_BOIL], bb, game.flameCount(), game.smokeCount(), game.fireLights.size(), game.world.counts[M_COAL],
                    game.world.counts[M_OIL], game.physics.bodies.size());
        } else if (cmd == "impacts") {  // impacts on|off   (water splash / soak behaviour, for comparisons)
            std::string v;
            ss >> v;
            game.world.impactsEnabled = v != "off";
            game.world.soakEnabled = v != "nosoak";
        } else if (cmd == "filldirt") {  // filldirt x0 x1 y0 y1 z0 z1   (settled dirt, as if it had always been there)
            int x0, x1, y0, y1, z0, z1;
            ss >> x0 >> x1 >> y0 >> y1 >> z0 >> z1;
            for (int z = z0; z <= z1; z++)
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) game.world.setRaw(x, y, z, M_DIRT);
            game.world.notifyBox(x0, y0, z0, x1, y1, z1);
        } else if (cmd == "growgrass") {  // growgrass x y z r   (grass on the dirt surface inside the sphere)
            float px, py, pz, pr;
            ss >> px >> py >> pz >> pr;
            int n = 0;
            game.world.forEachInSphere(Vec3(px, py, pz), pr, [&](int x, int y, int z) { n += game.world.paintGrass(x, y, z) ? 1 : 0; });
            logLine("growgrass: %d voxels", n);
        } else if (cmd == "fillstone") {  // fillstone x0 x1 y0 y1 z0 z1
            int x0, x1, y0, y1, z0, z1;
            ss >> x0 >> x1 >> y0 >> y1 >> z0 >> z1;
            for (int z = z0; z <= z1; z++)
                for (int y = y0; y <= y1; y++)
                    for (int x = x0; x <= x1; x++) game.world.setRaw(x, y, z, M_STONE);
            game.world.notifyBox(x0, y0, z0, x1, y1, z1);
        } else if (cmd == "dropwater") {  // dropwater x y z radius   (a ball of water appears in the air and falls)
            float px, py, pz, pr;
            ss >> px >> py >> pz >> pr;
            int x0 = 1 << 30, y0 = 1 << 30, z0 = 1 << 30, x1 = -1, y1 = -1, z1 = -1;
            game.world.forEachInSphere(Vec3(px, py, pz), pr, [&](int x, int y, int z) {
                game.world.setRaw(x, y, z, M_WATER);
                x0 = std::min(x0, x); y0 = std::min(y0, y); z0 = std::min(z0, z);
                x1 = std::max(x1, x); y1 = std::max(y1, y); z1 = std::max(z1, z);
            });
            if (x1 >= 0) game.world.notifyBox(x0, y0, z0, x1, y1, z1);
        } else if (cmd == "dropsand") {  // dropsand x y z radius   (a ball of sand appears in the air and falls)
            float px, py, pz, pr;
            ss >> px >> py >> pz >> pr;
            int x0 = 1 << 30, y0 = 1 << 30, z0 = 1 << 30, x1 = -1, y1 = -1, z1 = -1;
            game.world.forEachInSphere(Vec3(px, py, pz), pr, [&](int x, int y, int z) {
                game.world.setRaw(x, y, z, M_SAND);
                x0 = std::min(x0, x); y0 = std::min(y0, y); z0 = std::min(z0, z);
                x1 = std::max(x1, x); y1 = std::max(y1, y); z1 = std::max(z1, z);
            });
            if (x1 >= 0) game.world.notifyBox(x0, y0, z0, x1, y1, z1);
        } else if (cmd == "crater") {  // crater speed  (as if the player landed hard at this spot)
            float sp = 300;
            ss >> sp;
            game.sandCrater(sp);
        } else if (cmd == "walk" || cmd == "run") {  // walk seconds   (forward input; run = sprint)
            double sec = 1;
            ss >> sec;
            GameInput gi;
            gi.forward = 1;
            gi.sprint = (cmd == "run");
            const int n = std::max(1, (int)(sec * 60.0 + 0.5));
            for (int i = 0; i < n && g_running; i++) {
                game.update(1.0f / 60.0f, gi);
                r.speedForBob = std::sqrt(game.player.vel.x * game.player.vel.x + game.player.vel.z * game.player.vel.z);
                r.render(game, 1.0f / 60.0f);
                if ((i & 15) == 0) pumpMessages();
            }
        } else if (cmd == "freeze") {
            std::string v;
            ss >> v;
            game.player.frozen = (v != "off");
        } else if (cmd == "tool") {
            std::string v;
            ss >> v;
            game.tool = (v == "gun") ? TOOL_GUN : TOOL_CREATOR;
        } else if (cmd == "material") {
            std::string v;
            ss >> v;
            static const char* names[kPaintMaterialCount] = {"sand", "water", "stone", "permanent", "dirt", "grass", "coal", "fire", "smoke", "oil"};
            for (int k = 0; k < kPaintMaterialCount; k++)
                if (v == names[k]) game.materialIndex = k;
        } else if (cmd == "brush") {
            int b = 4;
            ss >> b;
            game.brush = std::clamp(b, cfg::BRUSH_MIN, cfg::BRUSH_MAX);
        } else if (cmd == "paint") {
            int n = 1;
            ss >> n;
            for (int i = 0; i < n; i++) {
                const double tp = wallMs();
                PaintResult pr = game.paintAtCrosshair();
                logLine("script %d: paint placed=%d static=%d newBodies=%d merged=%d  (took %.2f ms)", lineNo, pr.placed, pr.staticStone, pr.newBodies,
                        pr.mergedIntoBody, wallMs() - tp);
                game.update(1.0f / 120.0f, GameInput());
            }
        } else if (cmd == "fire") {
            int n = 1;
            ss >> n;
            for (int i = 0; i < n; i++) {
                const double tp = wallMs();
                CarveResult cr = game.fireAtCrosshair();
                logLine("script %d: fire removedWorld=%d removedBody=%d islands=%d islandVoxels=%d  (took %.2f ms)", lineNo, cr.removedWorld, cr.removedBody,
                        cr.islandsCreated, cr.islandVoxels, wallMs() - tp);
                game.update(1.0f / 60.0f, GameInput());
            }
        } else if (cmd == "carve") {  // carve x y z radius  (direct call, no aiming)
            float x, y, z, rad;
            ss >> x >> y >> z >> rad;
            CarveResult cr = carveSphere(game.world, game.physics, Vec3(x, y, z), rad, game.rng);
            logLine("script %d: carve removedWorld=%d removedBody=%d islands=%d", lineNo, cr.removedWorld, cr.removedBody, cr.islandsCreated);
        } else if (cmd == "shot") {
            std::string name;
            ss >> name;
            r.render(game, 1.0f / 60.0f);
            r.requestScreenshot(name);
            r.render(game, 0.0f);
        } else if (cmd == "settime") {  // settime seconds   (pins the animation clock, so two screenshots can be compared)
            float t = 0;
            ss >> t;
            game.time = t;
        } else if (cmd == "sun") {
            float az = 0.62f, el = 0.95f;
            ss >> az >> el;
            r.sunAzimuth = az;
            r.sunElevation = el;
        } else if (cmd == "quality") {  // quality auto|0..4
            std::string v;
            ss >> v;
            r.quality = v == "auto" ? -1 : std::clamp(atoi(v.c_str()), 0, 4);
        } else if (cmd == "rays") {
            ss >> r.skyRays;
        } else if (cmd == "hud") {
            std::string v;
            ss >> v;
            r.showHud = (v != "off");
        } else if (cmd == "help") {
            std::string v;
            ss >> v;
            r.showHelp = (v != "off");
        } else if (cmd == "prof") {
            std::string rest;
            std::getline(ss, rest);
            double cpu = 0;
            for (int i = 0; i < prof::P_COUNT; i++) if (i != prof::P_PRESENT) cpu += prof::g_avg[i];
            logLine("PROFILE%s (avg ms | worst ms), %d frames", rest.c_str(), prof::g_frames);
            for (int i = 0; i < prof::P_COUNT; i++) logLine("   cpu %-18s %7.2f | %7.2f", prof::kNames[i], prof::g_avg[i], prof::g_max[i]);
            logLine("   cpu %-18s %7.2f", "TOTAL (no present)", cpu);
            for (int i = 0; i < prof::G_COUNT; i++) logLine("   gpu %-18s %7.2f | %7.2f", prof::kGpuNames[i], prof::g_gpuAvg[i], prof::g_gpuMax[i]);
            logLine("   sim phases (ms total): parallel %.1f, impacts %.1f (%lld handled), hops %.1f; cell visits %lld, moves %lld", game.world.dbgParMs, game.world.dbgImpactMs, game.world.dbgImpacts, game.world.dbgHopMs, game.world.dbgVisits, game.world.dbgMoves);
            game.world.dbgParMs = game.world.dbgImpactMs = game.world.dbgHopMs = 0;
            game.world.dbgImpacts = game.world.dbgVisits = game.world.dbgMoves = 0;
            logLine("   water field: %d tiles active, %zu particles", game.waterField.activeTiles(), game.particles.size());
            logLine("   world: sand=%lld water=%lld dirt=%lld grass=%lld active=%zu simTicks/frame=%d bodies=%zu", game.world.counts[M_SAND], game.world.counts[M_WATER], game.world.counts[M_DIRT], game.world.counts[M_GRASS], game.world.activeCount(), game.lastSimTicks, game.physics.bodies.size());
        } else if (cmd == "profiler") {
            std::string v;
            ss >> v;
            r.showProfiler = (v != "off");
        } else if (cmd == "profreset") {
            prof::reset();
        } else if (cmd == "reset") {
            game.reset();
        } else if (cmd == "log") {
            std::string rest;
            std::getline(ss, rest);
            logLine("   player: pos=(%.1f %.1f %.1f) vel=(%.1f %.1f %.1f) onGround=%d grains=%d", game.player.pos.x, game.player.pos.y, game.player.pos.z,
                    game.player.vel.x, game.player.vel.y, game.player.vel.z, (int)game.player.onGround, game.grainsInFlight());
            if (!game.physics.bodies.empty()) {
                const Body& b0 = *game.physics.bodies[0];
                logLine("   body0: pos=(%.1f %.1f %.1f) vel=%.2f ang=%.3f count=%d asleep=%d", b0.pos.x, b0.pos.y, b0.pos.z, length(b0.vel), length(b0.ang), b0.count, (int)b0.asleep);
            }
            logLine("script %d: %s | bodies=%zu awake=%zu sand=%lld water=%lld fps=%.0f frameMs=%.2f meshMs=%.2f contacts=%d chunks=%d",
                    lineNo, rest.c_str(), game.physics.bodies.size(), game.physics.awakeCount(), game.world.counts[M_SAND],
                    game.world.counts[M_WATER], r.stats.fps, r.stats.frameMs, r.stats.meshMs, game.physics.lastContactCount,
                    r.stats.chunksDrawn);
        } else if (cmd == "quit") {
            break;
        } else {
            logLine("script %d: unknown command '%s'", lineNo, cmd.c_str());
        }
    }
    return true;
}

}  // namespace

// ------------------------------------------------------------------------------------------------
// Entry point
// ------------------------------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    g_exeDir = exeDirectory();

    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], n, nullptr, nullptr);
        s.resize(s.size() - 1);
        args.push_back(s);
    }
    LocalFree(wargv);

    int width = 1600, height = 900;
    std::string scriptPath;
    int rays = -1, msaa = 4, qualityArg = -1, worldMode = -1;  // worldMode: 0 = classic arena, 1 = infinite, -1 = default
    bool qualityGiven = false;
    bool noVsync = false;
    float autoQuit = 0;
    for (size_t i = 1; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--selftest") {
            int failures = runSelfTest((g_exeDir + "\\selftest.log").c_str());
            return failures ? 1 : 0;
        } else if (a == "--script" && i + 1 < args.size()) {
            scriptPath = args[++i];
        } else if (a == "--size" && i + 1 < args.size()) {
            sscanf(args[++i].c_str(), "%dx%d", &width, &height);
        } else if (a == "--rays" && i + 1 < args.size()) {
            rays = atoi(args[++i].c_str());
        } else if (a == "--msaa" && i + 1 < args.size()) {
            msaa = atoi(args[++i].c_str());
        } else if (a == "--autoquit" && i + 1 < args.size()) {
            autoQuit = (float)atof(args[++i].c_str());
        } else if (a == "--arena") {
            worldMode = 0;
        } else if (a == "--infinite") {
            worldMode = 1;
        } else if (a == "--novsync") {
            noVsync = true;
        } else if (a == "--quality" && i + 1 < args.size()) {
            const std::string q = args[++i];
            qualityArg = q == "auto" ? -1 : std::clamp(atoi(q.c_str()), 0, 4);
            qualityGiven = true;
        }
    }
    g_scriptMode = !scriptPath.empty();

    logLine("=== Voxel Sandfall 0.0.1 starting ===");
    g_hwnd = createMainWindow(inst, width, height, !g_scriptMode);
    if (!g_hwnd) return 2;

    static Renderer renderer;
    g_renderer = &renderer;
    renderer.requestedSamples = msaa;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    if (!renderer.init(g_hwnd, rc.right - rc.left, rc.bottom - rc.top)) {
        std::string msg = "Failed to initialise Direct3D 11:\n" + renderer.lastError + "\n\nSee voxelsandfall.log";
        MessageBoxA(nullptr, msg.c_str(), "Voxel Sandfall", MB_OK | MB_ICONERROR);
        return 3;
    }
    if (rays >= 0) renderer.skyRays = rays;
    renderer.quality = qualityGiven ? qualityArg : 4;  // full detail unless asked otherwise
    if (renderer.quality >= 0) renderer.setQualityNow(renderer.quality);
    if (noVsync) renderer.vsync = false;
    logLine("renderer ready on %s", renderer.adapterName.c_str());

    static Game game;
    g_game = &game;
    // Playing: the endless world. Scripts / tests keep the classic walled arena unless --infinite is given.
    game.setInfinite(worldMode >= 0 ? worldMode == 1 : !g_scriptMode);

    if (g_scriptMode) {
        runScript(scriptPath);
        logLine("script finished");
        return 0;
    }

    setMouseCapture(true);
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);

    const double startTime = (double)last.QuadPart / (double)freq.QuadPart;
    while (g_running) {
        pumpMessages();
        if (!g_running) break;
        if (autoQuit > 0 && (double)last.QuadPart / (double)freq.QuadPart - startTime > autoQuit) break;

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart);
        last = now;
        dt = std::min(dt, 0.1f);

        // ---- one-shot keys -----------------------------------------------------------------
        if (pressed(VK_ESCAPE)) {
            if (g_captured) setMouseCapture(false);
            else g_running = false;
        }
        if (pressed(VK_F1)) renderer.showHelp = !renderer.showHelp;
        if (pressed(VK_F3)) renderer.showHud = !renderer.showHud;
        if (pressed(VK_F4)) renderer.showProfiler = !renderer.showProfiler;
        if (pressed(VK_F5)) game.reset();
        if (pressed(VK_F6)) renderer.skyRays = renderer.skyRays == 0 ? 4 : renderer.skyRays == 4 ? 8 : renderer.skyRays == 8 ? 16 : 0;
        if (pressed(VK_F7)) renderer.vsync = !renderer.vsync;
        if (pressed(VK_F8)) renderer.cycleMsaa();
        if (pressed(VK_F12)) renderer.quality = renderer.quality >= 4 ? -1 : renderer.quality + 1;  // auto, 0 .. 4
        if (pressed(VK_F9)) renderer.reloadShaders();
        if (pressed(VK_F11)) toggleFullscreen();
        if (pressed(VK_F2)) {
            CreateDirectoryA((g_exeDir + "\\shots").c_str(), nullptr);
            renderer.requestScreenshot(g_exeDir + "\\shots\\shot_" + timestamp() + ".png");
        }
        const float sunRate = 0.9f * dt;
        if (g_keys[VK_OEM_4]) renderer.sunAzimuth -= sunRate;
        if (g_keys[VK_OEM_6]) renderer.sunAzimuth += sunRate;
        if (g_keys[VK_OEM_1]) renderer.sunElevation = std::max(0.12f, renderer.sunElevation - sunRate);
        if (g_keys[VK_OEM_7]) renderer.sunElevation = std::min(1.5f, renderer.sunElevation + sunRate);

        // ---- gameplay input --------------------------------------------------------------------
        GameInput gi;
        if (g_captured) {
            gi.forward = (g_keys['W'] ? 1.0f : 0.0f) - (g_keys['S'] ? 1.0f : 0.0f);
            gi.strafe = (g_keys['D'] ? 1.0f : 0.0f) - (g_keys['A'] ? 1.0f : 0.0f);
            gi.jump = g_keys[VK_SPACE];
            gi.sprint = g_keys[VK_SHIFT];
            gi.fireHeld = g_lmb;
            gi.fireClicked = g_lmb && !g_prevLmb;
            gi.wheel = g_wheel;
            gi.selectCreator = pressed('R');
            gi.selectGun = pressed('F');
            for (int k = 1; k <= kPaintMaterialCount; k++)
                if (pressed(k == 10 ? '0' : '0' + k)) gi.selectMaterial = k;
            if (pressed('E')) gi.cycleMaterial = 1;
            if (pressed('Q')) gi.cycleMaterial = -1;
            POINT p;
            GetCursorPos(&p);
            POINT c = clientCenterScreen();
            if (!g_skipMouse) {
                gi.lookDX = (float)(p.x - c.x);
                gi.lookDY = (float)(p.y - c.y);
            }
            g_skipMouse = false;
            SetCursorPos(c.x, c.y);
        } else {
            g_lmb = false;
        }
        g_wheel = 0;
        std::copy(std::begin(g_keys), std::end(g_keys), std::begin(g_prevKeys));
        g_prevLmb = g_lmb;

        if (g_captured) game.update(dt, gi);
        renderer.speedForBob = (g_captured && game.player.onGround) ? std::sqrt(game.player.vel.x * game.player.vel.x + game.player.vel.z * game.player.vel.z) : 0.0f;
        renderer.render(game, dt);

        char title[256];
        snprintf(title, sizeof(title), "Voxel Sandfall 0.0.1 - prototype engine  |  %.0f fps  |  %s", renderer.stats.fps, renderer.adapterName.c_str());
        static int titleTick = 0;
        if ((titleTick++ & 31) == 0) SetWindowTextA(g_hwnd, title);
    }
    setMouseCapture(false);
    return 0;
}
