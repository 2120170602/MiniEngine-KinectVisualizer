#pragma once

class CommandContext;
class GraphicsContext;

#define RECORD_TIME_HISTORY 0
namespace GPU_Profiler
{
const uint8_t MAX_TIMER_NAME_LENGTH = 32;
const uint8_t MAX_TIMER_COUNT = 128;
#if RECORD_TIME_HISTORY
const uint8_t MAX_TIMER_HISTORY = 128;
#else
const float AVG_COUNT_FACTOR = 256.f;
#endif
void Initialize();
HRESULT CreateResource();
void ShutDown();
void ProcessAndReadback(CommandContext& EngineContext);
uint16_t FillVertexData();
void DrawStats(GraphicsContext& gfxContext);
void RenderGui();
double ReadTimer(uint8_t idx, double* start = nullptr, double* stop = nullptr);
void SetFenceValue(uint64_t fenceValue);
uint16_t GetTimingStr(uint8_t idx, wchar_t* outStr);
};

class GPUProfileScope
{
public:
    GPUProfileScope(CommandContext& Context, const wchar_t* szName);
    ~GPUProfileScope();

    // Prevent copying
    GPUProfileScope(GPUProfileScope const&) = delete;
    GPUProfileScope& operator= (GPUProfileScope const&) = delete;

private:
    CommandContext& m_Context;
    uint32_t m_idx;
};

// Anon macros, used to create anonymous variables in macros.
#define ANON_INTERMEDIATE(a,b) a##b
#define ANON(a) ANON_INTERMEDIATE(a,__LINE__)

// attention: need to scope this macro and make sure their whole life span is
// during cmdlist record state
#ifndef RELEASE
#define GPU_PROFILE(d,x) GPUProfileScope ANON(pixProfile)(d, x)
#define GPU_PROFILE_FUNCTION(d) GPUProfileScope \
ANON(pixProfile)(d, __FUNCTION__ )
#else
#define GPU_PROFILE(d,x) ((void)0)
#define GPU_PROFILE_FUNCTION(d) ((void)0)
#endif