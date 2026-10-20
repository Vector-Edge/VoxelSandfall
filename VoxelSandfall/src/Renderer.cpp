#include "Renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <chrono>
#include <cstdio>

#include "Log.h"
#include "Parallel.h"
#include "Png.h"

// ------------------------------------------------------------------------------------------------
// Local helpers / constant buffer layouts (must match shaders/common.hlsli, viewmodel.hlsl, ui.hlsl)
// ------------------------------------------------------------------------------------------------
namespace {

struct FrameCB {
    Mat4 viewProj, lightViewProj;
    float camPos[4], sunDir[4], sunColor[4], skyZenith[4], skyHorizon[4], groundCol[4];
    float params[4], gridDims[4], camRight[4], camUp[4], camFwd[4], qual[4];
};
struct ObjectCB {
    Mat4 model;
    float tint[4];
    float objParams[4];
};
struct VMCB {
    Mat4 mvp, modelView;
    float lightDirView[4], ambientCol[4], flashCol[4];
};
struct UICB {
    float screen[2], pad[2];
};
struct UIVertex {
    float x, y, u, v;
    uint32_t col;
};

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

uint32_t packColor(float r, float g, float b, float a) {
    auto c = [](float v) { return (uint32_t)(clampf(v, 0.f, 1.f) * 255.0f + 0.5f); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | (c(a) << 24);
}

std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    if (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

}  // namespace

struct Frustum {
    float p[6][4];
};

static Frustum extractFrustum(const Mat4& m) {
    Frustum f;
    auto row = [&](int r, int i) { return m.m[r][i]; };
    float pl[6][4];
    for (int i = 0; i < 4; i++) {
        pl[0][i] = row(3, i) + row(0, i);  // left
        pl[1][i] = row(3, i) - row(0, i);  // right
        pl[2][i] = row(3, i) + row(1, i);  // bottom
        pl[3][i] = row(3, i) - row(1, i);  // top
        pl[4][i] = row(2, i);              // near (depth 0..1)
        pl[5][i] = row(3, i) - row(2, i);  // far
    }
    for (int k = 0; k < 6; k++) {
        float l = std::sqrt(pl[k][0] * pl[k][0] + pl[k][1] * pl[k][1] + pl[k][2] * pl[k][2]);
        for (int i = 0; i < 4; i++) f.p[k][i] = pl[k][i] / l;
    }
    return f;
}

static bool aabbVisible(const Frustum& f, const Vec3& mn, const Vec3& mx) {
    for (int k = 0; k < 6; k++) {
        const float* p = f.p[k];
        float x = p[0] >= 0 ? mx.x : mn.x, y = p[1] >= 0 ? mx.y : mn.y, z = p[2] >= 0 ? mx.z : mn.z;
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0) return false;
    }
    return true;
}

#define VS_CHECK(expr)                                                               \
    do {                                                                             \
        HRESULT _hr = (expr);                                                        \
        if (FAILED(_hr)) {                                                           \
            char _b[512];                                                            \
            snprintf(_b, sizeof(_b), "%s failed (0x%08lX)", #expr, (unsigned long)_hr); \
            lastError = _b;                                                          \
            logLine("%s", _b);                                                       \
            return false;                                                            \
        }                                                                            \
    } while (0)

// ------------------------------------------------------------------------------------------------
// Initialisation
// ------------------------------------------------------------------------------------------------
std::string Renderer::findShaderDir() const {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir(exe);
    dir = dir.substr(0, dir.find_last_of("\\/"));
    const std::string cands[] = {dir + "\\..\\..\\VoxelSandfall\\shaders", dir + "\\shaders", ".\\shaders", ".\\VoxelSandfall\\shaders"};
    for (const std::string& c : cands) {
        DWORD a = GetFileAttributesA((c + "\\common.hlsli").c_str());
        if (a != INVALID_FILE_ATTRIBUTES) return c;
    }
    return dir + "\\shaders";
}

bool Renderer::init(HWND hwnd, int width, int height) {
    hwnd_ = hwnd;
    width_ = std::max(width, 16);
    height_ = std::max(height, 16);
    shaderDir_ = findShaderDir();
    logLine("shader directory: %s", shaderDir_.c_str());
    if (!createDevice(hwnd)) return false;
    if (!createSizeResources()) return false;
    if (!createStaticResources()) return false;
    if (!createShaders(true)) return false;
    createTextResources();
    rifle_ = buildRifle();
    return true;
}

bool Renderer::createDevice(HWND hwnd) {
    ComPtr<IDXGIFactory2> fac;
    VS_CHECK(CreateDXGIFactory1(IID_PPV_ARGS(&fac)));
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIFactory6> fac6;
    if (SUCCEEDED(fac.As(&fac6)))
        fac6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (!adapter) fac->EnumAdapters1(0, &adapter);
    if (adapter) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        adapterName = narrow(d.Description);
    }
    logLine("adapter: %s", adapterName.c_str());

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = E_FAIL;
#ifdef _DEBUG
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags | D3D11_CREATE_DEVICE_DEBUG, levels, 2,
                           D3D11_SDK_VERSION, &dev_, &got, &ctx_);
#endif
    if (FAILED(hr))
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &dev_, &got,
                               &ctx_);
    VS_CHECK(hr);

    UINT q = 0;
    sampleCount_ = requestedSamples;
    if (sampleCount_ > 1 && (FAILED(dev_->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)sampleCount_, &q)) || q == 0)) sampleCount_ = 1;
    if (sampleCount_ > 1 && (FAILED(dev_->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT, (UINT)sampleCount_, &q)) || q == 0)) sampleCount_ = 1;

    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = (UINT)width_;
    sd.Height = (UINT)height_;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    VS_CHECK(fac->CreateSwapChainForHwnd(dev_.Get(), hwnd, &sd, nullptr, nullptr, &swap_));
    fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

const Renderer::Tier Renderer::kTiers[5] = {
    {2, 0.67f, 0, 28.0f, 32, false, false},
    {3, 0.78f, 0, 32.0f, 38, false, true},
    {4, 0.88f, 1, 36.0f, 42, true, true},
    {6, 1.00f, 1, 44.0f, 52, true, true},
    {8, 1.00f, 2, 44.0f, 52, true, true},
};

// Switches to a quality tier; a different render scale means new scene render targets.
void Renderer::setTier(int t) {
    t = std::clamp(t, 0, 4);
    const float oldScale = renderScale_;
    const int oldSamples = sampleCount_;
    if (userSamples_ == 0) userSamples_ = sampleCount_;
    tier_ = t;
    renderScale_ = kTiers[t].scale;
    static const int kMsaaCap[5] = {1, 2, 8, 8, 8};  // multisampling is bandwidth the weakest GPUs cannot spare
    sampleCount_ = std::min(userSamples_, kMsaaCap[t]);
    lastTierChange_ = prof::nowMs() * 0.001;
    qualOver_ = qualUnder_ = 0;
    const int wantShadow = t <= 1 ? 2048 : 4096;  // a quarter of the shadow pixels on the weakest tiers
    if (wantShadow != shadowSize_ && swap_) {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        createShadowMap(wantShadow);
    }
    if ((renderScale_ != oldScale || sampleCount_ != oldSamples) && swap_) {
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        createSizeResources();
    }
    logLine("quality tier %d (render scale %.0f %%, %d sky rays, water detail %d)", tier_, renderScale_ * 100.0f, kTiers[t].rays, kTiers[t].water);
}

// Automatic quality: keeps the cost of a frame (the larger of the CPU work and the GPU time, smoothed) under targetMs.
// Over budget for a third of a second -> one tier down; comfortably under for several seconds -> one tier up (but not
// straight back into a tier that has just proved too expensive).
void Renderer::updateAutoQuality() {
    if (quality >= 0) {
        if (quality != tier_) setTier(quality);
        return;
    }
    if (++qualityFrames_ < 90 || firstBuild_) return;
    double cpu = 0;
    for (int i = 0; i < prof::P_COUNT; i++)
        if (i != prof::P_PRESENT) cpu += prof::g_avg[i];
    const double gpu = prof::g_gpuAvg[prof::G_TOTAL];
    if (gpu <= 0.0) return;  // no GPU timings on this device: stay where we are
    const double cost = std::max(cpu, gpu);
    const double now = prof::nowMs() * 0.001;
    if (cost > targetMs) {
        qualUnder_ = 0;
        if (++qualOver_ >= 20 && tier_ > 0) {
            if (now - lastTierChange_ < 8.0) tierBlockedUntil_[tier_] = now + 25.0;  // the tier we just stepped up to was too much
            setTier(tier_ - 1);
        }
    } else if (cost < targetMs * 0.62) {
        qualOver_ = 0;
        if (++qualUnder_ >= 150 && tier_ < 4 && now - lastTierChange_ > 3.0 && now >= tierBlockedUntil_[tier_ + 1]) setTier(tier_ + 1);
    } else {
        qualOver_ = qualUnder_ = 0;
    }
}

