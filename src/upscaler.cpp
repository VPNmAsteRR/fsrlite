#include "fsrlite.h"
#include <d3dcompiler.h>
#include <cstring>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

// --------------------------------------------------------------- helpers

static const wchar_t* ShaderPath()
{
    static std::wstring path;
    if (path.empty())
    {
        wchar_t exe[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            dir.resize(slash + 1);
        path = dir + L"shaders\\upscale.hlsl";
    }
    return path.c_str();
}

static ComPtr<ID3DBlob> CompileShader(const char* entry, const char* target)
{
    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompileFromFile(
        ShaderPath(), nullptr, nullptr,
        entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS,
        0, &blob, &err);

    if (FAILED(hr))
    {
        if (err)
            OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
        return nullptr;
    }
    return blob;
}

// ------------------------------------------------------------- pipelines

bool Upscaler::BuildPipelines(ID3D11Device* device)
{
    auto vsBlob = CompileShader("VSFullscreen", "vs_5_0");
    auto eaBlob = CompileShader("PSEASU", "ps_5_0");
    auto rcBlob = CompileShader("PSRCAS", "ps_5_0");
    auto cpBlob = CompileShader("PSCopy", "ps_5_0");

    if (!vsBlob || !eaBlob || !rcBlob || !cpBlob)
    {
        m_error = L"shader compilation failed - check shaders/upscale.hlsl";
        return false;
    }

    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                          vsBlob->GetBufferSize(),
                                          nullptr, &m_vs)) ||
        FAILED(device->CreatePixelShader(eaBlob->GetBufferPointer(),
                                         eaBlob->GetBufferSize(),
                                         nullptr, &m_psEasu)) ||
        FAILED(device->CreatePixelShader(rcBlob->GetBufferPointer(),
                                         rcBlob->GetBufferSize(),
                                         nullptr, &m_psRcas)) ||
        FAILED(device->CreatePixelShader(cpBlob->GetBufferPointer(),
                                         cpBlob->GetBufferSize(),
                                         nullptr, &m_psCopy)))
    {
        m_error = L"Create*Shader failed";
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;

    if (FAILED(device->CreateSamplerState(&sd, &m_smp)))
    {
        m_error = L"CreateSamplerState failed";
        return false;
    }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = 32;   // SrcSize, DstSize, Sharpness, EdgeStrength, pad
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device->CreateBuffer(&bd, nullptr, &m_cb)))
    {
        m_error = L"CreateBuffer(constants) failed";
        return false;
    }

    return true;
}

bool Upscaler::EnsureMid(ID3D11Device* device, UINT w, UINT h)
{
    if (m_mid && w == m_vpW && h == m_vpH)
        return true;

    m_mid.Reset();
    m_midSrv.Reset();
    m_midRtv.Reset();
    m_vpW = w;
    m_vpH = h;

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    HRESULT hr = device->CreateTexture2D(&td, nullptr, &m_mid);
    if (FAILED(hr))
    {
        m_error = L"CreateTexture2D(mid) failed";
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format                    = td.Format;
    srvd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels       = 1;
    srvd.Texture2D.MostDetailedMip = 0;

    if (FAILED(device->CreateShaderResourceView(m_mid.Get(), &srvd, &m_midSrv)) ||
        FAILED(device->CreateRenderTargetView(m_mid.Get(), nullptr, &m_midRtv)))
    {
        m_error = L"Create*View(mid) failed";
        return false;
    }

    return true;
}

bool Upscaler::Init(ID3D11Device* device)
{
    return BuildPipelines(device);
}

bool Upscaler::Rebuild(ID3D11Device* device, UINT srcW, UINT srcH, UINT vpW, UINT vpH)
{
    if (srcW == 0 || srcH == 0 || vpW == 0 || vpH == 0)
    {
        m_error = L"invalid upscale sizes";
        return false;
    }

    m_srcW = srcW;
    m_srcH = srcH;
    return EnsureMid(device, vpW, vpH);
}

void Upscaler::UploadConstants(ID3D11DeviceContext* ctx)
{
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(m_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
        return;

    struct
    {
        float srcSize[2];
        float dstSize[2];
        float sharpness;
        float edge;
        float pad0, pad1;
    } p{};

    p.srcSize[0] = static_cast<float>(m_srcW);
    p.srcSize[1] = static_cast<float>(m_srcH);
    p.dstSize[0] = static_cast<float>(m_vpW);
    p.dstSize[1] = static_cast<float>(m_vpH);
    p.sharpness  = m_sharpness;
    p.edge       = 6.0f;

    std::memcpy(ms.pData, &p, sizeof(p));
    ctx->Unmap(m_cb.Get(), 0);
}

static void DrawPass(ID3D11DeviceContext* ctx,
                     ID3D11VertexShader* vs,
                     ID3D11PixelShader* ps,
                     ID3D11Buffer* cb,
                     ID3D11SamplerState* smp,
                     ID3D11ShaderResourceView* src,
                     ID3D11RenderTargetView* dst,
                     const D3D11_VIEWPORT& vp)
{
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11RenderTargetView* rt[] = { dst };
    ctx->OMSetRenderTargets(1, rt, nullptr);

    D3D11_VIEWPORT v = vp;
    ctx->RSSetViewports(1, &v);

    ctx->VSSetShader(vs, nullptr, 0);
    ctx->PSSetShader(ps, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->PSSetShaderResources(0, 1, &src);

    ctx->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrv[] = { nullptr };
    ctx->PSSetShaderResources(0, 1, nullSrv);
}

void Upscaler::Process(ID3D11DeviceContext* ctx,
                       ID3D11ShaderResourceView* src,
                       ID3D11RenderTargetView* dst,
                       const D3D11_VIEWPORT& vp)
{
    UploadConstants(ctx);

    // pass 1: EASU  src -> mid, filling the whole mid texture
    D3D11_VIEWPORT midVp{};
    midVp.Width    = static_cast<float>(m_vpW);
    midVp.Height   = static_cast<float>(m_vpH);
    midVp.MaxDepth = 1.0f;

    DrawPass(ctx, m_vs.Get(), m_psEasu.Get(), m_cb.Get(), m_smp.Get(),
             src, m_midRtv.Get(), midVp);

    // pass 2: RCAS  mid -> dst, confined to the destination viewport
    DrawPass(ctx, m_vs.Get(), m_psRcas.Get(), m_cb.Get(), m_smp.Get(),
             m_midSrv.Get(), dst, vp);
}

void Upscaler::Copy(ID3D11DeviceContext* ctx,
                    ID3D11ShaderResourceView* src,
                    ID3D11RenderTargetView* dst,
                    const D3D11_VIEWPORT& vp)
{
    UploadConstants(ctx);
    DrawPass(ctx, m_vs.Get(), m_psCopy.Get(), m_cb.Get(), m_smp.Get(),
             src, dst, vp);
}
