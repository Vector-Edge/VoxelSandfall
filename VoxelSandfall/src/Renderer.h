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

    RenderStats stats;
    std::string adapterName;
    std::string lastError;

private:
    struct GpuMesh {
        ComPtr<ID3D11Buffer> vb, ib;
        UINT indexCount = 0;
        UINT vbCap = 0, ibCap = 0;  // allocated bytes (buffers are reused and only grow)
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
    void updateBodies(Game& g);
    void uploadMesh(GpuMesh& gm, const MeshData& md);
    void uploadDynamic(ComPtr<ID3D11Buffer>& vb, ComPtr<ID3D11Buffer>& ib, UINT& indexCount, UINT& vbCap, UINT& ibCap,
                       const MeshData& md);
    void setObject(const Mat4& model, float r, float g, float b, float a, float volumeAO = 0);
    void drawSky();
    void drawChunks(bool water, const Frustum* fr);
    void drawBodies(const Game& g);
    void drawParticles();
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
    static constexpr int kShadowSize = 4096;
    ComPtr<ID3D11Texture2D> shadowTex_;
    ComPtr<ID3D11DepthStencilView> shadowDSV_;
    ComPtr<ID3D11ShaderResourceView> shadowSRV_;
    ComPtr<ID3D11Texture3D> occTex_, brickTex_;
    ComPtr<ID3D11ShaderResourceView> occSRV_, brickSRV_;

    // shaders
    ComPtr<ID3D11VertexShader> vsShadow_, vsWorld_, vsSky_, vsLine_, vsUI_, vsView_;
    ComPtr<ID3D11PixelShader> psWorld_, psWater_, psSky_, psLine_, psUI_, psView_, psFlash_;
    ComPtr<ID3D11InputLayout> layWorld_, layLine_, layUI_, layView_;

    // states
    ComPtr<ID3D11RasterizerState> rsSolid_, rsShadow_, rsNoCull_;
    ComPtr<ID3D11DepthStencilState> dsLess_, dsLessNoWrite_, dsOff_, dsEqual_, dsSky_;
    ComPtr<ID3D11BlendState> bsOpaque_, bsAlpha_, bsAdd_;
    ComPtr<ID3D11SamplerState> sampShadow_, sampLinear_;

    // constant buffers
    ComPtr<ID3D11Buffer> cbFrame_, cbObject_, cbUI_, cbVM_;

    // geometry
    std::vector<GpuMesh> chunkOpaque_, chunkWater_;
    std::unordered_map<int, BodyGpu> bodyMeshes_;
    ComPtr<ID3D11Buffer> lineVB_, uiVB_;
    UINT lineCap_ = 0, uiCap_ = 0;
    ComPtr<ID3D11Buffer> partVB_, partIB_;
    UINT partIndexCount_ = 0, partVBCap_ = 0, partIBCap_ = 0;
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
        bool mesh = false, tex = false;
        MeshData opaque, water;
        uint8_t bricks[64];
    };
    std::vector<ChunkWork> work_;
    bool sceneDirty_ = true;            // something the shadow map shows has changed
    float shadowAz_ = -1, shadowEl_ = -1;

    // GPU timing (3 frames in flight)
    ComPtr<ID3D11Query> qDisjoint_[3];
    ComPtr<ID3D11Query> qStamp_[3][5];
    bool qUsed_[3] = {false, false, false};

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