bool Renderer::createSizeResources() {
    backRTV_.Reset();
    backTex_.Reset();
    msaaColor_.Reset();
    msaaDepth_.Reset();
    msaaRTV_.Reset();
    msaaDSV_.Reset();
    sceneTex_.Reset();
    sceneSRV_.Reset();
    VS_CHECK(swap_->GetBuffer(0, IID_PPV_ARGS(&backTex_)));
    VS_CHECK(dev_->CreateRenderTargetView(backTex_.Get(), nullptr, &backRTV_));

    const bool scaled = renderScale_ < 0.999f;
    sceneW_ = scaled ? std::max(16, ((int)std::lround(width_ * renderScale_) + 1) & ~1) : width_;
    sceneH_ = scaled ? std::max(16, ((int)std::lround(height_ * renderScale_) + 1) & ~1) : height_;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)sceneW_;
    td.Height = (UINT)sceneH_;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = (UINT)sampleCount_;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    VS_CHECK(dev_->CreateTexture2D(&td, nullptr, &msaaColor_));
    VS_CHECK(dev_->CreateRenderTargetView(msaaColor_.Get(), nullptr, &msaaRTV_));
    td.Format = DXGI_FORMAT_D32_FLOAT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    VS_CHECK(dev_->CreateTexture2D(&td, nullptr, &msaaDepth_));
    VS_CHECK(dev_->CreateDepthStencilView(msaaDepth_.Get(), nullptr, &msaaDSV_));
    if (scaled) {  // the resolved scene, which the upscale pass reads
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        VS_CHECK(dev_->CreateTexture2D(&td, nullptr, &sceneTex_));
        VS_CHECK(dev_->CreateShaderResourceView(sceneTex_.Get(), nullptr, &sceneSRV_));
    }
    return true;
}

void Renderer::cycleMsaa() {
    static const int order[4] = {1, 2, 4, 8};
    int cur = 0;
    for (int i = 0; i < 4; i++)
        if (order[i] == sampleCount_) cur = i;
    for (int step = 1; step <= 4; step++) {
        const int cand = order[(cur + step) % 4];
        UINT q1 = 0, q2 = 0;
        if (cand == 1 || (SUCCEEDED(dev_->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, (UINT)cand, &q1)) && q1 > 0 &&
                          SUCCEEDED(dev_->CheckMultisampleQualityLevels(DXGI_FORMAT_D32_FLOAT, (UINT)cand, &q2)) && q2 > 0)) {
            ctx_->OMSetRenderTargets(0, nullptr, nullptr);
            userSamples_ = cand;
            sampleCount_ = cand;
            if (tier_ == 0) sampleCount_ = 1;
            else if (tier_ == 1) sampleCount_ = std::min(cand, 2);
            createSizeResources();
            logLine("MSAA now %dx", cand);
            return;
        }
    }
}

void Renderer::resize(int width, int height) {
    if (width < 16 || height < 16 || !swap_) return;
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    backRTV_.Reset();
    backTex_.Reset();
    msaaColor_.Reset();
    msaaDepth_.Reset();
    msaaRTV_.Reset();
    msaaDSV_.Reset();
    sceneTex_.Reset();
    sceneSRV_.Reset();
    if (FAILED(swap_->ResizeBuffers(0, (UINT)width_, (UINT)height_, DXGI_FORMAT_UNKNOWN, 0))) {
        logLine("ResizeBuffers failed");
        return;
    }
    createSizeResources();
}

bool Renderer::createDynamicBuffer(ComPtr<ID3D11Buffer>& buf, UINT bytes, UINT bind) {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = bind;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(dev_->CreateBuffer(&bd, nullptr, &buf));
}

bool Renderer::createShadowMap(int size) {
    shadowTex_.Reset();
    shadowDSV_.Reset();
    shadowSRV_.Reset();
    shadowSize_ = size;
    sceneDirty_ = true;  // the map has to be drawn again
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = td.Height = (UINT)size;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R32_TYPELESS;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        VS_CHECK(dev_->CreateTexture2D(&td, nullptr, &shadowTex_));
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {};
        dv.Format = DXGI_FORMAT_D32_FLOAT;
        dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        VS_CHECK(dev_->CreateDepthStencilView(shadowTex_.Get(), &dv, &shadowDSV_));
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_R32_FLOAT;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MipLevels = 1;
        VS_CHECK(dev_->CreateShaderResourceView(shadowTex_.Get(), &sv, &shadowSRV_));
    }
    return true;
}

bool Renderer::createStaticResources() {
    if (!createShadowMap(shadowSize_)) return false;
    // World occupancy volume used for ray-traced sky light (same layout as the chunk storage).
    {
        D3D11_TEXTURE3D_DESC td = {};
        td.Width = cfg::WORLD_X;
        td.Height = cfg::WORLD_Y;
        td.Depth = cfg::WORLD_Z;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UINT;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        VS_CHECK(dev_->CreateTexture3D(&td, nullptr, &occTex_));
        VS_CHECK(dev_->CreateShaderResourceView(occTex_.Get(), nullptr, &occSRV_));
        td.Width = cfg::WORLD_X / 8;
        td.Height = cfg::WORLD_Y / 8;
        td.Depth = cfg::WORLD_Z / 8;
        VS_CHECK(dev_->CreateTexture3D(&td, nullptr, &brickTex_));
        VS_CHECK(dev_->CreateShaderResourceView(brickTex_.Get(), nullptr, &brickSRV_));
    }
    // Water surface field (ripples, foam, smoothed slope): RGBA16F, one texel per world column.
    {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = WaterField::W;
        td.Height = WaterField::D;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        VS_CHECK(dev_->CreateTexture2D(&td, nullptr, &waterFieldTex_));
        VS_CHECK(dev_->CreateShaderResourceView(waterFieldTex_.Get(), nullptr, &waterFieldSRV_));
        const std::vector<uint16_t> zero((size_t)WaterField::W * WaterField::D * 4, 0);
        ctx_->UpdateSubresource(waterFieldTex_.Get(), 0, nullptr, zero.data(), WaterField::W * 8, 0);
    }
    // States.
    {
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_BACK;
        rd.FrontCounterClockwise = TRUE;
        rd.DepthClipEnable = TRUE;
        rd.MultisampleEnable = TRUE;
        rd.AntialiasedLineEnable = TRUE;
        VS_CHECK(dev_->CreateRasterizerState(&rd, &rsSolid_));
        rd.CullMode = D3D11_CULL_NONE;
        VS_CHECK(dev_->CreateRasterizerState(&rd, &rsNoCull_));
        rd.CullMode = D3D11_CULL_FRONT;  // render back faces into the shadow map: no acne on lit faces
        rd.SlopeScaledDepthBias = 1.5f;
        rd.MultisampleEnable = FALSE;
        VS_CHECK(dev_->CreateRasterizerState(&rd, &rsShadow_));

        D3D11_DEPTH_STENCIL_DESC dd = {};
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        VS_CHECK(dev_->CreateDepthStencilState(&dd, &dsLess_));
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        VS_CHECK(dev_->CreateDepthStencilState(&dd, &dsLessNoWrite_));
        dd.DepthFunc = D3D11_COMPARISON_EQUAL;  // colour pass after the depth pre-pass: shade each pixel once
        VS_CHECK(dev_->CreateDepthStencilState(&dd, &dsEqual_));
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;  // sky only where nothing was drawn (depth == 1)
        VS_CHECK(dev_->CreateDepthStencilState(&dd, &dsSky_));
        dd.DepthFunc = D3D11_COMPARISON_LESS;
        dd.DepthEnable = FALSE;
        VS_CHECK(dev_->CreateDepthStencilState(&dd, &dsOff_));

        D3D11_BLEND_DESC bd = {};
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        VS_CHECK(dev_->CreateBlendState(&bd, &bsOpaque_));
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        VS_CHECK(dev_->CreateBlendState(&bd, &bsAlpha_));
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
        VS_CHECK(dev_->CreateBlendState(&bd, &bsAdd_));
        // Water: dual-source blend, result = water.add + background * water.mul (per-channel absorption).
        D3D11_BLEND_DESC wb = {};
        wb.RenderTarget[0].BlendEnable = TRUE;
        wb.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        wb.RenderTarget[0].DestBlend = D3D11_BLEND_SRC1_COLOR;
        wb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        wb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        wb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        wb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        wb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        VS_CHECK(dev_->CreateBlendState(&wb, &bsWater_));

        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = sd.BorderColor[3] = 1.0f;
        sd.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        VS_CHECK(dev_->CreateSamplerState(&sd, &sampShadow_));
        D3D11_SAMPLER_DESC ld = {};
        ld.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        ld.AddressU = ld.AddressV = ld.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        ld.ComparisonFunc = D3D11_COMPARISON_NEVER;
        ld.MaxLOD = D3D11_FLOAT32_MAX;
        VS_CHECK(dev_->CreateSamplerState(&ld, &sampLinear_));
    }
    // Constant buffers.
    auto makeCB = [&](ComPtr<ID3D11Buffer>& b, UINT bytes) { return createDynamicBuffer(b, (bytes + 15) & ~15u, D3D11_BIND_CONSTANT_BUFFER); };
    if (!makeCB(cbFrame_, sizeof(FrameCB)) || !makeCB(cbObject_, sizeof(ObjectCB)) || !makeCB(cbUI_, sizeof(UICB)) ||
        !makeCB(cbVM_, sizeof(VMCB))) {
        lastError = "constant buffer creation failed";
        return false;
    }
    chunkOpaque_.assign((size_t)cfg::WORLD_X / 32 * (cfg::WORLD_Y / 32) * (cfg::WORLD_Z / 32), GpuMesh());
    chunkWater_.assign(chunkOpaque_.size(), GpuMesh());
    chunkTops_.assign(chunkOpaque_.size() * 1024, 0);
    waterTopMap_.assign((size_t)WaterField::W * WaterField::D, 0);
    lineCap_ = 8192 * 12;
    uiCap_ = 4096 * sizeof(UIVertex);
    if (!createDynamicBuffer(lineVB_, lineCap_, D3D11_BIND_VERTEX_BUFFER) || !createDynamicBuffer(uiVB_, uiCap_, D3D11_BIND_VERTEX_BUFFER)) {
        lastError = "dynamic buffer creation failed";
        return false;
    }
    return true;
}

