#include "pch.h"
#include "SeparableFilter.h"

using namespace DirectX;
using namespace Microsoft::WRL;

#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define TransFlush(ctx, res, state) \
ctx.TransitionResource(res, state, true)

#define BeginTrans(ctx, res, state) \
ctx.BeginResourceTransition(res, state)

namespace {
typedef D3D12_RESOURCE_STATES State;
const State RTV = D3D12_RESOURCE_STATE_RENDER_TARGET;
const State psSRV = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
const State csSRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

GraphicsPSO _hPassPSO[SeperableFilter::kNumKernelDiameter];
GraphicsPSO _vPassPSO[SeperableFilter::kNumKernelDiameter];
RootSignature _rootSignature;

std::once_flag _psoCompiled_flag;

SeperableFilter::KernelSize _kernelSize = SeperableFilter::k7KernelDiameter;
float _fGaussianVar = 625;
bool _cbStaled = true;
bool _enabled = false;

void _CreatePSOs()
{
    HRESULT hr;
    // Create Rootsignature
    _rootSignature.Reset(2);
    _rootSignature[0].InitAsConstantBuffer(0);
    _rootSignature[1].InitAsDescriptorRange(
        D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
    _rootSignature.Finalize(L"SeparableFilter",
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS);

    // Create PSO
    ComPtr<ID3DBlob> quadVS;
    ComPtr<ID3DBlob> hPassPS[SeperableFilter::kNumKernelDiameter];
    ComPtr<ID3DBlob> vPassPS[SeperableFilter::kNumKernelDiameter];

    uint32_t compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    D3D_SHADER_MACRO macro[] = {
        {"__hlsl", "1"}, // 0
        {"HorizontalPass", "0"}, // 1
        {"KERNEL_RADIUS", "0"}, // 2
        {nullptr, nullptr}
    };
    V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        L"SeparableFilter_SingleTriangleQuad_vs.hlsl").c_str(), macro,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_1",
        compileFlags, 0, &quadVS));
    char kernelSizeStr[8];
    for (int i = 0; i < SeperableFilter::kNumKernelDiameter; ++i) {
        sprintf_s(kernelSizeStr, 8, "%d", i);
        macro[2].Definition = kernelSizeStr;
        macro[1].Definition = "0";
        V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
            L"SeparableFilter_BilateralFilter_ps.hlsl").c_str(), macro,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
            "ps_5_1", compileFlags, 0, &vPassPS[i]));
        macro[1].Definition = "1";
        V(Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
            L"SeparableFilter_BilateralFilter_ps.hlsl").c_str(), macro,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
            "ps_5_1", compileFlags, 0, &hPassPS[i]));

        _hPassPSO[i].SetRootSignature(_rootSignature);
        _hPassPSO[i].SetRasterizerState(Graphics::g_RasterizerDefault);
        _hPassPSO[i].SetBlendState(Graphics::g_BlendDisable);
        _hPassPSO[i].SetDepthStencilState(Graphics::g_DepthStateDisabled);
        _hPassPSO[i].SetSampleMask(0xFFFFFFFF);
        _hPassPSO[i].SetInputLayout(0, nullptr);
        _hPassPSO[i].SetVertexShader(
            quadVS->GetBufferPointer(), quadVS->GetBufferSize());
        DXGI_FORMAT format = DXGI_FORMAT_R16_UINT;
        _hPassPSO[i].SetRenderTargetFormats(
            1, &format, DXGI_FORMAT_UNKNOWN);
        _hPassPSO[i].SetPrimitiveTopologyType(
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        _vPassPSO[i] = _hPassPSO[i];
        _vPassPSO[i].SetPixelShader(
            vPassPS[i]->GetBufferPointer(), vPassPS[i]->GetBufferSize());
        _vPassPSO[i].Finalize();
        _hPassPSO[i].SetPixelShader(
            hPassPS[i]->GetBufferPointer(), hPassPS[i]->GetBufferSize());
        _hPassPSO[i].Finalize();
    }
}
}

SeperableFilter::SeperableFilter()
{
    _dataCB.u2Reso = XMUINT2(0, 0);
    // We set 50mm as edge threshold, so 50 will be 2*deviation
    // ->deviation = 25;
    // ->variance = 625
    _dataCB.fGaussianVar = _fGaussianVar;
}

SeperableFilter::~SeperableFilter()
{
}

