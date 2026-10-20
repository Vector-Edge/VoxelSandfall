// Direct3D 11 renderer: chunk meshes, rigid bodies, cascade-free sun shadow map, voxel-ray-traced sky
// light, water, debris, first-person rifle and HUD. No third-party libraries.
#pragma once
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "Game.h"
#include "Mesher.h"
#include "Profile.h"
#include "ViewModel.h"

using Microsoft::WRL::ComPtr;

struct Frustum;

struct RenderStats {
    int chunksDrawn = 0;
    int chunkRebuilds = 0;
    int bodiesDrawn = 0;
    double meshMs = 0;
    double frameMs = 0;
    float fps = 0;
};

class Renderer {
public:
    bool init(HWND hwnd, int width, int height);
    void resize(int width, int height);
    // Updates GPU resources from the game state and draws one frame (then presents).
    void render(Game& game, float dt);
    bool reloadShaders();
    void requestScreenshot(const std::string& path) { screenshotPath_ = path; }
    void cycleMsaa();                 // 1x -> 2x -> 4x -> 8x (whatever the GPU supports)
    int msaaSamples() const { return sampleCount_; }

    // User-facing settings.
    int skyRays = 8;          // 0 disables ray-traced sky lighting
    bool vsync = true;
    int requestedSamples = 4;     // MSAA sample count (1, 2, 4 or 8); read at init
    bool showHelp = true;
    bool showHud = true;
    float sunAzimuth = 0.62f;     // radians around Y
    float sunElevation = 0.95f;   // radians above the horizon
    float speedForBob = 0;        // set by the caller (horizontal speed)
    bool showProfiler = false;    // F4
    // Graphics quality: -1 = automatic (steps up and down so the frame cost stays under targetMs), 0..4 = fixed tier
    // (0 = lowest: 2 sky rays, 67 % resolution, simple water; 4 = everything). F12 cycles.
    int quality = 4;              // the full-detail look by default; F12 / --quality auto lets it adapt
    float targetMs = 9.0f;        // frame cost (CPU work or GPU time, whichever is larger) the automatic mode aims for
    void setQualityNow(int q) {
        quality = q;
        if (q >= 0) setTier(q);
    }
    int currentTier() const { return tier_; }
    float currentScale() const { return renderScale_; }
    // Debug / screenshot camera that does not move the player (script command `viewcam`).
    bool camOverride = false;
    Vec3 camOverridePos;
    float camOverrideYaw = 0, camOverridePitch = 0;

    RenderStats stats;
    std::string adapterName;
    std::string lastError;

private:
    struct GpuMesh {
        ComPtr<ID3D11Buffer> vb, ib;
        UINT indexCount = 0;
        UINT vbCap = 0, ibCap = 0;  // allocated bytes (buffers are reused and only grow)
        float sx = 0, sz = 0;       // translation since it was built (the world window has moved; cleared by the next rebuild)
    };
    struct BodyGpu {
        GpuMesh mesh;
        uint32_t version = 0;
        bool seen = false;
    };

    // setup
    bool createDevice(HWND hwnd);
    bool createSizeResources();
    bool createStaticResources();
    bool createShaders(bool initial);
    bool compile(const char* file, const char* entry, const char* target, ComPtr<ID3DBlob>& blob);
    std::string findShaderDir() const;
    bool createDynamicBuffer(ComPtr<ID3D11Buffer>& buf, UINT bytes, UINT bind);
    void createTextResources();

    // per-frame
    void updateWorldGpu(Game& g);
    void updateWaterField(Game& g);
    void updateBodies(Game& g);
    void uploadMesh(GpuMesh& gm, const MeshData& md);
    void uploadDynamic(ComPtr<ID3D11Buffer>& vb, ComPtr<ID3D11Buffer>& ib, UINT& indexCount, UINT& vbCap, UINT& ibCap,
                       const MeshData& md);
    void setObject(const Mat4& model, float r, float g, float b, float a, float volumeAO = 0);
    void drawSky();
    void drawChunks(bool water, const Frustum* fr, float volumeAO);
    void applyShift(Game& g, int dRx, int dRz);  // the world window moved by whole regions: move the GPU data with it
    void resetChunkGpu();
    void drawBodies(const Game& g);
    void drawParticles();
    void buildFx(const Game& g);  // sparks and smoke as camera-facing sprites (smoke sorted back to front)
    void drawFx();
    void buildFireHeat(const Game& g);  // the flames' heat field (what firevol.hlsl turns into fire)
    void drawFireVolume();
    void drawBrush(const Game& g);
    void drawViewModel(const Game& g);
    void drawHud(const Game& g);
    void buildHudText(const Game& g, std::vector<std::string>& hud, std::vector<std::string>& help);
    void saveScreenshot(const std::string& path);
    void gpuBegin();
    void gpuStamp(int i);
    void gpuEnd();