bool Renderer::compile(const char* file, const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
    std::wstring path = widen(shaderDir_ + "\\" + file);
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompileFromFile(path.c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, target, flags, 0, &blob, &err);
    if (FAILED(hr)) {
        lastError = std::string(file) + ":" + entry + " - " + (err ? (const char*)err->GetBufferPointer() : "compile failed");
        logLine("SHADER ERROR %s", lastError.c_str());
        return false;
    }
    return true;
}

bool Renderer::createShaders(bool initial) {
    ComPtr<ID3DBlob> b;
    ComPtr<ID3D11VertexShader> vsShadow, vsWorld, vsSky, vsLine, vsUI, vsView;
    ComPtr<ID3D11PixelShader> psWorld, psWater, psUnder, psBlit, psSky, psLine, psUI, psView, psFlash;
    ComPtr<ID3D11InputLayout> layWorld, layLine, layUI, layView;
    auto vs = [&](const char* f, const char* e, ComPtr<ID3D11VertexShader>& out, ComPtr<ID3DBlob>* keep = nullptr) {
        ComPtr<ID3DBlob> blob;
        if (!compile(f, e, "vs_5_0", blob)) return false;
        if (FAILED(dev_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out))) return false;
        if (keep) *keep = blob;
        return true;
    };
    auto ps = [&](const char* f, const char* e, ComPtr<ID3D11PixelShader>& out) {
        ComPtr<ID3DBlob> blob;
        if (!compile(f, e, "ps_5_0", blob)) return false;
        return SUCCEEDED(dev_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &out));
    };
    ComPtr<ID3DBlob> bWorld, bLine, bUI, bView;
    bool ok = vs("world.hlsl", "VSShadow", vsShadow) && vs("world.hlsl", "VSWorld", vsWorld, &bWorld) &&
              vs("sky.hlsl", "VSSky", vsSky) && vs("line.hlsl", "VSLine", vsLine, &bLine) && vs("ui.hlsl", "VSUI", vsUI, &bUI) &&
              vs("viewmodel.hlsl", "VSView", vsView, &bView) && ps("world.hlsl", "PSWorld", psWorld) &&
              ps("world.hlsl", "PSWater", psWater) && ps("sky.hlsl", "PSUnderwater", psUnder) && ps("sky.hlsl", "PSBlit", psBlit) && ps("sky.hlsl", "PSSky", psSky) && ps("line.hlsl", "PSLine", psLine) &&
              ps("ui.hlsl", "PSUI", psUI) && ps("viewmodel.hlsl", "PSView", psView) && ps("viewmodel.hlsl", "PSFlash", psFlash);
    if (!ok) return false;

    const D3D11_INPUT_ELEMENT_DESC worldDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"ATTR", 0, DXGI_FORMAT_R32_UINT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const D3D11_INPUT_ELEMENT_DESC lineDesc[] = {{"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0}};
    const D3D11_INPUT_ELEMENT_DESC uiDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    const D3D11_INPUT_ELEMENT_DESC viewDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(dev_->CreateInputLayout(worldDesc, 2, bWorld->GetBufferPointer(), bWorld->GetBufferSize(), &layWorld)) ||
        FAILED(dev_->CreateInputLayout(lineDesc, 1, bLine->GetBufferPointer(), bLine->GetBufferSize(), &layLine)) ||
        FAILED(dev_->CreateInputLayout(uiDesc, 3, bUI->GetBufferPointer(), bUI->GetBufferSize(), &layUI)) ||
        FAILED(dev_->CreateInputLayout(viewDesc, 3, bView->GetBufferPointer(), bView->GetBufferSize(), &layView))) {
        lastError = "input layout creation failed";
        return false;
    }
    vsShadow_ = vsShadow; vsWorld_ = vsWorld; vsSky_ = vsSky; vsLine_ = vsLine; vsUI_ = vsUI; vsView_ = vsView;
    psWorld_ = psWorld; psWater_ = psWater; psUnder_ = psUnder; psBlit_ = psBlit; psSky_ = psSky; psLine_ = psLine; psUI_ = psUI; psView_ = psView; psFlash_ = psFlash;
    layWorld_ = layWorld; layLine_ = layLine; layUI_ = layUI; layView_ = layView;

    if (initial) {
        // Rifle geometry buffers.
        D3D11_BUFFER_DESC bd = {};
        bd.Usage = D3D11_USAGE_IMMUTABLE;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        ViewModelMesh rf = buildRifle();
        bd.ByteWidth = (UINT)(rf.verts.size() * sizeof(VMVertex));
        D3D11_SUBRESOURCE_DATA sd = {rf.verts.data(), 0, 0};
        if (FAILED(dev_->CreateBuffer(&bd, &sd, &rifleVB_))) return false;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        bd.ByteWidth = (UINT)(rf.idx.size() * sizeof(uint32_t));
        sd.pSysMem = rf.idx.data();
        if (FAILED(dev_->CreateBuffer(&bd, &sd, &rifleIB_))) return false;
    }
    lastError.clear();
    return true;
}

bool Renderer::reloadShaders() {
    shaderDir_ = findShaderDir();
    bool ok = createShaders(false);
    logLine("shader reload: %s", ok ? "ok" : lastError.c_str());
    return ok;
}

