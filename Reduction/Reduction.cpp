#include "pch.h"
#include "Reduction.h"

#define Trans(ctx, res, state) \
ctx.TransitionResource(res, state)

#define Bind(ctx, rootIdx, offset, count, resource) \
ctx.SetDynamicDescriptors(rootIdx, offset, count, resource)

namespace {
enum THREAD : int {
    kT32 = 0,
    kT64,
    kT128,
    kT256,
    kT512,
    kT1024,
    kTNum
};
enum FETCH : int{
    kF1 = 0,
    kF2,
    kF4,
    kF8,
    kF16,
    kFNum
};

typedef D3D12_RESOURCE_STATES State;
const State UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
const State SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

RootSignature _rootsig;
ComputePSO _cptReductionPSO[Reduction::kTypes][kTNum][kFNum];
std::once_flag _psoCompiled_flag;

THREAD _threadPerTG = kT128;
FETCH _fetchPerThread = kF4;

inline uint32_t _GetReductionRate()
{
    return 32 * (1 << _threadPerTG) * (1 << _fetchPerThread);
}

inline HRESULT _Compile(LPCWSTR shaderName,
    const D3D_SHADER_MACRO* macro, ID3DBlob** bolb)
{
    UINT compilerFlag = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(DEBUG) || defined(_DEBUG)
    compilerFlag = D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#endif
    int iLen = (int)wcslen(shaderName);
    char target[8];
    wchar_t fileName[128];
    swprintf_s(fileName, 128, L"Reduction_%ls.hlsl", shaderName);
    switch (shaderName[iLen - 2]) {
    case 'c': sprintf_s(target, 8, "cs_5_1"); break;
    case 'p': sprintf_s(target, 8, "ps_5_1"); break;
    case 'v': sprintf_s(target, 8, "vs_5_1"); break;
    default:
        PRINTERROR(L"Shader name: %s is Invalid!", shaderName);
    }
    return Graphics::CompileShaderFromFile(Core::GetAssetFullPath(
        fileName).c_str(), macro, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main",
        target, compilerFlag, 0, bolb);
}

void _CreatePSO()
{
    using namespace Microsoft::WRL;
    HRESULT hr;
    ComPtr<ID3DBlob> reductionCS[Reduction::kTypes][kTNum][kFNum];
    D3D_SHADER_MACRO macros[] = {
        {"__hlsl", "1"},
        {"TYPE", ""},
        {"THREAD", ""},
        {"FETCH_COUNT", ""},
        {nullptr, nullptr}
    };
    char temp[32];
    for (UINT i = 0; i < Reduction::kTypes; ++i) {
        switch ((Reduction::TYPE) i)
        {
        case Reduction::kFLOAT4: macros[1].Definition = "float4"; break;
        case Reduction::kFLOAT: macros[1].Definition = "float"; break;
        default: PRINTERROR("Reduction Type is invalid"); break;
        }
        for (UINT j = 0; j < kTNum; ++j) {
            sprintf_s(temp, 32, "%d", 32 * (1 << j));
            macros[2].Definition = temp;
            char temp1[32];
            for (UINT k = 0; k < kFNum; ++k) {
                sprintf_s(temp1, 32, "%d", 1 << k);
                macros[3].Definition = temp1;
                V(_Compile(L"Reduction_cs", macros, &reductionCS[i][j][k]));
                _cptReductionPSO[i][j][k].SetRootSignature(_rootsig);
                _cptReductionPSO[i][j][k].SetComputeShader(
                    reductionCS[i][j][k]->GetBufferPointer(),
                    reductionCS[i][j][k]->GetBufferSize());
                _cptReductionPSO[i][j][k].Finalize();
            }
        }
    }
}

void _CreateStaticResoure()
{
    _rootsig.Reset(3);
    _rootsig[0].InitAsConstants(0, 2);
    _rootsig[1].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
    _rootsig[2].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
    _rootsig.Finalize(L"Reduction",
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS);
    _CreatePSO();
}
}

Reduction::Reduction(TYPE type, uint32_t size, uint32_t bufCount)
    : _type(type), _size(size), _bufCount(bufCount)
{
}

Reduction::~Reduction()
{
}

void
Reduction::OnCreateResource()
{
    std::call_once(_psoCompiled_flag, _CreateStaticResoure);
    uint32_t elementSize;
    switch (_type)
    {
    case kFLOAT4:
        elementSize = 4 * sizeof(float);
        _finalResult = new float[4 * _bufCount];
        break;
    case kFLOAT:
        elementSize = sizeof(float);
        _finalResult = new float[_bufCount];
        break;
    default:
        PRINTERROR("Reduction Type is invalid");break;
    }
    _currentReductionRate = _GetReductionRate();
    _CreateIntermmediateBuf(elementSize);
    _finalResultBuf.Create(L"Reduction_result", _bufCount, elementSize);
    _readBackBuf.Create(L"Reduction_Readback", _bufCount, elementSize);
}

void
Reduction::OnDestory()
{
    delete[] (float*)_finalResult;
    _intermmediateResultBuf[0].Destroy();
    _intermmediateResultBuf[1].Destroy();
    _finalResultBuf.Destroy();
    _readBackBuf.Destroy();
}

