/*
** d3d8gles3 — GLES3 device core.
**
** This is the portable OpenGL ES 3.0 state machine that backs the D3D8 device.
** It deliberately knows NOTHING about COM: the IDirect3DDevice8 shim
** (d3d8_entry.cpp) owns the ABI and forwards into the plain methods here. That
** split keeps all the GL logic host-compilable and unit-testable without the
** D3D8 headers, and keeps the COM glue trivial.
**
** Method names mirror the D3D8 device calls the WW3D2 engine actually issues
** (see dx8wrapper.cpp / the DX8CALL macros), so the shim is a 1:1 forward.
*/
#pragma once

#include <GLES3/gl3.h>
#include <cstdint>
#include "ffp_shader_gen.h"

namespace d3d8gles3 {

class GLBuffer;   // resources_gles3.h
class GLTexture;

// D3D fixed-function transform slots we care about.
struct Mat4 { float m[16]; };

class GLES3Device {
public:
    bool Init(int backbufferW, int backbufferH);
    void Shutdown();
    void OnContextLost();   // drop GL objects (program cache etc.)

    // --- frame ---------------------------------------------------------------
    void Clear(bool color, bool depthStencil, const float rgba[4], float z, uint32_t stencil);
    void BeginScene();
    void EndScene();
    void Present();         // SDL_GL_SwapWindow on the bound window (set via SetSwapWindow)
    void SetViewport(int x, int y, int w, int h, float minZ, float maxZ);

    // The SDL window whose GL context the device renders into. Passed down from
    // the D3D8 CreateDevice HWND (which is the SDL_Window* on the SDL3 ports).
    // Present() flips this window; if null, Present() is a no-op (host tests).
    void SetSwapWindow(void* sdlWindow) { swapWindow_ = sdlWindow; }

    // --- state (deferred; flushed at draw) -----------------------------------
    void SetRenderState(uint32_t state, uint32_t value);
    void SetTextureStageState(uint32_t stage, uint32_t type, uint32_t value);
    void SetTransform(uint32_t transformType, const float* m4x4);
    void SetTexture(uint32_t stage, GLTexture* tex);
    void SetMaterial(const void* d3dmaterial8);    // decoded internally
    void SetLight(uint32_t index, const void* d3dlight8);
    void LightEnable(uint32_t index, bool enable);
    void SetTextureFactor(uint32_t argb);

    // --- geometry ------------------------------------------------------------
    void SetStreamSource(uint32_t stream, GLBuffer* vb, uint32_t stride);
    void SetIndices(GLBuffer* ib, uint32_t baseVertexIndex);
    void SetVertexShader(uint32_t fvfOrHandle);    // FVF code (engine uses FVF, not handles)

    void DrawIndexedPrimitive(uint32_t primType, uint32_t minIndex, uint32_t numVertices,
                              uint32_t startIndex, uint32_t primCount);

private:
    // Translate the deferred D3D state into live GL state + bind the FFP program
    // for the current FVF/stage/light/fog configuration. Called by Draw*.
    void ApplyState(uint32_t primType);
    FFPKey BuildFFPKey() const;
    void   ApplyFixedFunctionUniforms(const FFPProgram& prog);
    void   ApplyBlendDepthStencilCull();
    void   BindVertexAttributes(const FFPProgram& prog);

    // ---- presentation ----
    void* swapWindow_ = nullptr;   // SDL_Window* to SDL_GL_SwapWindow in Present()

    // ---- cached D3D state ----
    static constexpr int kNumRenderStates = 256;
    uint32_t renderStates_[kNumRenderStates] = {};
    uint32_t stageStates_[kMaxStages][32] = {};
    GLTexture* boundTex_[kMaxStages] = {};

    Mat4 world_{}, view_{}, proj_{};
    uint32_t fvf_ = 0;
    uint32_t tFactor_ = 0xFFFFFFFF;

    // material (decoded)
    float matAmbient_[4]  = {1,1,1,1};
    float matDiffuse_[4]  = {1,1,1,1};
    float matSpecular_[4] = {0,0,0,0};
    float matEmissive_[4] = {0,0,0,0};
    float matPower_ = 0.0f;

    // lights (decoded), kMaxLights slots
    struct Light { uint8_t type=0; bool enabled=false; float dir[3]={0,0,1};
                   float pos[3]={0,0,0}; float diffuse[4]={0,0,0,0};
                   float ambient[4]={0,0,0,0}; float atten[3]={1,0,0}; };
    Light lights_[kMaxLights];

    // streams
    enum { MaxStreams = 2 };
    GLBuffer* streamVB_[MaxStreams] = {};
    uint32_t  streamStride_[MaxStreams] = {};
    GLBuffer* indexBuffer_ = nullptr;
    uint32_t  baseVertexIndex_ = 0;

    GLuint vao_ = 0;             // single VAO reconfigured per draw
    FFPShaderCache ffpCache_;
    bool   s3tcSupported_ = false;
    int    bbW_ = 0, bbH_ = 0;
    bool   initialized_ = false;
};

} // namespace d3d8gles3