void Renderer::createTextResources() {
    HDC screen = GetDC(nullptr);
    textDC_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = kTextW;
    bi.bmiHeader.biHeight = -kTextH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    textBmp_ = CreateDIBSection(textDC_, &bi, DIB_RGB_COLORS, &textBits_, nullptr, 0);
    SelectObject(textDC_, textBmp_);
    textFont_ = CreateFontA(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    SelectObject(textDC_, textFont_);

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = kTextW;
    td.Height = kTextH;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    dev_->CreateTexture2D(&td, nullptr, &textTex_);
    dev_->CreateShaderResourceView(textTex_.Get(), nullptr, &textSRV_);

    td.Width = td.Height = 1;
    uint32_t white = 0xFFFFFFFFu;
    D3D11_SUBRESOURCE_DATA sd = {&white, 4, 4};
    dev_->CreateTexture2D(&td, &sd, &whiteTex_);
    dev_->CreateShaderResourceView(whiteTex_.Get(), nullptr, &whiteSRV_);
}

// ------------------------------------------------------------------------------------------------
// Per-frame GPU updates
// ------------------------------------------------------------------------------------------------
void Renderer::uploadMesh(GpuMesh& gm, const MeshData& md) {
    gm.indexCount = 0;
    if (md.empty()) return;  // keep the buffers: this chunk will probably be re-meshed soon
    const UINT vbBytes = (UINT)(md.verts.size() * sizeof(MeshVertex)), ibBytes = (UINT)(md.idx.size() * sizeof(uint32_t));
    if (!gm.vb || vbBytes > gm.vbCap) {
        gm.vbCap = std::max(vbBytes + vbBytes / 2, 4096u);
        gm.vb.Reset();
        if (!createDynamicBuffer(gm.vb, gm.vbCap, D3D11_BIND_VERTEX_BUFFER)) { gm.vbCap = 0; return; }
    }
    if (!gm.ib || ibBytes > gm.ibCap) {
        gm.ibCap = std::max(ibBytes + ibBytes / 2, 4096u);
        gm.ib.Reset();
        if (!createDynamicBuffer(gm.ib, gm.ibCap, D3D11_BIND_INDEX_BUFFER)) { gm.ibCap = 0; return; }
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(gm.vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, md.verts.data(), vbBytes);
    ctx_->Unmap(gm.vb.Get(), 0);
    if (FAILED(ctx_->Map(gm.ib.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, md.idx.data(), ibBytes);
    ctx_->Unmap(gm.ib.Get(), 0);
    gm.indexCount = (UINT)md.idx.size();
}

void Renderer::uploadDynamic(ComPtr<ID3D11Buffer>& vb, ComPtr<ID3D11Buffer>& ib, UINT& indexCount, UINT& vbCap, UINT& ibCap,
                             const MeshData& md) {
    indexCount = 0;
    if (md.empty()) return;
    UINT vbBytes = (UINT)(md.verts.size() * sizeof(MeshVertex)), ibBytes = (UINT)(md.idx.size() * sizeof(uint32_t));
    if (!vb || vbBytes > vbCap) {
        vbCap = std::max(vbBytes * 2, 65536u);
        createDynamicBuffer(vb, vbCap, D3D11_BIND_VERTEX_BUFFER);
    }
    if (!ib || ibBytes > ibCap) {
        ibCap = std::max(ibBytes * 2, 65536u);
        createDynamicBuffer(ib, ibCap, D3D11_BIND_INDEX_BUFFER);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(vb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, md.verts.data(), vbBytes);
        ctx_->Unmap(vb.Get(), 0);
    }
    if (SUCCEEDED(ctx_->Map(ib.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, md.idx.data(), ibBytes);
        ctx_->Unmap(ib.Get(), 0);
    }
    indexCount = (UINT)md.idx.size();
}

void Renderer::updateWorldGpu(Game& g) {
    World& w = g.world;
    const int n = w.chunkCount();
    const double t0 = nowMs();

    // Work is done in parallel batches until the frame's world-update budget (~6 ms) is spent; chunks that are
    // still dirty simply wait for the next frame, so even a half-million-voxel pour cannot stall the frame.
    int rebuilds = 0, fullRebuilds = 0;
    int cursor = rebuildCursor_;
    int scanned = 0;
    const double budgetMs = firstBuild_ ? 1e9 : 3.5;  // small slices: a big edit spreads over a few frames instead of one long hitch
    while (scanned < n && nowMs() - t0 < budgetMs) {
        size_t count = 0;
        const size_t batch = firstBuild_ ? (size_t)n : 8;
        while (scanned < n && count < batch) {
            const int ci = cursor;
            cursor = (cursor + 1) % n;
            scanned++;
            const bool tex = w.texDirty[ci] != 0, mesh = w.meshDirty[ci] != 0, wmesh = !mesh && w.waterDirty[ci] != 0;
            if (!tex && !mesh && !wmesh) continue;
            // Chunks where only water moved are refreshed every other frame (30 Hz is plenty for a flowing surface).
            if (!mesh && ((unsigned)ci + (unsigned)frameIndex_) % 2u) continue;
            if (work_.size() <= count) work_.emplace_back();
            ChunkWork& cw = work_[count++];
            cw.ci = ci;
            cw.tex = tex;
            cw.mesh = mesh;
            cw.waterOnly = wmesh;
        }
        if (!count) break;

        {  // brick occupancy + greedy meshing on all cores (the world is read-only here)
            prof::Scope sc(prof::P_MESH);
            auto job = [&](int k, int) {
                ChunkWork& cw = work_[(size_t)k];
                if (cw.tex) computeBricks(w, cw.ci, cw.bricks);
                if (cw.mesh || cw.waterOnly) {
                    const int cx = cw.ci % w.CX, cy = (cw.ci / w.CX) % w.CY, cz = cw.ci / (w.CX * w.CY);
                    if (cw.mesh) buildChunkMesh(w, cx, cy, cz, cw.opaque, cw.water);
                    else buildChunkWaterMesh(w, cx, cy, cz, cw.water);
                    waterTopsOfMesh(cw.water, cx, cy, cz, cw.tops);
                }
            };
            if (count < 3) {
                for (size_t k = 0; k < count; k++) job((int)k, 0);
            } else {
                globalPool().parallelFor((int)count, job);
            }
        }
        prof::Scope sc(prof::P_UPLOAD);  // immediate context: serial
        for (size_t k = 0; k < count; k++) {
            ChunkWork& cw = work_[k];
            const int cx = cw.ci % w.CX, cy = (cw.ci / w.CX) % w.CY, cz = cw.ci / (w.CX * w.CY);
            if (cw.tex) {
                D3D11_BOX box = {(UINT)cx * 32, (UINT)cy * 32, (UINT)cz * 32, (UINT)cx * 32 + 32, (UINT)cy * 32 + 32, (UINT)cz * 32 + 32};
                ctx_->UpdateSubresource(occTex_.Get(), 0, &box, w.chunkData(cw.ci), 32, 1024);
                D3D11_BOX bb = {(UINT)cx * 4, (UINT)cy * 4, (UINT)cz * 4, (UINT)cx * 4 + 4, (UINT)cy * 4 + 4, (UINT)cz * 4 + 4};
                ctx_->UpdateSubresource(brickTex_.Get(), 0, &bb, cw.bricks, 4, 16);
                w.texDirty[cw.ci] = 0;
            }
            if (cw.mesh || cw.waterOnly) {
                if (cw.mesh) uploadMesh(chunkOpaque_[cw.ci], cw.opaque);
                uploadMesh(chunkWater_[cw.ci], cw.water);
                uint8_t* tops = &chunkTops_[(size_t)cw.ci * 1024];
                if (std::memcmp(tops, cw.tops, 1024) != 0) {
                    std::memcpy(tops, cw.tops, 1024);
                    topsDirty_ = true;
                    topsX0_ = std::min(topsX0_, cx * 32);
                    topsZ0_ = std::min(topsZ0_, cz * 32);
                    topsX1_ = std::max(topsX1_, cx * 32 + 31);
                    topsZ1_ = std::max(topsZ1_, cz * 32 + 31);
                }
                if (cw.mesh) {
                    w.meshDirty[cw.ci] = 0;
                    fullRebuilds++;
                }
                w.waterDirty[cw.ci] = 0;
                rebuilds++;
            }
        }
    }
    rebuildCursor_ = cursor;
    if (fullRebuilds) sceneDirty_ = true;  // water is not in the shadow map: water-only rebuilds leave it alone
    firstBuild_ = false;
    stats.chunkRebuilds = rebuilds;
    stats.meshMs = nowMs() - t0;
}

// Combines the per-chunk water surface maps into one per-column map for the WaterField (a few times per second at most
// while the water is changing; only the chunk columns that changed) and uploads the changed part of the field's texture.
void Renderer::updateWaterField(Game& g) {
    prof::Scope sc(prof::P_UPLOAD);
    if (topsDirty_ && (++waterMapTick_ % 3) == 0) {
        topsDirty_ = false;
        const World& w = g.world;
        constexpr int N = World::CS;
        const int rx0 = topsX0_, rz0 = topsZ0_, rx1 = topsX1_, rz1 = topsZ1_;
        topsX0_ = topsZ0_ = 1 << 20;
        topsX1_ = topsZ1_ = -1;
        for (int cz = rz0 / N; cz <= rz1 / N; cz++)
            for (int cx = rx0 / N; cx <= rx1 / N; cx++)
                for (int lz = 0; lz < N; lz++) {
                    uint16_t* out = &waterTopMap_[(size_t)(cz * N + lz) * WaterField::W + (size_t)cx * N];
                    for (int lx = 0; lx < N; lx++) {
                        uint16_t top = 0;
                        for (int cy = w.CY - 1; cy >= 0 && !top; cy--) {
                            const uint8_t t = chunkTops_[(size_t)w.chunkIndex(cx, cy, cz) * 1024 + (size_t)(lx + N * lz)];
                            if (t) top = (uint16_t)(cy * N + t);
                        }
                        out[lx] = top;
                    }
                }
        g.waterField.setSurface(waterTopMap_.data(), rx0, rz0, rx1, rz1);
    }
    int x0, z0, x1, z1;
    if (g.waterField.consumeDirty(x0, z0, x1, z1)) {
        const D3D11_BOX box = {(UINT)x0, (UINT)z0, 0, (UINT)x1 + 1, (UINT)z1 + 1, 1};
        const uint16_t* src = g.waterField.texels() + ((size_t)z0 * WaterField::W + (size_t)x0) * 4;
        ctx_->UpdateSubresource(waterFieldTex_.Get(), 0, &box, src, WaterField::W * 8, 0);
    }
}

void Renderer::updateBodies(Game& g) {
    prof::Scope sc(prof::P_BODIES);
    for (auto& kv : bodyMeshes_) kv.second.seen = false;
    static MeshData md;
    for (auto& up : g.physics.bodies) {
        const Body& b = *up;
        if (b.dead) continue;
        BodyGpu& bg = bodyMeshes_[b.id];
        bg.seen = true;
        if (bg.version != b.version) {
            buildBodyMesh(b, md);
            uploadMesh(bg.mesh, md);
            bg.version = b.version;
            sceneDirty_ = true;
        }
    }
    for (auto it = bodyMeshes_.begin(); it != bodyMeshes_.end();) {
        if (!it->second.seen) {
            it = bodyMeshes_.erase(it);
            sceneDirty_ = true;
        } else {
            ++it;
        }
    }
}

void Renderer::setObject(const Mat4& model, float r, float g, float b, float a, float volumeAO) {
    ObjectCB cb;
    cb.model = model;
    cb.tint[0] = r; cb.tint[1] = g; cb.tint[2] = b; cb.tint[3] = a;
    cb.objParams[0] = volumeAO;
    cb.objParams[1] = cb.objParams[2] = cb.objParams[3] = 0;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(cbObject_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof(cb));
        ctx_->Unmap(cbObject_.Get(), 0);
    }
}

// ------------------------------------------------------------------------------------------------
// Drawing helpers
// ------------------------------------------------------------------------------------------------
void Renderer::drawSky() {
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vsSky_.Get(), nullptr, 0);
    ctx_->PSSetShader(psSky_.Get(), nullptr, 0);
    ctx_->OMSetDepthStencilState(dsSky_.Get(), 0);
    ctx_->RSSetState(rsNoCull_.Get());
    ctx_->Draw(3, 0);
    ctx_->RSSetState(rsSolid_.Get());
}

void Renderer::drawChunks(bool water, const Frustum* fr) {
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(MeshVertex), offset = 0;
    const std::vector<GpuMesh>& meshes = water ? chunkWater_ : chunkOpaque_;
    const int CXn = cfg::WORLD_X / 32, CYn = cfg::WORLD_Y / 32;
    int drawn = 0;
    for (size_t ci = 0; ci < meshes.size(); ci++) {
        const GpuMesh& gm = meshes[ci];
        if (!gm.indexCount) continue;
        if (fr) {
            const int cx = (int)ci % CXn, cy = ((int)ci / CXn) % CYn, cz = (int)ci / (CXn * CYn);
            const Vec3 mn((float)cx * 32, (float)cy * 32, (float)cz * 32);
            if (!aabbVisible(*fr, mn, mn + Vec3(32, 32, 32))) continue;
        }
        ctx_->IASetVertexBuffers(0, 1, gm.vb.GetAddressOf(), &stride, &offset);
        ctx_->IASetIndexBuffer(gm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx_->DrawIndexed(gm.indexCount, 0, 0);
        drawn++;
    }
    if (!water && fr) stats.chunksDrawn = drawn;
}

void Renderer::drawBodies(const Game& g) {
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT stride = sizeof(MeshVertex), offset = 0;
    int n = 0;
    for (auto& up : g.physics.bodies) {
        const Body& b = *up;
        auto it = bodyMeshes_.find(b.id);
        if (b.dead || it == bodyMeshes_.end() || !it->second.mesh.indexCount) continue;
        // The mesh lives in lattice space; this matrix maps lattice -> world.
        const Mat4 m = Mat4::fromRT(b.rot.toMat3(), b.pos) * Mat4::translation(-b.com);
        setObject(m, 1, 1, 1, 1);
        ctx_->IASetVertexBuffers(0, 1, it->second.mesh.vb.GetAddressOf(), &stride, &offset);
        ctx_->IASetIndexBuffer(it->second.mesh.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx_->DrawIndexed(it->second.mesh.indexCount, 0, 0);
        n++;
    }
    stats.bodiesDrawn = n;
}

void Renderer::drawParticles() {
    if (!partIndexCount_) return;
    const UINT stride = sizeof(MeshVertex), offset = 0;
    setObject(Mat4::identity(), 1, 1, 1, 1);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetVertexBuffers(0, 1, partVB_.GetAddressOf(), &stride, &offset);
    ctx_->IASetIndexBuffer(partIB_.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx_->DrawIndexed(partIndexCount_, 0, 0);
}

// Wireframe sphere: the Creator's brush indicator (material coloured) or the Gun's carve preview (red).
void Renderer::drawBrush(const Game& g) {
    if (!g.preview.valid) return;
    const Vec3 c = g.preview.center;
    const float r = g.preview.radius + 0.5f;
    float col[3] = {1.0f, 0.25f, 0.2f};
    if (g.tool == TOOL_CREATOR) {
        switch (g.currentMaterial()) {
            case M_SAND: col[0] = 0.98f; col[1] = 0.82f; col[2] = 0.42f; break;
            case M_WATER: col[0] = 0.35f; col[1] = 0.65f; col[2] = 1.0f; break;
            case M_STONE: col[0] = 0.92f; col[1] = 0.92f; col[2] = 0.96f; break;
            case M_DIRT: col[0] = 0.62f; col[1] = 0.42f; col[2] = 0.24f; break;
            case M_GRASS: col[0] = 0.35f; col[1] = 0.85f; col[2] = 0.25f; break;
            default: col[0] = 0.72f; col[1] = 0.45f; col[2] = 1.0f; break;
        }
    }
    static std::vector<Vec3> pts;
    pts.clear();
    const int N = 56;
    for (int axis = 0; axis < 3; axis++)
        for (int i = 0; i < N; i++) {
            const float a0 = 2 * kPi * i / N, a1 = 2 * kPi * (i + 1) / N;
            auto P = [&](float a) {
                const float s = std::sin(a) * r, k = std::cos(a) * r;
                return axis == 0 ? Vec3(0, s, k) : axis == 1 ? Vec3(s, 0, k) : Vec3(s, k, 0);
            };
            pts.push_back(c + P(a0));
            pts.push_back(c + P(a1));
        }
    for (int a = 0; a < 3; a++) {  // small cross at the centre
        Vec3 d(0, 0, 0);
        d[a] = 1.0f;
        pts.push_back(c - d);
        pts.push_back(c + d);
    }
    if (pts.size() * sizeof(Vec3) > lineCap_) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(lineVB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, pts.data(), pts.size() * sizeof(Vec3));
    ctx_->Unmap(lineVB_.Get(), 0);

    const UINT stride = sizeof(Vec3), offset = 0;
    ctx_->IASetInputLayout(layLine_.Get());
    ctx_->IASetVertexBuffers(0, 1, lineVB_.GetAddressOf(), &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ctx_->VSSetShader(vsLine_.Get(), nullptr, 0);
    ctx_->PSSetShader(psLine_.Get(), nullptr, 0);
    ctx_->RSSetState(rsNoCull_.Get());
    ctx_->OMSetBlendState(bsAlpha_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(dsLessNoWrite_.Get(), 0);
    setObject(Mat4::identity(), col[0], col[1], col[2], 0.95f);
    ctx_->Draw((UINT)pts.size(), 0);
    ctx_->OMSetDepthStencilState(dsOff_.Get(), 0);  // faint x-ray pass so the brush is visible behind geometry
    setObject(Mat4::identity(), col[0], col[1], col[2], 0.20f);
    ctx_->Draw((UINT)pts.size(), 0);
    ctx_->OMSetBlendState(bsOpaque_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void Renderer::drawViewModel(const Game& g) {
    if (g.tool != TOOL_GUN || !rifleVB_) return;
    ctx_->ClearDepthStencilView(msaaDSV_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
    const float aspect = (float)width_ / (float)height_;
    const Mat4 vproj = Mat4::perspective(deg2rad(56.0f), aspect, 0.02f, 8.0f);

    const float sp = clampf(speedForBob / cfg::SPRINT_SPEED, 0.0f, 1.0f);
    const Vec3 bob(std::sin(bobTime_) * 0.0045f * sp, -std::fabs(std::sin(bobTime_)) * 0.0045f * sp, 0);
    const float rec = g.recoil;
    const Mat4 rx = Mat4::fromRT(Quat::fromAxisAngle(Vec3(1, 0, 0), 0.055f * rec).toMat3(), Vec3(0, 0, 0));
    const Mat4 ry = Mat4::fromRT(Quat::fromAxisAngle(Vec3(0, 1, 0), 0.025f).toMat3(), Vec3(0, 0, 0));
    const Mat4 model = Mat4::translation(Vec3(0.205f, -0.215f, -0.33f) + bob + Vec3(0, 0.004f * rec, 0.04f * rec)) * rx * ry;

    VMCB cb;
    cb.mvp = vproj * model;
    cb.modelView = model;
    const Vec3 L(std::cos(sunElevation) * std::sin(sunAzimuth), std::sin(sunElevation), std::cos(sunElevation) * std::cos(sunAzimuth));
    cb.lightDirView[0] = dot(L, camRight_);
    cb.lightDirView[1] = dot(L, camUp_);
    cb.lightDirView[2] = -dot(L, camFwd_);
    cb.lightDirView[3] = 0.95f;
    cb.ambientCol[0] = 0.40f; cb.ambientCol[1] = 0.43f; cb.ambientCol[2] = 0.50f; cb.ambientCol[3] = 1;
    const float fl = clampf(g.muzzleFlash / 0.06f, 0.0f, 1.0f);
    cb.flashCol[0] = 1.0f; cb.flashCol[1] = 0.72f; cb.flashCol[2] = 0.35f; cb.flashCol[3] = fl;
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ctx_->Map(cbVM_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &cb, sizeof(cb));
        ctx_->Unmap(cbVM_.Get(), 0);
    }
    ctx_->VSSetConstantBuffers(2, 1, cbVM_.GetAddressOf());
    ctx_->PSSetConstantBuffers(2, 1, cbVM_.GetAddressOf());

    const UINT stride = sizeof(VMVertex), offset = 0;
    ctx_->IASetInputLayout(layView_.Get());
    ctx_->IASetVertexBuffers(0, 1, rifleVB_.GetAddressOf(), &stride, &offset);
    ctx_->IASetIndexBuffer(rifleIB_.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vsView_.Get(), nullptr, 0);
    ctx_->PSSetShader(psView_.Get(), nullptr, 0);
    ctx_->RSSetState(rsSolid_.Get());
    ctx_->OMSetDepthStencilState(dsLess_.Get(), 0);
    ctx_->OMSetBlendState(bsOpaque_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->DrawIndexed(rifle_.flashFirstIndex, 0, 0);
    if (fl > 0.01f) {
        ctx_->PSSetShader(psFlash_.Get(), nullptr, 0);
        ctx_->RSSetState(rsNoCull_.Get());
        ctx_->OMSetBlendState(bsAdd_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->OMSetDepthStencilState(dsLessNoWrite_.Get(), 0);
        ctx_->DrawIndexed((UINT)rifle_.idx.size() - rifle_.flashFirstIndex, rifle_.flashFirstIndex, 0);
        ctx_->OMSetBlendState(bsOpaque_.Get(), nullptr, 0xFFFFFFFF);
    }
}

// ------------------------------------------------------------------------------------------------
// HUD
// ------------------------------------------------------------------------------------------------
void Renderer::buildHudText(const Game& g, std::vector<std::string>& hud, std::vector<std::string>& help) {
    char b[256];
    snprintf(b, sizeof(b), "VOXEL SANDFALL  v0.0.1                 %3.0f FPS", fpsShown_);
    hud.push_back(b);
    if (g.tool == TOOL_CREATOR) {
        hud.push_back("TOOL      [R] CREATOR  -  place / attach voxels");
        snprintf(b, sizeof(b), "MATERIAL  %s   (1 sand  2 water  3 stone  4 permanent  5 dirt  6 grass)", materialName(g.currentMaterial()));
        hud.push_back(b);
    } else {
        hud.push_back("TOOL      [F] GUN  -  carve voxels, split islands");
        hud.push_back("MATERIAL  -");
    }
    snprintf(b, sizeof(b), "BRUSH     radius %d   (mouse wheel)", g.brush);
    hud.push_back(b);
    size_t awake = g.physics.awakeCount();
    snprintf(b, sizeof(b), "BODIES    %zu (%zu awake)   SAND %lld  WATER %lld  DIRT %lld  GRASS %lld", g.physics.bodies.size(), awake, g.world.counts[M_SAND],
             g.world.counts[M_WATER], g.world.counts[M_DIRT], g.world.counts[M_GRASS]);
    hud.push_back(b);
    snprintf(b, sizeof(b), "RENDER    quality %s tier %d (%.0f%% res, %d rays, water %d)   chunks %d   meshing %.1f ms", quality < 0 ? "auto" : "fixed", tier_, renderScale_ * 100.0f, kTiers[tier_].rays, kTiers[tier_].water, stats.chunksDrawn, stats.meshMs);
    hud.push_back(b);

    if (showProfiler) {
        hud.push_back("--- profiler (ms, smoothed) ---");
        double cpu = 0;
        for (int i = 0; i < prof::P_COUNT; i++) {
            if (i != prof::P_PRESENT) cpu += prof::g_avg[i];
            snprintf(b, sizeof(b), "%-18s %6.2f", prof::kNames[i], prof::g_avg[i]);
            hud.push_back(b);
        }
        snprintf(b, sizeof(b), "%-18s %6.2f", "CPU total (no present)", cpu);
        hud.push_back(b);
        for (int i = 0; i < prof::G_COUNT; i++) {
            snprintf(b, sizeof(b), "gpu %-14s %6.2f", prof::kGpuNames[i], prof::g_gpuAvg[i]);
            hud.push_back(b);
        }
    }
    if (showHelp) {
        help.push_back("WASD move   SPACE jump   SHIFT sprint   MOUSE look   ESC release mouse");
        help.push_back("R creator   F gun   1-4 or Q/E material   WHEEL brush size   LMB paint / fire");
        help.push_back("[ ] sun azimuth   ; ' sun height   F1 help   F2 screenshot   F3 HUD");
        help.push_back("F4 profiler   F5 reset world   F6 sky rays   F7 vsync   F8 MSAA   F9 reload shaders   F12 quality (auto / 0-4)");
    }
}

void Renderer::drawHud(const Game& g) {
    std::vector<std::string> hud, help;
    buildHudText(g, hud, help);
    const int lineH = 24;

    std::string key;
    for (auto& s : hud) key += s + "\n";
    key += "|";
    for (auto& s : help) key += s + "\n";
    if (key != textKey_) {
        textKey_ = key;
        PatBlt(textDC_, 0, 0, kTextW, kTextH, BLACKNESS);
        SetBkMode(textDC_, TRANSPARENT);
        SetTextColor(textDC_, RGB(255, 255, 255));
        textLines_.clear();
        int y = 0;
        auto put = [&](const std::string& s) {
            TextOutA(textDC_, 0, y, s.c_str(), (int)s.size());
            SIZE sz = {0, 0};
            GetTextExtentPoint32A(textDC_, s.c_str(), (int)s.size(), &sz);
            textLines_.push_back({y, (int)sz.cx, (int)std::max<LONG>(sz.cy, 18)});
            y += lineH;
        };
        for (auto& s : hud) put(s);
        for (auto& s : help) put(s);
        GdiFlush();
        static std::vector<uint32_t> conv;
        conv.resize((size_t)kTextW * kTextH);
        const uint32_t* src = (const uint32_t*)textBits_;
        for (size_t i = 0; i < conv.size(); i++) conv[i] = 0x00FFFFFFu | (((src[i] >> 8) & 0xFF) << 24);
        ctx_->UpdateSubresource(textTex_.Get(), 0, nullptr, conv.data(), kTextW * 4, 0);
    }

    static std::vector<UIVertex> solid, text;
    solid.clear();
    text.clear();
    auto quad = [&](std::vector<UIVertex>& v, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
        v.push_back({x0, y0, u0, v0, c});
        v.push_back({x1, y0, u1, v0, c});
        v.push_back({x1, y1, u1, v1, c});
        v.push_back({x0, y0, u0, v0, c});
        v.push_back({x1, y1, u1, v1, c});
        v.push_back({x0, y1, u0, v1, c});
    };
    auto rect = [&](float x0, float y0, float x1, float y1, uint32_t c) { quad(solid, x0, y0, x1, y1, 0, 0, 1, 1, c); };
    auto textAt = [&](size_t li, float x, float y, uint32_t c) {
        const TextLine& tl = textLines_[li];
        const float u1 = (float)tl.w / kTextW, v0 = (float)tl.y / kTextH, v1 = (float)(tl.y + tl.h) / kTextH;
        quad(text, x + 1, y + 1, x + 1 + tl.w, y + 1 + tl.h, 0, v0, u1, v1, packColor(0, 0, 0, 0.9f));
        quad(text, x, y, x + tl.w, y + tl.h, 0, v0, u1, v1, c);
    };

    const float W = (float)width_, H = (float)height_;
    // Crosshair.
    {
        const float cx = std::floor(W * 0.5f), cy = std::floor(H * 0.5f);
        const uint32_t white = packColor(1, 1, 1, 0.92f), dark = packColor(0, 0, 0, 0.55f);
        const float gap = 5, len = 9, th = 2;
        auto bars = [&](float grow, uint32_t c) {
            rect(cx - gap - len - grow, cy - th * 0.5f - grow, cx - gap + grow, cy + th * 0.5f + grow, c);
            rect(cx + gap - grow, cy - th * 0.5f - grow, cx + gap + len + grow, cy + th * 0.5f + grow, c);
            rect(cx - th * 0.5f - grow, cy - gap - len - grow, cx + th * 0.5f + grow, cy - gap + grow, c);
            rect(cx - th * 0.5f - grow, cy + gap - grow, cx + th * 0.5f + grow, cy + gap + len + grow, c);
        };
        bars(1, dark);
        bars(0, white);
        rect(cx - 1, cy - 1, cx + 1, cy + 1, packColor(1.0f, 0.35f, 0.25f, 0.95f));
    }
    // Info panel.
    float maxW = 0;
    for (size_t i = 0; i < hud.size() && i < textLines_.size(); i++) maxW = std::max(maxW, (float)textLines_[i].w);
    rect(8, 8, 26 + maxW, 16 + (float)hud.size() * lineH, packColor(0.02f, 0.03f, 0.05f, 0.45f));
    for (size_t i = 0; i < hud.size(); i++) textAt(i, 18, 14 + (float)i * lineH, i == 0 ? packColor(1.0f, 0.86f, 0.45f, 1) : packColor(0.95f, 0.97f, 1.0f, 1));
    // Help.
    if (!help.empty()) {
        float hw = 0;
        for (size_t i = 0; i < help.size(); i++) hw = std::max(hw, (float)textLines_[hud.size() + i].w);
        const float y0 = H - 22 - (float)help.size() * lineH;
        rect(8, y0 - 6, 26 + hw, H - 12, packColor(0.02f, 0.03f, 0.05f, 0.45f));
        for (size_t i = 0; i < help.size(); i++) textAt(hud.size() + i, 18, y0 + (float)i * lineH, packColor(0.82f, 0.88f, 0.98f, 1));
    }
    // Material swatches (Creator).
    if (g.tool == TOOL_CREATOR) {
        const float sw = 46, gapx = 10, total = (float)kPaintMaterialCount * sw + (float)(kPaintMaterialCount - 1) * gapx;
        const float x0 = W * 0.5f - total * 0.5f, y0 = H - 78;
        const uint32_t cols[kPaintMaterialCount] = {packColor(0.87f, 0.71f, 0.40f, 1), packColor(0.18f, 0.47f, 0.84f, 1), packColor(0.58f, 0.58f, 0.62f, 1),
                                                    packColor(0.28f, 0.22f, 0.45f, 1), packColor(0.45f, 0.30f, 0.17f, 1), packColor(0.25f, 0.62f, 0.18f, 1)};
        for (int i = 0; i < kPaintMaterialCount; i++) {
            const float x = x0 + (float)i * (sw + gapx);
            const bool sel = (i == g.materialIndex);
            const float pad = sel ? 4.0f : 2.0f;
            rect(x - pad, y0 - pad, x + sw + pad, y0 + sw + pad, sel ? packColor(1, 1, 1, 0.95f) : packColor(0, 0, 0, 0.5f));
            rect(x, y0, x + sw, y0 + sw, cols[i]);
        }
    }

    const size_t total = solid.size() + text.size();
    if (total * sizeof(UIVertex) > uiCap_) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(uiVB_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, solid.data(), solid.size() * sizeof(UIVertex));
    memcpy((uint8_t*)m.pData + solid.size() * sizeof(UIVertex), text.data(), text.size() * sizeof(UIVertex));
    ctx_->Unmap(uiVB_.Get(), 0);

    UICB ucb = {{W, H}, {0, 0}};
    if (SUCCEEDED(ctx_->Map(cbUI_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &ucb, sizeof(ucb));
        ctx_->Unmap(cbUI_.Get(), 0);
    }
    D3D11_VIEWPORT vp = {0, 0, W, H, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, backRTV_.GetAddressOf(), nullptr);
    const UINT stride = sizeof(UIVertex), offset = 0;
    ctx_->IASetInputLayout(layUI_.Get());
    ctx_->IASetVertexBuffers(0, 1, uiVB_.GetAddressOf(), &stride, &offset);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vsUI_.Get(), nullptr, 0);
    ctx_->PSSetShader(psUI_.Get(), nullptr, 0);
    ctx_->VSSetConstantBuffers(0, 1, cbUI_.GetAddressOf());
    ctx_->PSSetSamplers(0, 1, sampLinear_.GetAddressOf());
    ctx_->RSSetState(rsNoCull_.Get());
    ctx_->OMSetBlendState(bsAlpha_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(dsOff_.Get(), 0);
    ctx_->PSSetShaderResources(0, 1, whiteSRV_.GetAddressOf());
    if (!solid.empty()) ctx_->Draw((UINT)solid.size(), 0);
    ctx_->PSSetShaderResources(0, 1, textSRV_.GetAddressOf());
    if (!text.empty()) ctx_->Draw((UINT)text.size(), (UINT)solid.size());
    ctx_->OMSetBlendState(bsOpaque_.Get(), nullptr, 0xFFFFFFFF);
}

void Renderer::saveScreenshot(const std::string& path) {
    D3D11_TEXTURE2D_DESC d;
    backTex_->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> stg;
    if (FAILED(dev_->CreateTexture2D(&d, nullptr, &stg))) return;
    ctx_->CopyResource(stg.Get(), backTex_.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx_->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    std::vector<uint8_t> rgba((size_t)d.Width * d.Height * 4);
    for (UINT y = 0; y < d.Height; y++) memcpy(&rgba[(size_t)y * d.Width * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)d.Width * 4);
    ctx_->Unmap(stg.Get(), 0);
    bool ok = writePng(path.c_str(), (int)d.Width, (int)d.Height, rgba.data());
    logLine("screenshot %s: %s (%ux%u)", path.c_str(), ok ? "saved" : "FAILED", d.Width, d.Height);
}

// ------------------------------------------------------------------------------------------------
// GPU timestamps
// ------------------------------------------------------------------------------------------------
void Renderer::gpuBegin() {
    const int s = (int)(frameIndex_ % kGpuRing);
    // Collect whatever has finished (non-blocking); the slot of this frame is only reused once its old data was read.
    for (int r = 0; r < kGpuRing; r++) {
        if (!qUsed_[r] || r == s) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
        if (ctx_->GetData(qDisjoint_[r].Get(), &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t[5];
        bool ok = !dj.Disjoint;
        for (int i = 0; i < 5 && ok; i++) ok = ctx_->GetData(qStamp_[r][i].Get(), &t[i], sizeof(UINT64), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (ok) {
            const double k = 1000.0 / (double)dj.Frequency;
            prof::setGpu(prof::G_SHADOW, (double)(t[1] - t[0]) * k);
            prof::setGpu(prof::G_MAIN, (double)(t[2] - t[1]) * k);
            prof::setGpu(prof::G_WATER, (double)(t[3] - t[2]) * k);
            prof::setGpu(prof::G_POST, (double)(t[4] - t[3]) * k);
            prof::setGpu(prof::G_TOTAL, (double)(t[4] - t[0]) * k);
        }
        qUsed_[r] = false;
    }
    gpuTiming_ = !qUsed_[s];
    if (!gpuTiming_) return;
    if (!qDisjoint_[s]) {
        D3D11_QUERY_DESC qd = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        dev_->CreateQuery(&qd, &qDisjoint_[s]);
        qd.Query = D3D11_QUERY_TIMESTAMP;
        for (int i = 0; i < 5; i++) dev_->CreateQuery(&qd, &qStamp_[s][i]);
    }
    if (qDisjoint_[s]) {
        ctx_->Begin(qDisjoint_[s].Get());
        ctx_->End(qStamp_[s][0].Get());
    }
}

void Renderer::gpuStamp(int i) {
    const int s = (int)(frameIndex_ % kGpuRing);
    if (gpuTiming_ && qStamp_[s][i]) ctx_->End(qStamp_[s][i].Get());
}

void Renderer::gpuEnd() {
    const int s = (int)(frameIndex_ % kGpuRing);
    if (gpuTiming_ && qDisjoint_[s]) {
        ctx_->End(qStamp_[s][4].Get());
        ctx_->End(qDisjoint_[s].Get());
        qUsed_[s] = true;
    }
}

// ------------------------------------------------------------------------------------------------
// Frame
// ------------------------------------------------------------------------------------------------
void Renderer::render(Game& g, float dt) {
    if (!swap_ || width_ <= 0 || height_ <= 0 || !vsWorld_) return;
    const double tFrame = nowMs();
    frameIndex_++;
    fpsAccum_ += dt;
    fpsFrames_++;
    if (fpsAccum_ >= 0.5) {
        fpsShown_ = (float)(fpsFrames_ / fpsAccum_);
        fpsAccum_ = 0;
        fpsFrames_ = 0;
    }
    stats.fps = fpsShown_;
    bobTime_ += dt * (5.0f + 7.0f * clampf(speedForBob / cfg::SPRINT_SPEED, 0, 1));

    updateWorldGpu(g);
    updateWaterField(g);
    updateBodies(g);
    {
        static MeshData pm;
        pm.clear();
        castingParticles_ = false;
        for (const Particle& p : g.particles) {
            appendCube(pm, p.pos, p.size, p.mat);
            castingParticles_ |= p.mat != M_WATER;  // water droplets cast no shadow
        }
        uploadDynamic(partVB_, partIB_, partIndexCount_, partVBCap_, partIBCap_, pm);
    }

    // --- camera ---------------------------------------------------------------------------------
    eye_ = g.player.eye();
    camFwd_ = g.player.forwardDir();
    camRight_ = g.player.rightDir();
    if (camOverride) {
        eye_ = camOverridePos;
        const float cp = std::cos(camOverridePitch);
        camFwd_ = Vec3(-std::sin(camOverrideYaw) * cp, std::sin(camOverridePitch), -std::cos(camOverrideYaw) * cp);
        camRight_ = Vec3(std::cos(camOverrideYaw), 0, -std::sin(camOverrideYaw));
    }
    camUp_ = normalize(cross(camRight_, camFwd_));
    const float aspect = (float)width_ / (float)height_;
    view_ = Mat4::lookAt(eye_, eye_ + camFwd_, Vec3(0, 1, 0));
    proj_ = Mat4::perspective(fovY_, aspect, 0.4f, 3000.0f);
    viewProj_ = proj_ * view_;

    // --- sun + shadow frustum fitted to the arena ---------------------------------------------------
    const Vec3 L(std::cos(sunElevation) * std::sin(sunAzimuth), std::sin(sunElevation), std::cos(sunElevation) * std::cos(sunAzimuth));
    {
        const Vec3 c(cfg::ARENA_OX + cfg::ARENA_TOTAL * 0.5f, 80.0f, cfg::ARENA_OZ + cfg::ARENA_TOTAL * 0.5f);
        const Vec3 up = std::fabs(L.y) > 0.95f ? Vec3(0, 0, 1) : Vec3(0, 1, 0);
        const Mat4 lv = Mat4::lookAt(c + L * 600.0f, c, up);
        const Vec3 bmin(cfg::ARENA_OX - 4.0f, -2.0f, cfg::ARENA_OZ - 4.0f);
        const Vec3 bmax(cfg::ARENA_OX + cfg::ARENA_TOTAL + 4.0f, cfg::WALL_HEIGHT + 70.0f, cfg::ARENA_OZ + cfg::ARENA_TOTAL + 4.0f);
        Vec3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
        for (int i = 0; i < 8; i++) {
            const Vec3 p((i & 1) ? bmax.x : bmin.x, (i & 2) ? bmax.y : bmin.y, (i & 4) ? bmax.z : bmin.z);
            const Vec3 q(lv.m[0][0] * p.x + lv.m[0][1] * p.y + lv.m[0][2] * p.z + lv.m[0][3],
                         lv.m[1][0] * p.x + lv.m[1][1] * p.y + lv.m[1][2] * p.z + lv.m[1][3],
                         lv.m[2][0] * p.x + lv.m[2][1] * p.y + lv.m[2][2] * p.z + lv.m[2][3]);
            lo = vmin(lo, q);
            hi = vmax(hi, q);
        }
        const Mat4 lp = Mat4::ortho(lo.x, hi.x, lo.y, hi.y, std::max(0.5f, -hi.z - 10.0f), -lo.z + 10.0f);
        lightViewProj_ = lp * lv;
    }

    FrameCB fc;
    fc.viewProj = viewProj_;
    fc.lightViewProj = lightViewProj_;
    auto set4 = [](float* d, float a, float b, float c, float e) { d[0] = a; d[1] = b; d[2] = c; d[3] = e; };
    set4(fc.camPos, eye_.x, eye_.y, eye_.z, g.time);
    set4(fc.sunDir, L.x, L.y, L.z, 2.1f);
    set4(fc.sunColor, 1.0f, 0.92f, 0.78f, 1);
    set4(fc.skyZenith, 0.30f, 0.41f, 0.64f, 1);
    set4(fc.skyHorizon, 0.56f, 0.64f, 0.80f, 1);
    set4(fc.groundCol, 0.22f, 0.19f, 0.16f, 1);
    const Tier& tr = kTiers[std::clamp(tier_, 0, 4)];
    set4(fc.params, 1.0f / (float)shadowSize_, (float)(skyRays > 0 ? std::min(skyRays, tr.rays) : 0), tr.rayLen, (float)tr.raySteps);
    set4(fc.qual, (float)tr.water, tr.shadow9 ? 1.0f : 0.0f, tr.contact ? 1.0f : 0.0f, tr.rays <= 4 ? 1.0f : 0.0f);
    set4(fc.gridDims, (float)cfg::WORLD_X, (float)cfg::WORLD_Y, (float)cfg::WORLD_Z, 0);
    set4(fc.camRight, camRight_.x, camRight_.y, camRight_.z, aspect);
    set4(fc.camUp, camUp_.x, camUp_.y, camUp_.z, std::tan(fovY_ * 0.5f));
    set4(fc.camFwd, camFwd_.x, camFwd_.y, camFwd_.z, 0);
    D3D11_MAPPED_SUBRESOURCE mp;
    if (SUCCEEDED(ctx_->Map(cbFrame_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp))) {
        memcpy(mp.pData, &fc, sizeof(fc));
        ctx_->Unmap(cbFrame_.Get(), 0);
    }

    ID3D11Buffer* cbs[2] = {cbFrame_.Get(), cbObject_.Get()};
    ctx_->VSSetConstantBuffers(0, 2, cbs);
    ctx_->PSSetConstantBuffers(0, 2, cbs);
    ID3D11ShaderResourceView* nullSrv[4] = {nullptr, nullptr, nullptr, nullptr};
    ctx_->PSSetShaderResources(0, 4, nullSrv);

    // --- 1. sun shadow map (re-rendered only when something it shows has changed) ---------------------
    const double tDraw0 = prof::nowMs();
    gpuBegin();
    const bool sunMoved = std::fabs(sunAzimuth - shadowAz_) > 1e-4f || std::fabs(sunElevation - shadowEl_) > 1e-4f;
    const bool needShadow = sceneDirty_ || sunMoved || g.physics.awakeCount() > 0 || castingParticles_;
    if (needShadow) {
        shadowAz_ = sunAzimuth;
        shadowEl_ = sunElevation;
        sceneDirty_ = false;
        ctx_->OMSetRenderTargets(0, nullptr, shadowDSV_.Get());
        ctx_->ClearDepthStencilView(shadowDSV_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT svp = {0, 0, (float)shadowSize_, (float)shadowSize_, 0, 1};
        ctx_->RSSetViewports(1, &svp);
        ctx_->RSSetState(rsShadow_.Get());
        ctx_->OMSetDepthStencilState(dsLess_.Get(), 0);
        ctx_->OMSetBlendState(bsOpaque_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->IASetInputLayout(layWorld_.Get());
        ctx_->VSSetShader(vsShadow_.Get(), nullptr, 0);
        ctx_->PSSetShader(nullptr, nullptr, 0);
        setObject(Mat4::identity(), 1, 1, 1, 1);
        drawChunks(false, nullptr);
        drawBodies(g);
        drawParticles();
    }
    gpuStamp(1);

    // --- 2. main pass (MSAA): depth pre-pass, then shade each visible pixel exactly once --------------
    D3D11_VIEWPORT vp = {0, 0, (float)sceneW_, (float)sceneH_, 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->OMSetRenderTargets(1, msaaRTV_.GetAddressOf(), msaaDSV_.Get());
    const float clear[4] = {0, 0, 0, 1};
    ctx_->ClearRenderTargetView(msaaRTV_.Get(), clear);
    ctx_->ClearDepthStencilView(msaaDSV_.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    ID3D11ShaderResourceView* srvs[4] = {shadowSRV_.Get(), occSRV_.Get(), brickSRV_.Get(), waterFieldSRV_.Get()};
    ctx_->PSSetShaderResources(0, 4, srvs);
    ID3D11SamplerState* samps[2] = {sampShadow_.Get(), sampLinear_.Get()};
    ctx_->PSSetSamplers(0, 2, samps);
    ctx_->RSSetState(rsSolid_.Get());
    ctx_->OMSetBlendState(bsOpaque_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->IASetInputLayout(layWorld_.Get());
    ctx_->VSSetShader(vsWorld_.Get(), nullptr, 0);
    const Frustum fr = extractFrustum(viewProj_);
    auto drawOpaque = [&]() {
        setObject(Mat4::identity(), 1, 1, 1, 1, 1.0f);  // chunk meshes: ambient occlusion comes from the volume
        drawChunks(false, &fr);
        drawBodies(g);
        drawParticles();
    };
    ctx_->OMSetDepthStencilState(dsLess_.Get(), 0);
    ctx_->PSSetShader(nullptr, nullptr, 0);
    drawOpaque();
    ctx_->OMSetDepthStencilState(dsEqual_.Get(), 0);
    ctx_->PSSetShader(psWorld_.Get(), nullptr, 0);
    drawOpaque();
    drawSky();
    ctx_->VSSetShader(vsWorld_.Get(), nullptr, 0);
    ctx_->IASetInputLayout(layWorld_.Get());

    gpuStamp(2);
    ctx_->PSSetShader(psWater_.Get(), nullptr, 0);
    ctx_->OMSetBlendState(bsWater_.Get(), nullptr, 0xFFFFFFFF);
    ctx_->OMSetDepthStencilState(dsLessNoWrite_.Get(), 0);
    setObject(Mat4::identity(), 1, 1, 1, 1);
    drawChunks(true, &fr);
    ctx_->OMSetBlendState(bsOpaque_.Get(), nullptr, 0xFFFFFFFF);

    // Camera under water: tint the whole view (the water surface itself is not drawn from below).
    if (g.world.get(floorToInt(eye_.x), floorToInt(eye_.y), floorToInt(eye_.z)) == M_WATER) {
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vsSky_.Get(), nullptr, 0);
        ctx_->PSSetShader(psUnder_.Get(), nullptr, 0);
        ctx_->OMSetBlendState(bsAlpha_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->OMSetDepthStencilState(dsOff_.Get(), 0);
        ctx_->RSSetState(rsNoCull_.Get());
        ctx_->Draw(3, 0);
        ctx_->RSSetState(rsSolid_.Get());
        ctx_->OMSetBlendState(bsOpaque_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->IASetInputLayout(layWorld_.Get());
        ctx_->VSSetShader(vsWorld_.Get(), nullptr, 0);
    }

    drawBrush(g);
    drawViewModel(g);
    gpuStamp(3);

    // --- 3. resolve + HUD ---------------------------------------------------------------------------
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_->PSSetShaderResources(0, 4, nullSrv);
    ID3D11Texture2D* resolveDst = sceneTex_ ? sceneTex_.Get() : backTex_.Get();
    if (sampleCount_ > 1)
        ctx_->ResolveSubresource(resolveDst, 0, msaaColor_.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    else
        ctx_->CopyResource(resolveDst, msaaColor_.Get());
    ctx_->OMSetRenderTargets(1, backRTV_.GetAddressOf(), nullptr);
    D3D11_VIEWPORT fullVp = {0, 0, (float)width_, (float)height_, 0, 1};
    ctx_->RSSetViewports(1, &fullVp);
    if (sceneTex_) {  // the scene was rendered below window resolution: upscale it
        ctx_->IASetInputLayout(nullptr);
        ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx_->VSSetShader(vsSky_.Get(), nullptr, 0);
        ctx_->PSSetShader(psBlit_.Get(), nullptr, 0);
        ctx_->OMSetDepthStencilState(dsOff_.Get(), 0);
        ctx_->OMSetBlendState(bsOpaque_.Get(), nullptr, 0xFFFFFFFF);
        ctx_->RSSetState(rsNoCull_.Get());
        ID3D11ShaderResourceView* blitSrv = sceneSRV_.Get();
        ctx_->PSSetShaderResources(4, 1, &blitSrv);
        ctx_->Draw(3, 0);
        ID3D11ShaderResourceView* noSrv = nullptr;
        ctx_->PSSetShaderResources(4, 1, &noSrv);
        ctx_->RSSetState(rsSolid_.Get());
    }
    if (showHud) drawHud(g);
    gpuEnd();
    prof::g_cur[prof::P_DRAW] += prof::nowMs() - tDraw0;

    if (!screenshotPath_.empty()) {
        saveScreenshot(screenshotPath_);
        screenshotPath_.clear();
    }
    {
        prof::Scope sc(prof::P_PRESENT);
        swap_->Present(vsync ? 1 : 0, 0);
    }
    stats.frameMs = nowMs() - tFrame;
    prof::endFrame();
    updateAutoQuality();
}