void
Reduction::ProcessingOneBuffer(ComputeContext& cptCtx,
    StructuredBuffer* pInputBuf, uint32_t bufId)
{
    uint32_t newReductionRate = _GetReductionRate();
    if (_currentReductionRate != newReductionRate) {
        uint32_t elementSize;
        switch (_type)
        {
        case kFLOAT4: elementSize = 4 * sizeof(float); break;
        case kFLOAT: elementSize = sizeof(float); break;
        default: PRINTERROR("Reduction Type is invalid"); break;
        }
        _currentReductionRate = newReductionRate;
        _CreateIntermmediateBuf(elementSize);
    }
    Trans(cptCtx, *pInputBuf, SRV);
    uint8_t curBuf = 0;
    cptCtx.SetRootSignature(_rootsig);
    cptCtx.SetPipelineState(
        _cptReductionPSO[_type][_threadPerTG][_fetchPerThread]);
    Bind(cptCtx, 2, 0, 1, &pInputBuf->GetSRV());
    UINT groupCount =
        (_size + _currentReductionRate - 1) / _currentReductionRate;
    while (groupCount > 1) {
        Trans(cptCtx, _intermmediateResultBuf[curBuf], UAV);
        Bind(cptCtx, 1, 0, 1, &_intermmediateResultBuf[curBuf].GetUAV());
        cptCtx.SetConstants(0, DWParam(bufId), DWParam(0));
        cptCtx.Dispatch(groupCount);
        Trans(cptCtx, _intermmediateResultBuf[curBuf], SRV);
        Bind(cptCtx, 2, 0, 1, &_intermmediateResultBuf[curBuf].GetSRV());
        curBuf = 1 - curBuf;
        groupCount =
            (groupCount + _currentReductionRate - 1) / _currentReductionRate;
    }
    Bind(cptCtx, 1, 0, 1, &_finalResultBuf.GetUAV());
    cptCtx.SetConstants(0, UINT(bufId), UINT(1));
    cptCtx.Dispatch(1);
}

void
Reduction::PrepareResult(ComputeContext& cptCtx)
{
    UINT elementSize;
    switch (_type)
    {
    case kFLOAT4: elementSize = 4 * sizeof(float); break;
    case kFLOAT: elementSize = sizeof(float); break;
    default: PRINTERROR("Reduction Type is invalid"); break;
    }
    cptCtx.CopyBufferRegion(
        _readBackBuf, 0, _finalResultBuf, 0, _bufCount * elementSize);
    _readBackFence = cptCtx.Flush(false);
}

void*
Reduction::ReadLastResult()
{
    Graphics::g_cmdListMngr.WaitForFence(_readBackFence);
    UINT elementSize;
    switch (_type)
    {
    case kFLOAT4: elementSize = 4 * sizeof(float); break;
    case kFLOAT: elementSize = sizeof(float); break;
    default: PRINTERROR("Reduction Type is invalid"); break;
    }
    D3D12_RANGE range = {0, elementSize};
    D3D12_RANGE umapRange = {};
    _readBackBuf.Map(&range, &_readBackPtr);
    memcpy(_finalResult, _readBackPtr, _bufCount * elementSize);
    _readBackBuf.Unmap(&umapRange);
    return _finalResult;
}

void
Reduction::RenderGui()
{
    using namespace ImGui;
    if (!CollapsingHeader("Reduction")) {
        return;
    }
    if (Button("ReconpileShaders##Reduction")) {
        _CreatePSO();
    }
    Separator();
    Columns(2);
    Text("GThread Num"); NextColumn();
    Text("Fetch Num"); NextColumn();
    RadioButton("32##Reduction", (int*)&_threadPerTG, kT32); NextColumn();
    RadioButton("1##Reduction", (int*)&_fetchPerThread, kF1); NextColumn();
    RadioButton("64##Reduction", (int*)&_threadPerTG, kT64); NextColumn();
    RadioButton("2##Reduction", (int*)&_fetchPerThread, kF2); NextColumn();
    RadioButton("128##Reduction", (int*)&_threadPerTG, kT128); NextColumn();
    RadioButton("4##Reduction", (int*)&_fetchPerThread, kF4); NextColumn();
    RadioButton("256##Reduction", (int*)&_threadPerTG, kT256); NextColumn();
    RadioButton("8##Reduction", (int*)&_fetchPerThread, kF8); NextColumn();
    RadioButton("512##Reduction", (int*)&_threadPerTG, kT512); NextColumn();
    RadioButton("16##Reduction", (int*)&_fetchPerThread, kF16); NextColumn();
    RadioButton("1024##Reduction", (int*)&_threadPerTG, kT1024); NextColumn();
    Columns(1);
}

void
Reduction::_CreateIntermmediateBuf(uint32_t elementSize)
{
    _intermmediateResultBuf[0].Destroy();
    _intermmediateResultBuf[1].Destroy();
    _intermmediateResultBuf[0].Create(L"Reduction",
        (_size + _currentReductionRate - 1) / _currentReductionRate,
        elementSize);
    _intermmediateResultBuf[1].Create(L"Reduction",
        (_size + _currentReductionRate - 1) / _currentReductionRate,
        elementSize);
}