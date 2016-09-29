#pragma once
#include "IRGBDStreamer.h"
#include "LinearFrameAllocator.h"

class SensorTexGen
{
#include "SensorTexGen.inl"
public:
    enum ProcessMode {
        kRaw = 0,
        kUndistorted = 1,
        kNumDataMode
    };
    enum DepthMode {
        kNoDepth = -1,
        kDepth = 0,
        kDepthWithVisual = 1,
        kDepthWithVisualWithInfrared = 2,
        kNumDepthMode
    };
    enum ColorMode {
        kNoColor = -1,
        kColor = 0,
        kNumColorMode,
    };
    enum TargetTexture {
        kDepthTex = 0,
        kDepthVisualTex = 1,
        kInfraredTex = 2,
        kColorTex = 3,
        kNumTargetTex,
    };

    SensorTexGen(bool enableColor, bool enableDepth, bool enableInfrared);
    ~SensorTexGen();
    void OnCreateResource();
    void OnDestory();
    // return true if get new data, false otherwise
    bool OnRender(CommandContext& EngineContext);
    void RenderGui();
    void GetDepthInfrareReso(uint16_t& width, uint16_t& height) const;
    void GetColorReso(uint16_t& width, uint16_t& height) const;
    ColorBuffer* GetOutTex(TargetTexture target);

private:
    ColorMode _colorMode = kColor, _preColorMode;
    DepthMode _depthMode = kDepthWithVisualWithInfrared, _preDepthMode;
    ProcessMode _processMode = kUndistorted;

    ColorBuffer _outTex[kNumTargetTex];
    D3D12_CPU_DESCRIPTOR_HANDLE _outTextureRTV[kNumTargetTex];

    IRGBDStreamer* _pKinect2;
    LinearFrameAllocator* _pFrameAlloc[IRGBDStreamer::kNumBufferTypes];
    KinectBuffer* _pKinectBuf[IRGBDStreamer::kNumBufferTypes];
    RenderCB _cbKinect;

    D3D12_VIEWPORT _depthInfraredViewport = {};
    D3D12_RECT _depthInfraredScissorRect = {};
    D3D12_VIEWPORT _colorViewport = {};
    D3D12_RECT _colorScissorRect = {};

    void _RetirePreviousFrameKinectBuffer();
    bool _PrepareAndFillinKinectBuffers();
    bool _FillinKinectBuffer(
        IRGBDStreamer::BufferType, const FrameData& frame);
};