    // device
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain1> swap_;
    ComPtr<ID3D11Texture2D> backTex_;
    ComPtr<ID3D11RenderTargetView> backRTV_;
    ComPtr<ID3D11Texture2D> msaaColor_, msaaDepth_;
    ComPtr<ID3D11RenderTargetView> msaaRTV_;
    ComPtr<ID3D11DepthStencilView> msaaDSV_;
    int sampleCount_ = 4;
    int width_ = 0, height_ = 0;
    HWND hwnd_ = nullptr;
    std::string shaderDir_;

    // shadow + occupancy
    int shadowSize_ = 4096;            // 2048 on the lowest tiers
    bool createShadowMap(int size);
    ComPtr<ID3D11Texture2D> shadowTex_;
    ComPtr<ID3D11DepthStencilView> shadowDSV_;
    ComPtr<ID3D11ShaderResourceView> shadowSRV_;
    ComPtr<ID3D11Texture3D> occTex_, brickTex_;
    ComPtr<ID3D11ShaderResourceView> occSRV_, brickSRV_;
    ComPtr<ID3D11Texture3D> occTexB_, brickTexB_;  // the other half of a ping-pong pair: the window shifts by copying into it
    ComPtr<ID3D11ShaderResourceView> occSRVB_, brickSRVB_;
    uint32_t epoch_ = 0;
    ComPtr<ID3D11Texture2D> waterFieldTex_;           // ripples / foam / surface slope (see WaterField.h)
    ComPtr<ID3D11ShaderResourceView> waterFieldSRV_;

    // shaders
    ComPtr<ID3D11VertexShader> vsShadow_, vsWorld_, vsSky_, vsLine_, vsUI_, vsView_, vsFx_;
    ComPtr<ID3D11PixelShader> psWorld_, psWater_, psUnder_, psBlit_, psSky_, psLine_, psUI_, psView_, psFlash_, psFire_, psSmoke_,
        psFireVol_, psDepthMS_, psDepth1_;
    ComPtr<ID3D11InputLayout> layWorld_, layLine_, layUI_, layView_, layFx_;

    // states
    ComPtr<ID3D11RasterizerState> rsSolid_, rsShadow_, rsNoCull_;
    ComPtr<ID3D11DepthStencilState> dsLess_, dsLessNoWrite_, dsOff_, dsEqual_, dsSky_;
    ComPtr<ID3D11BlendState> bsOpaque_, bsAlpha_, bsAdd_, bsWater_, bsFire_;
    ComPtr<ID3D11SamplerState> sampShadow_, sampLinear_, sampWrap_;

    // constant buffers
    ComPtr<ID3D11Buffer> cbFrame_, cbObject_, cbUI_, cbVM_;

    // geometry
    std::vector<GpuMesh> chunkOpaque_, chunkWater_;
    ComPtr<ID3D11Buffer> chunkOffVB_;  // per-instance (x, z) translation of every chunk mesh (slot 1 of the world layout)
    std::vector<float> chunkOff_;
    bool chunkOffDirty_ = true;
    std::unordered_map<int, BodyGpu> bodyMeshes_;
    ComPtr<ID3D11Buffer> lineVB_, uiVB_;
    UINT lineCap_ = 0, uiCap_ = 0;
    ComPtr<ID3D11Buffer> partVB_, partIB_;
    UINT partIndexCount_ = 0, partVBCap_ = 0, partIBCap_ = 0;
    struct FxInst {  // per-instance data of one flame / smoke sprite (matches fx.hlsl)
        float x, y, z, halfSize;
        uint32_t col;   // rgba8
        float rot, kind;
        uint32_t pad;
    };
    // fire volume: a coarse 3D heat texture over the whole window (one texel per kHeatCell voxels), rebuilt on the CPU every frame
    static constexpr int kHeatCell = 4;
    static constexpr int kHX = cfg::WORLD_X / kHeatCell, kHY = cfg::WORLD_Y / kHeatCell, kHZ = cfg::WORLD_Z / kHeatCell;
    ComPtr<ID3D11Texture3D> heatTex_, noiseTex_;
    ComPtr<ID3D11ShaderResourceView> heatSRV_, noiseSRV_;
    ComPtr<ID3D11Texture2D> fireDepthTex_;      // the scene's depth, one sample per pixel
    ComPtr<ID3D11RenderTargetView> fireDepthRTV_;
    ComPtr<ID3D11ShaderResourceView> fireDepthSRV_, msaaDepthSRV_;
    std::vector<uint8_t> heat_;
    int heatMin_[3] = {1 << 20, 1 << 20, 1 << 20}, heatMax_[3] = {-1, -1, -1};  // texels written last frame (they are cleared first)
    std::vector<float> colStr_, colY_;
    std::vector<uint32_t> colStamp_;
    std::vector<int> colList_;
    uint32_t heatStamp_ = 0;
    bool fireActive_ = false;
    Vec3 fireBoxMin_, fireBoxMax_;
    ComPtr<ID3D11Buffer> fxVB_;
    UINT fxCap_ = 0;                  // instances the buffer holds
    std::vector<FxInst> fxInst_;      // smoke first (sorted far to near), then flames and sparks
    UINT fxSmoke_ = 0, fxFire_ = 0;
    ComPtr<ID3D11Buffer> rifleVB_, rifleIB_;
    ViewModelMesh rifle_;

