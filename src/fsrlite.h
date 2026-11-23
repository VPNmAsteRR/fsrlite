#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

// Two-pass chain: EASU (src -> mid) then RCAS (mid -> dst).
//
// The mid texture is sized to the destination viewport, so the second pass
// samples it 1:1 and letterbox bars on the swapchain never get touched.
// Rebuild() re-targets the chain; call it when the capture size or the
// viewport size changes.
class Upscaler
{
public:
    bool Init(ID3D11Device* device);
    bool Rebuild(ID3D11Device* device, UINT srcW, UINT srcH, UINT vpW, UINT vpH);

    void Process(ID3D11DeviceContext* ctx,
                 ID3D11ShaderResourceView* src,
                 ID3D11RenderTargetView* dst,
                 const D3D11_VIEWPORT& vp);

    void Copy(ID3D11DeviceContext* ctx,
              ID3D11ShaderResourceView* src,
              ID3D11RenderTargetView* dst,
              const D3D11_VIEWPORT& vp);

    void SetSharpness(float v) { m_sharpness = v; }
    float Sharpness() const { return m_sharpness; }

    const std::wstring& Error() const { return m_error; }

private:
    bool BuildPipelines(ID3D11Device* device);
    bool EnsureMid(ID3D11Device* device, UINT w, UINT h);
    void UploadConstants(ID3D11DeviceContext* ctx);

    ComPtr<ID3D11VertexShader>       m_vs;
    ComPtr<ID3D11PixelShader>        m_psEasu;
    ComPtr<ID3D11PixelShader>        m_psRcas;
    ComPtr<ID3D11PixelShader>        m_psCopy;
    ComPtr<ID3D11Buffer>             m_cb;
    ComPtr<ID3D11SamplerState>       m_smp;
    ComPtr<ID3D11Texture2D>          m_mid;
    ComPtr<ID3D11ShaderResourceView> m_midSrv;
    ComPtr<ID3D11RenderTargetView>   m_midRtv;

    UINT  m_srcW = 0, m_srcH = 0;
    UINT  m_vpW  = 0, m_vpH  = 0;
    float m_sharpness = 0.8f;

    std::wstring m_error;
};