HRESULT
SeperableFilter::OnCreateResoure(LinearAllocator& uploadHeapAlloc)
{
    HRESULT hr = S_OK;
    std::call_once(_psoCompiled_flag, _CreatePSOs);
    _gpuCB.Create(L"SeparableFilter_CB", 1, sizeof(CBuffer),
        (void*)&_dataCB);
    _pUploadCB = new DynAlloc(
        std::move(uploadHeapAlloc.Allocate(sizeof(CBuffer))));
    return hr;
}

void
SeperableFilter::OnDestory()
{
    delete _pUploadCB;
    _gpuCB.Destroy();
    _outBuf.Destroy();
    _intermediateBuf.Destroy();
}

void
SeperableFilter::UpdateCB(DirectX::XMUINT2 reso)
{
    if (_dataCB.u2Reso.x != reso.x || _dataCB.u2Reso.y != reso.y) {
        _intermediateBuf.Destroy();
        _intermediateBuf.Create(L"BilateralTemp",
            (uint32_t)reso.x, (uint32_t)reso.y, 1, DXGI_FORMAT_R16_UINT);
        _outBuf.Destroy();
        _outBuf.Create(L"BilateralOut",
            (uint32_t)reso.x, (uint32_t)reso.y, 1, DXGI_FORMAT_R16_UINT);
        _dataCB.u2Reso = reso;
        _viewport.Width = static_cast<float>(reso.x);
        _viewport.Height = static_cast<float>(reso.y);
        _viewport.MaxDepth = 1.0;
        _scisorRact.right = static_cast<LONG>(reso.x);
        _scisorRact.bottom = static_cast<LONG>(reso.y);
        _cbStaled = true;
    }
}

void
SeperableFilter::OnRender(
    GraphicsContext& gfxCtx, ColorBuffer* pInputTex)
{
    if (!_enabled) {
        return;
    }

    if (_cbStaled) {
        _dataCB.fGaussianVar = _fGaussianVar;
        memcpy(_pUploadCB->DataPtr, &_dataCB, sizeof(CBuffer));
        gfxCtx.CopyBufferRegion(_gpuCB, 0, _pUploadCB->Buffer,
            _pUploadCB->Offset, sizeof(CBuffer));
        _cbStaled = false;
    }
    Trans(gfxCtx, *pInputTex, psSRV | csSRV);
    Trans(gfxCtx, _intermediateBuf, RTV);
    BeginTrans(gfxCtx, _outBuf, RTV);
    GPU_PROFILE(gfxCtx, L"BilateralFilter");
    gfxCtx.SetRootSignature(_rootSignature);
    gfxCtx.SetPipelineState(_hPassPSO[_kernelSize]);
    gfxCtx.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gfxCtx.SetRenderTargets(1, &_intermediateBuf.GetRTV());
    gfxCtx.SetViewport(_viewport);
    gfxCtx.SetScisor(_scisorRact);
    gfxCtx.SetConstantBuffer(0, _gpuCB.RootConstantBufferView());
    Bind(gfxCtx, 1, 0, 1, &pInputTex->GetSRV());
    gfxCtx.Draw(3);
    Trans(gfxCtx, _outBuf, RTV);
    Trans(gfxCtx, _intermediateBuf, psSRV);
    gfxCtx.SetPipelineState(_vPassPSO[_kernelSize]);
    gfxCtx.SetRenderTargets(1, &_outBuf.GetRTV());
    Bind(gfxCtx, 1, 0, 1, &_intermediateBuf.GetSRV());
    gfxCtx.Draw(3);
    BeginTrans(gfxCtx, _intermediateBuf, RTV);
}

void
SeperableFilter::RenderGui()
{
    using namespace ImGui;
    if (!CollapsingHeader("BilateralFilter")) {
        return;
    }
    if (Button("RecompileShaders##NormalGenerator")) {
        _CreatePSOs();
    }
    _cbStaled |= Checkbox("Enable Bilateral Filter", &_enabled);
    SliderInt("Kernel Radius", (int*)&_kernelSize, 0, kNumKernelDiameter - 1);
    _cbStaled |= DragFloat("Range Var", &_fGaussianVar, 1.f, 100, 2500);
}

ColorBuffer*
SeperableFilter::GetOutTex()
{
    return _enabled ? &_outBuf : nullptr;
}