    // text
    ComPtr<ID3D11Texture2D> textTex_, whiteTex_;
    ComPtr<ID3D11ShaderResourceView> textSRV_, whiteSRV_;
    HDC textDC_ = nullptr;
    HBITMAP textBmp_ = nullptr;
    HFONT textFont_ = nullptr;
    void* textBits_ = nullptr;
    static constexpr int kTextW = 1024, kTextH = 768;
    std::string textKey_;
    struct TextLine { int y, w, h; };
    std::vector<TextLine> textLines_;
    int hudLineCount_ = 0;

    // parallel chunk meshing
    struct ChunkWork {
        int ci = 0;
        bool mesh = false, waterOnly = false, tex = false, skipEmpty = false;
        MeshData opaque, water;
        uint8_t bricks[64];
        uint8_t tops[1024];  // open-water surface heights per column (waterTopsOfMesh)
    };
    std::vector<ChunkWork> work_;
    std::vector<uint8_t> chunkTops_;     // per chunk: 32x32 water surface heights
    std::vector<uint16_t> waterTopMap_;  // per world column: highest open-water surface plane (0 = none)
    bool topsDirty_ = false;
    int topsX0_ = 1 << 20, topsZ0_ = 1 << 20, topsX1_ = -1, topsZ1_ = -1;  // columns whose water surface changed
    bool castingParticles_ = false;     // some flying particle is not water (the shadow map must follow it)
    uint32_t waterMapTick_ = 0;
    bool sceneDirty_ = true;            // something the shadow map shows has changed
    float shadowAz_ = -1, shadowEl_ = -1;

    // GPU timing: the GPU can run several frames behind the CPU, so results are collected from a ring of queries
    static constexpr int kGpuRing = 8;
    ComPtr<ID3D11Query> qDisjoint_[kGpuRing];
    ComPtr<ID3D11Query> qStamp_[kGpuRing][5];
    bool qUsed_[kGpuRing] = {};
    bool gpuTiming_ = false;   // this frame carries timestamps

    // quality / render scale
    struct Tier {
        int rays;          // sky rays per pixel
        float scale;       // render resolution relative to the window
        int water;         // water shader detail 0..2
        float rayLen;      // sky ray length
        int raySteps;      // sky ray step limit
        bool shadow9;      // 9 tap shadow filter (else 4)
        bool contact;      // contact shadow ray
    };
    static const Tier kTiers[5];
    int tier_ = 4;
    int userSamples_ = 0;               // the MSAA level the user asked for (the lowest tiers use less)
    float renderScale_ = 1.0f;
    int sceneW_ = 0, sceneH_ = 0;      // size the 3D scene is rendered at
    ComPtr<ID3D11Texture2D> sceneTex_;  // resolved scene when it is upscaled
    ComPtr<ID3D11ShaderResourceView> sceneSRV_;
    int qualOver_ = 0, qualUnder_ = 0;  // consecutive frames over / well under the budget
    double lastTierChange_ = -100, tierBlockedUntil_[5] = {};
    int qualityFrames_ = 0;
    void setTier(int t);
    void updateAutoQuality();

    // frame state
    Mat4 view_, proj_, viewProj_, lightViewProj_;
    Vec3 eye_, camFwd_, camRight_, camUp_;
    float fovY_ = deg2rad(72.0f);
    std::string screenshotPath_;
    int rebuildCursor_ = 0;
    bool firstBuild_ = true;
    double fpsAccum_ = 0;
    int fpsFrames_ = 0;
    float fpsShown_ = 0;
    float bobTime_ = 0;
    uint64_t frameIndex_ = 0;
};
