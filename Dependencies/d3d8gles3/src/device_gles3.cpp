/*
** d3d8gles3 — GLES3 device core implementation.
**
** Translates the deferred D3D8 render/stage/transform state into live GLES3
** state and dispatches indexed draws through the FFP shader cache. The COM shim
** (d3d8_entry.cpp) forwards IDirect3DDevice8 calls here verbatim.
*/
#include "device_gles3.h"
#include "gl_state_map.h"
#include "resources_gles3.h"

#include <cstring>
#include <cstdio>

// SDL3 owns the GL context/window; Present() flips it. SDL3 is linked PUBLIC to
// this backend (see CMakeLists.txt), so this include resolves on every platform.
#include <SDL3/SDL_video.h>

namespace d3d8gles3 {

// D3DRENDERSTATETYPE values the device reads. (Subset actually used by WW3D2.)
enum {
    RS_ZENABLE=7, RS_FILLMODE=8, RS_ZWRITEENABLE=14, RS_ALPHATESTENABLE=15,
    RS_SRCBLEND=19, RS_DESTBLEND=20, RS_CULLMODE=22, RS_ZFUNC=23,
    RS_ALPHAREF=24, RS_ALPHAFUNC=25, RS_ALPHABLENDENABLE=27, RS_FOGENABLE=28,
    RS_FOGCOLOR=34, RS_FOGTABLEMODE=35, RS_FOGSTART=36, RS_FOGEND=37,
    RS_FOGDENSITY=38, RS_FOGVERTEXMODE=140, RS_RANGEFOGENABLE=48,
    RS_STENCILENABLE=52, RS_STENCILFAIL=53, RS_STENCILZFAIL=54, RS_STENCILPASS=55,
    RS_STENCILFUNC=56, RS_STENCILREF=57, RS_STENCILMASK=58, RS_STENCILWRITEMASK=59,
    RS_TEXTUREFACTOR=60, RS_LIGHTING=137, RS_AMBIENT=139, RS_COLORVERTEX=141,
    RS_BLENDOP=171, RS_ZBIAS=47
};
// D3DTSS
enum {
    TSS_COLOROP=1, TSS_COLORARG1=2, TSS_COLORARG2=3, TSS_ALPHAOP=4,
    TSS_ALPHAARG1=5, TSS_ALPHAARG2=6, TSS_TEXCOORDINDEX=11,
    TSS_TEXTURETRANSFORMFLAGS=24
};
// D3DTRANSFORMSTATETYPE
enum { TS_VIEW=2, TS_PROJECTION=3, TS_WORLD=256 };

// DEBUG counters (strip before commit) — see if draws reach GL each frame.
unsigned long g_dbgFrame = 0, g_dbgDrawIdx = 0, g_dbgDrawArr = 0,
              g_dbgDrawUP = 0, g_dbgDrawIdxUP = 0, g_dbgClear = 0;

static inline void U8x4FromARGB(uint32_t c, float out[4]) {
    out[3] = ((c >> 24) & 0xFF) / 255.0f; // A
    out[0] = ((c >> 16) & 0xFF) / 255.0f; // R
    out[1] = ((c >> 8)  & 0xFF) / 255.0f; // G
    out[2] = ((c)       & 0xFF) / 255.0f; // B
}

// ============================================================================
bool GLES3Device::Init(int w, int h) {
    bbW_ = w; bbH_ = h;
    vpX_ = 0; vpY_ = 0; vpW_ = w; vpH_ = h;
    curTargetH_ = h;
    // SDL's GL backbuffer is not guaranteed to be FBO 0 (Android may give us an
    // FBO-backed window surface). Capture whatever is bound now as the target to
    // restore when render-to-texture finishes.
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &defaultFbo_);
    glGenVertexArrays(1, &vao_);
    // S3TC availability decides DXT upload vs CPU decompress (resources layer).
    const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    s3tcSupported_ = exts && std::strstr(exts, "GL_EXT_texture_compression_s3tc");
    // Front-face winding. The engine uploads the raw D3D projection (no clip-space
    // Y flip), and SetViewport() flips only the viewport *rectangle*, not the image.
    // D3D and GL determine facing in window space with opposite Y directions, so the
    // same triangle that D3D treats as front-facing reads as CCW in GL's bottom-left
    // window space. GL's default front is already GL_CCW — matching D3D — so leaving
    // it as CW would invert every cull, hiding the single-sided terrain (its up-faces
    // get culled) and exposing the back/inside faces of closed meshes (the whole
    // scene appeared to render "from inside"). MapCull() is paired with this winding.
    glFrontFace(GL_CCW);

    // Seed D3D8 default render states. The engine only ever writes the states it
    // wants to *change*; for anything it never sets it relies on D3D's documented
    // defaults. Most critically D3DRS_ZENABLE defaults to TRUE — WW3D never sets
    // it (it manages only ZFUNC/ZWRITEENABLE per shader). Our zero-init left it
    // FALSE, so the whole 3D scene ran with depth testing off and drew in painter
    // order, letting late/bright surfaces erase the scene to white. Seed the
    // depth defaults (others already have apply-time fallbacks).
    renderStates_[RS_ZENABLE]      = 1;  // D3DZB_TRUE
    renderStates_[RS_ZWRITEENABLE] = 1;  // TRUE
    renderStates_[RS_ZFUNC]        = 4;  // D3DCMP_LESSEQUAL

    initialized_ = true;
    return true;
}

void GLES3Device::Shutdown() {
    ffpCache_.Clear();
    if (vao_)   { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    if (upVBO_) { glDeleteBuffers(1, &upVBO_); upVBO_ = 0; }
    if (upIBO_) { glDeleteBuffers(1, &upIBO_); upIBO_ = 0; }
    if (rtFBO_) { glDeleteFramebuffers(1, &rtFBO_); rtFBO_ = 0; }
    if (rtDepthRB_) { glDeleteRenderbuffers(1, &rtDepthRB_); rtDepthRB_ = 0; }
    initialized_ = false;
}

void GLES3Device::OnContextLost() {
    ffpCache_.Clear();
    vao_ = 0;
    upVBO_ = 0; upIBO_ = 0;   // GL objects are gone with the context
    rtFBO_ = 0; rtDepthRB_ = 0; rtDepthW_ = 0; rtDepthH_ = 0;
}

void GLES3Device::EnsureUPBuffers() {
    if (!upVBO_) glGenBuffers(1, &upVBO_);
    if (!upIBO_) glGenBuffers(1, &upIBO_);
}

// ---- frame -----------------------------------------------------------------
void GLES3Device::Clear(bool color, bool depthStencil, const float rgba[4],
                        float z, uint32_t stencil) {
    GLbitfield mask = 0;
    if (color) {
        glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
        mask |= GL_COLOR_BUFFER_BIT;
        // a Clear that touches color must allow the write even if a prior draw
        // disabled color writes — restore full mask defensively.
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    if (depthStencil) {
        glClearDepthf(z);
        glClearStencil(static_cast<GLint>(stencil));
        glDepthMask(GL_TRUE);   // depth clear needs depth writes enabled
        mask |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
    }
    if (mask) { glClear(mask); ++g_dbgClear; }
}

void GLES3Device::BeginScene() { /* nothing: GLES has no scene begin */ }
void GLES3Device::EndScene()   { glFlush(); }

void GLES3Device::Present()    {
    // Flip the SDL-owned GL context. swapWindow_ is the SDL_Window* handed down
    // from D3D8 CreateDevice (null in host unit tests -> no-op).
    if ((g_dbgFrame % 60) == 0) {
        GLenum e = glGetError();
        fprintf(stderr, "DEBUG: Present frame=%lu clears=%lu drawIdx=%lu drawArr=%lu drawUP=%lu drawIdxUP=%lu glErr=0x%x swapWin=%p\n",
                g_dbgFrame, g_dbgClear, g_dbgDrawIdx, g_dbgDrawArr, g_dbgDrawUP, g_dbgDrawIdxUP, (unsigned)e, swapWindow_);
        fflush(stderr);
    }
    ++g_dbgFrame;
    if (swapWindow_) SDL_GL_SwapWindow(static_cast<SDL_Window*>(swapWindow_));
}

void GLES3Device::SetViewport(int x, int y, int w, int h, float, float) {
    // Remember the D3D viewport rect (top-left origin) for the XYZRHW screen->clip
    // conversion in the FFP vertex shader (uViewport).
    vpX_ = x; vpY_ = y; vpW_ = w; vpH_ = h;
    // D3D viewport origin is top-left; GL is bottom-left. Flip Y against the bound
    // target's height (the backbuffer normally, or the render-target when one is
    // bound) — not always the backbuffer, or RT draws land off-screen.
    glViewport(x, curTargetH_ - (y + h), w, h);
}

void GLES3Device::SetRenderTarget(GLTexture* color) {
    if (!color) {
        // Restore the window framebuffer + full-backbuffer viewport.
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(defaultFbo_));
        curTargetH_ = bbH_;
        SetViewport(0, 0, bbW_, bbH_, 0.0f, 1.0f);
        return;
    }

    if (!rtFBO_) glGenFramebuffers(1, &rtFBO_);
    glBindFramebuffer(GL_FRAMEBUFFER, rtFBO_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           color->GlName(), 0);

    // Shadow casters are rendered with depth testing, so the FBO needs a depth
    // buffer. Reuse one renderbuffer, growing it when a larger target appears.
    const int w = static_cast<int>(color->Width());
    const int h = static_cast<int>(color->Height());
    if (!rtDepthRB_ || w > rtDepthW_ || h > rtDepthH_) {
        if (!rtDepthRB_) glGenRenderbuffers(1, &rtDepthRB_);
        glBindRenderbuffer(GL_RENDERBUFFER, rtDepthRB_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        rtDepthW_ = w; rtDepthH_ = h;
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rtDepthRB_);

    curTargetH_ = h;
    SetViewport(0, 0, w, h, 0.0f, 1.0f);
}

// ---- deferred state --------------------------------------------------------
void GLES3Device::SetRenderState(uint32_t state, uint32_t value) {
    if (state < kNumRenderStates) renderStates_[state] = value;
}
void GLES3Device::SetTextureStageState(uint32_t stage, uint32_t type, uint32_t value) {
    if (stage < kMaxStages && type < 32) stageStates_[stage][type] = value;
}
void GLES3Device::SetTransform(uint32_t t, const float* m) {
    if (t == TS_WORLD)           std::memcpy(world_.m, m, sizeof(float)*16);
    else if (t == TS_VIEW)       std::memcpy(view_.m, m, sizeof(float)*16);
    else if (t == TS_PROJECTION) std::memcpy(proj_.m, m, sizeof(float)*16);
    // D3DTS_TEXTURE0..7 == 16..23: per-stage texture-coordinate transform used by
    // the projected-shadow texgen path.
    else if (t >= 16 && t < 16 + kMaxStages) std::memcpy(texMatrix_[t-16].m, m, sizeof(float)*16);
}
void GLES3Device::GetTransform(uint32_t t, float* out) const {
    // Return the stored transform. Several engine paths read VIEW back and invert
    // it (terrain cloud/noise + shroud texgen via _Get_DX8_Transform); without
    // this they'd invert uninitialised garbage -> NaN texture matrix -> the
    // shroud/cloud texture samples NaN and the whole terrain multiplies to black.
    static const float kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const float* src = kIdentity;
    if      (t == TS_WORLD)                   src = world_.m;
    else if (t == TS_VIEW)                    src = view_.m;
    else if (t == TS_PROJECTION)              src = proj_.m;
    else if (t >= 16 && t < 16 + kMaxStages)  src = texMatrix_[t-16].m;
    std::memcpy(out, src, sizeof(float)*16);
}
void GLES3Device::SetTexture(uint32_t stage, GLTexture* tex) {
    if (stage < kMaxStages) boundTex_[stage] = tex;
}
void GLES3Device::SetTextureFactor(uint32_t argb) { tFactor_ = argb; }
void GLES3Device::SetVertexShader(uint32_t fvf) { fvf_ = fvf; }

void GLES3Device::SetMaterial(const void* p) {
    // D3DMATERIAL8 layout: Diffuse, Ambient, Specular, Emissive (D3DCOLORVALUE
    // = 4 floats each), then Power (float). Copy by offset to avoid needing the
    // struct definition here.
    const float* f = static_cast<const float*>(p);
    std::memcpy(matDiffuse_,  f + 0,  sizeof(float)*4);
    std::memcpy(matAmbient_,  f + 4,  sizeof(float)*4);
    std::memcpy(matSpecular_, f + 8,  sizeof(float)*4);
    std::memcpy(matEmissive_, f + 12, sizeof(float)*4);
    matPower_ = f[16];
}

void GLES3Device::SetLight(uint32_t index, const void* p) {
    if (index >= kMaxLights) return;
    // D3DLIGHT8 float layout: Type(0), Diffuse(1-4), Specular(5-8), Ambient(9-12),
    // Position(13-15), Direction(16-18), Range(19), Falloff(20),
    // Attenuation0/1/2(21,22,23), Theta(24), Phi(25).
    const auto* u = static_cast<const uint32_t*>(p);
    const float* f = reinterpret_cast<const float*>(u);
    Light& L = lights_[index];
    L.type = static_cast<uint8_t>(u[0]);             // 1 point, 2 spot, 3 directional
    std::memcpy(L.diffuse, f + 1, sizeof(float)*4);
    std::memcpy(L.ambient, f + 9, sizeof(float)*4);
    std::memcpy(L.pos,     f + 13, sizeof(float)*3);
    std::memcpy(L.dir,     f + 16, sizeof(float)*3);
    L.atten[0] = f[21]; L.atten[1] = f[22]; L.atten[2] = f[23];
}
void GLES3Device::LightEnable(uint32_t index, bool enable) {
    if (index < kMaxLights) lights_[index].enabled = enable;
}

// ---- geometry --------------------------------------------------------------
void GLES3Device::SetStreamSource(uint32_t stream, GLBuffer* vb, uint32_t stride) {
    if (stream < MaxStreams) { streamVB_[stream] = vb; streamStride_[stream] = stride; }
}
void GLES3Device::SetIndices(GLBuffer* ib, uint32_t base) {
    indexBuffer_ = ib; baseVertexIndex_ = base;
}

// ============================================================================
// State translation
// ============================================================================
FFPKey GLES3Device::BuildFFPKey() const {
    FFPKey k;
    // FVF decode (D3DFVF_*): XYZ=0x002, XYZRHW=0x004, NORMAL=0x010,
    // DIFFUSE=0x040, SPECULAR=0x080, TEXCOUNT shifted by 8.
    k.isPretransformed = (fvf_ & 0x004) ? 1 : 0;
    k.hasNormal   = (fvf_ & 0x010) ? 1 : 0;
    k.hasDiffuse  = (fvf_ & 0x040) ? 1 : 0;
    k.hasSpecular = (fvf_ & 0x080) ? 1 : 0;
    k.numTexCoords = static_cast<uint8_t>((fvf_ >> 8) & 0xF);

    k.lightingEnabled = (!k.isPretransformed && renderStates_[RS_LIGHTING]) ? 1 : 0;
    k.colorVertex     = renderStates_[RS_COLORVERTEX] ? 1 : 0;
    uint8_t n = 0;
    for (int i = 0; i < kMaxLights; ++i)
        if (lights_[i].enabled) { k.lightType[n] = lights_[i].type ? lights_[i].type : 3; ++n; }
    k.numLights = n;

    if (renderStates_[RS_FOGENABLE]) {
        k.fogEnabled = 1;
        uint32_t mode = renderStates_[RS_FOGTABLEMODE];
        if (mode == 0) mode = renderStates_[RS_FOGVERTEXMODE];
        k.fogMode = (mode == 1) ? 1 : (mode == 2) ? 2 : (mode == 3) ? 3 : 1;
        k.fogRange = renderStates_[RS_RANGEFOGENABLE] ? 1 : 0;
    }

    if (renderStates_[RS_ALPHATESTENABLE]) {
        k.alphaTestEnabled = 1;
        k.alphaTestFunc = static_cast<uint8_t>(renderStates_[RS_ALPHAFUNC] ? renderStates_[RS_ALPHAFUNC] : 8);
    }

    for (int i = 0; i < kMaxStages; ++i) {
        StageKey& s = k.stages[i];
        s.colorOp   = static_cast<uint8_t>(stageStates_[i][TSS_COLOROP] ? stageStates_[i][TSS_COLOROP] : (i==0?4:1));
        s.colorArg1 = static_cast<uint8_t>(stageStates_[i][TSS_COLORARG1] ? stageStates_[i][TSS_COLORARG1] : 2);
        s.colorArg2 = static_cast<uint8_t>(stageStates_[i][TSS_COLORARG2] ? stageStates_[i][TSS_COLORARG2] : 1);
        s.alphaOp   = static_cast<uint8_t>(stageStates_[i][TSS_ALPHAOP] ? stageStates_[i][TSS_ALPHAOP] : 1);
        s.alphaArg1 = static_cast<uint8_t>(stageStates_[i][TSS_ALPHAARG1] ? stageStates_[i][TSS_ALPHAARG1] : 2);
        s.alphaArg2 = static_cast<uint8_t>(stageStates_[i][TSS_ALPHAARG2] ? stageStates_[i][TSS_ALPHAARG2] : 1);
        const uint32_t tci = stageStates_[i][TSS_TEXCOORDINDEX];
        s.texCoordIndex = static_cast<uint8_t>(tci & 0xFF);
        // High word selects fixed-function texcoord generation (D3DTSS_TCI_*):
        // 0 passthru, 1 camera-space normal, 2 camera-space position (projected
        // shadows), 3 camera-space reflection.
        s.texGen = static_cast<uint8_t>((tci >> 16) & 0xFF);
        // D3DTTFF_* low bits are the output coord count; non-zero means run the
        // coords through the stage's texture-transform matrix.
        s.texXform = static_cast<uint8_t>(stageStates_[i][TSS_TEXTURETRANSFORMFLAGS] & 0x7);
        s.textureBound  = boundTex_[i] ? 1 : 0;
    }
    return k;
}

void GLES3Device::ApplyBlendDepthStencilCull() {
    // Z
    if (renderStates_[RS_ZENABLE]) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(MapCompareFunc(renderStates_[RS_ZFUNC] ? renderStates_[RS_ZFUNC] : 4));
    glDepthMask(renderStates_[RS_ZWRITEENABLE] ? GL_TRUE : GL_FALSE);

    // Blend
    if (renderStates_[RS_ALPHABLENDENABLE]) {
        glEnable(GL_BLEND);
        glBlendEquation(MapBlendOp(renderStates_[RS_BLENDOP] ? renderStates_[RS_BLENDOP] : 1));
        glBlendFunc(MapBlendFactor(renderStates_[RS_SRCBLEND] ? renderStates_[RS_SRCBLEND] : 2),
                    MapBlendFactor(renderStates_[RS_DESTBLEND] ? renderStates_[RS_DESTBLEND] : 1));
    } else {
        glDisable(GL_BLEND);
    }

    // Cull
    bool cullEnable; GLenum cullFace;
    MapCull(renderStates_[RS_CULLMODE] ? renderStates_[RS_CULLMODE] : 3, cullEnable, cullFace);
    if (cullEnable) { glEnable(GL_CULL_FACE); glCullFace(cullFace); } else glDisable(GL_CULL_FACE);

    // Stencil
    if (renderStates_[RS_STENCILENABLE]) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(MapCompareFunc(renderStates_[RS_STENCILFUNC] ? renderStates_[RS_STENCILFUNC] : 8),
                      static_cast<GLint>(renderStates_[RS_STENCILREF]),
                      renderStates_[RS_STENCILMASK] ? renderStates_[RS_STENCILMASK] : 0xFFFFFFFF);
        glStencilOp(MapStencilOp(renderStates_[RS_STENCILFAIL] ? renderStates_[RS_STENCILFAIL] : 1),
                    MapStencilOp(renderStates_[RS_STENCILZFAIL] ? renderStates_[RS_STENCILZFAIL] : 1),
                    MapStencilOp(renderStates_[RS_STENCILPASS] ? renderStates_[RS_STENCILPASS] : 1));
    } else {
        glDisable(GL_STENCIL_TEST);
    }

    // Z bias -> polygon offset
    if (renderStates_[RS_ZBIAS]) {
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-static_cast<float>(renderStates_[RS_ZBIAS]), -static_cast<float>(renderStates_[RS_ZBIAS]));
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
}

void GLES3Device::ApplyFixedFunctionUniforms(const FFPProgram& prog) {
    if (prog.uWorld >= 0) glUniformMatrix4fv(prog.uWorld, 1, GL_FALSE, world_.m);
    if (prog.uView  >= 0) glUniformMatrix4fv(prog.uView,  1, GL_FALSE, view_.m);
    if (prog.uProj  >= 0) glUniformMatrix4fv(prog.uProj,  1, GL_FALSE, proj_.m);
    if (prog.uViewport >= 0) {
        float vp[4] = { (float)vpX_, (float)vpY_, (float)vpW_, (float)vpH_ };
        glUniform4fv(prog.uViewport, 1, vp);
    }
    // Per-stage texture-transform matrices (only the stages whose shader declared
    // one resolve to a valid location).
    for (int i = 0; i < kMaxStages; ++i)
        if (prog.uTexMatrix[i] >= 0)
            glUniformMatrix4fv(prog.uTexMatrix[i], 1, GL_FALSE, texMatrix_[i].m);

    if (prog.uMaterialAmbient  >= 0) glUniform4fv(prog.uMaterialAmbient,  1, matAmbient_);
    if (prog.uMaterialDiffuse  >= 0) glUniform4fv(prog.uMaterialDiffuse,  1, matDiffuse_);
    if (prog.uMaterialEmissive >= 0) glUniform4fv(prog.uMaterialEmissive, 1, matEmissive_);

    float globalAmbient[4]; U8x4FromARGB(renderStates_[RS_AMBIENT], globalAmbient);
    if (prog.uGlobalAmbient >= 0) glUniform4fv(prog.uGlobalAmbient, 1, globalAmbient);

    float tf[4]; U8x4FromARGB(tFactor_, tf);
    if (prog.uTFactor >= 0) glUniform4fv(prog.uTFactor, 1, tf);

    // pack enabled lights contiguously to match shader indices
    if (prog.uLightDiffuse >= 0) {
        float dir[3*kMaxLights]{}, pos[3*kMaxLights]{}, dif[4*kMaxLights]{},
              amb[4*kMaxLights]{}, att[3*kMaxLights]{};
        int n = 0;
        for (int i = 0; i < kMaxLights && n < kMaxLights; ++i) {
            if (!lights_[i].enabled) continue;
            const Light& L = lights_[i];
            std::memcpy(dir + 3*n, L.dir, sizeof(float)*3);
            std::memcpy(pos + 3*n, L.pos, sizeof(float)*3);
            std::memcpy(dif + 4*n, L.diffuse, sizeof(float)*4);
            std::memcpy(amb + 4*n, L.ambient, sizeof(float)*4);
            std::memcpy(att + 3*n, L.atten, sizeof(float)*3);
            ++n;
        }
        if (prog.uLightDir     >= 0) glUniform3fv(prog.uLightDir,     kMaxLights, dir);
        if (prog.uLightPos     >= 0) glUniform3fv(prog.uLightPos,     kMaxLights, pos);
        if (prog.uLightDiffuse >= 0) glUniform4fv(prog.uLightDiffuse, kMaxLights, dif);
        if (prog.uLightAmbient >= 0) glUniform4fv(prog.uLightAmbient, kMaxLights, amb);
        if (prog.uLightAtten   >= 0) glUniform3fv(prog.uLightAtten,   kMaxLights, att);
    }

    // fog
    if (prog.uFogColor >= 0) { float fc[4]; U8x4FromARGB(renderStates_[RS_FOGCOLOR], fc); glUniform4fv(prog.uFogColor, 1, fc); }
    if (prog.uFogStart   >= 0) glUniform1f(prog.uFogStart,   *reinterpret_cast<float*>(&renderStates_[RS_FOGSTART]));
    if (prog.uFogEnd     >= 0) glUniform1f(prog.uFogEnd,     *reinterpret_cast<float*>(&renderStates_[RS_FOGEND]));
    if (prog.uFogDensity >= 0) glUniform1f(prog.uFogDensity, *reinterpret_cast<float*>(&renderStates_[RS_FOGDENSITY]));

    // alpha test ref (D3DRS_ALPHAREF is 0..255)
    if (prog.uAlphaRef >= 0) glUniform1f(prog.uAlphaRef, (renderStates_[RS_ALPHAREF] & 0xFF) / 255.0f);
}

// D3DFVF bits we decode.
enum {
    FVF_POSITION_MASK=0x00E, FVF_XYZ=0x002, FVF_XYZRHW=0x004,
    FVF_NORMAL=0x010, FVF_PSIZE=0x020, FVF_DIFFUSE=0x040, FVF_SPECULAR=0x080,
    FVF_TEXCOUNT_MASK=0xF00, FVF_TEXCOUNT_SHIFT=8
};

// Fixed attribute locations — must match ffp_shader_gen's `layout(location=N)`.
enum { LOC_POS=0, LOC_NORMAL=1, LOC_DIFFUSE=2, LOC_SPECULAR=3, LOC_TEX0=4 };

// Float count of the position component (incl. blend weights for XYZBn).
static uint32_t PositionFloats(uint32_t fvf) {
    switch (fvf & FVF_POSITION_MASK) {
        case FVF_XYZ:    return 3;
        case FVF_XYZRHW: return 4;
        case 0x006:      return 4; // XYZB1 (3 + 1 weight)
        case 0x008:      return 5; // XYZB2
        case 0x00A:      return 6; // XYZB3
        case 0x00C:      return 7; // XYZB4
        case 0x00E:      return 8; // XYZB5
        default:         return 3;
    }
}

// Floats per texcoord set N, from the FVF high-word size bits (default 2).
static uint32_t TexCoordSize(uint32_t fvf, int set) {
    const uint32_t bits = (fvf >> (16 + set * 2)) & 0x3;
    switch (bits) { case 1: return 3; case 2: return 4; case 3: return 1; default: return 2; }
}

void GLES3Device::BindVertexAttributes(uintptr_t baseByteOffset) {
    if (!streamVB_[0]) return;
    streamVB_[0]->BindAs(GL_ARRAY_BUFFER);
    if (indexBuffer_) indexBuffer_->BindAs(GL_ELEMENT_ARRAY_BUFFER);
    SetupVertexAttributes(static_cast<int>(streamStride_[0]), baseByteOffset);
}

// Configure the FVF-derived vertex attribute pointers against whatever buffer is
// currently bound to GL_ARRAY_BUFFER (the stream VB for indexed/array draws, or
// the transient UP VBO for user-pointer draws). `stride` is the vertex size.
void GLES3Device::SetupVertexAttributes(int strideIn, uintptr_t baseByteOffset) {
    const GLsizei stride = static_cast<GLsizei>(strideIn);
    const uint32_t fvf = fvf_;
    // baseByteOffset folds D3D's BaseVertexIndex (SetIndices) into the attribute
    // pointers: GLES3 core has no glDrawElementsBaseVertex, so we shift every
    // attribute's start by baseVertexIndex*stride. The per-vertex field offsets
    // then accumulate on top of it. Zero for non-indexed / user-pointer draws.
    uintptr_t off = baseByteOffset;
    auto attrib = [&](GLuint loc, GLint size, GLenum type, GLboolean norm, uint32_t bytes) {
        glEnableVertexAttribArray(loc);
        glVertexAttribPointer(loc, size, type, norm, stride,
                              reinterpret_cast<const void*>(off));
        off += bytes;
    };

    // start every draw from a known-clean attribute set
    for (GLuint l = LOC_POS; l <= LOC_TEX0 + 7; ++l) glDisableVertexAttribArray(l);

    // 1) position (bind xyz, or xyzw for RHW); advance past any blend weights
    const uint32_t posFloats = PositionFloats(fvf);
    const bool rhw = (fvf & FVF_POSITION_MASK) == FVF_XYZRHW;
    attrib(LOC_POS, rhw ? 4 : 3, GL_FLOAT, GL_FALSE, posFloats * 4);

    // 2) normal
    if (fvf & FVF_NORMAL) attrib(LOC_NORMAL, 3, GL_FLOAT, GL_FALSE, 3 * 4);

    // point size — not a shader attribute; just skip its bytes
    if (fvf & FVF_PSIZE) off += 4;

    // 3) diffuse / 4) specular — D3DCOLOR: 4 normalised bytes (BGRA; shader swizzles)
    if (fvf & FVF_DIFFUSE)  attrib(LOC_DIFFUSE,  4, GL_UNSIGNED_BYTE, GL_TRUE, 4);
    if (fvf & FVF_SPECULAR) attrib(LOC_SPECULAR, 4, GL_UNSIGNED_BYTE, GL_TRUE, 4);

    // 5) texture coordinate sets
    const uint32_t texCount = (fvf & FVF_TEXCOUNT_MASK) >> FVF_TEXCOUNT_SHIFT;
    for (uint32_t i = 0; i < texCount; ++i) {
        const uint32_t sz = TexCoordSize(fvf, static_cast<int>(i));
        attrib(LOC_TEX0 + i, static_cast<GLint>(sz), GL_FLOAT, GL_FALSE, sz * 4);
    }
}

bool GLES3Device::ApplyStateCommon() {
    glBindVertexArray(vao_);
    ApplyBlendDepthStencilCull();

    FFPKey key = BuildFFPKey();
    const FFPProgram* prog = ffpCache_.GetProgram(key);
    if (!prog) return false;           // generation failed; skip draw (logged)
    glUseProgram(prog->program);

    // bind textures + samplers
    for (int i = 0; i < kMaxStages; ++i) {
        if (key.stages[i].colorOp == 1) break;
        if (boundTex_[i] && prog->uSampler[i] >= 0) {
            glActiveTexture(GL_TEXTURE0 + i);
            boundTex_[i]->Bind();
            // Apply the stage's D3D sampler state (wrap/filter). Without this every
            // texture keeps GL's default GL_REPEAT, which tiles clamp-addressed art
            // such as the projected shadow decals into dark streaks.
            boundTex_[i]->ApplySamplerState(stageStates_[i]);
            glUniform1i(prog->uSampler[i], i);
        }
    }

    ApplyFixedFunctionUniforms(*prog);
    return true;
}

void GLES3Device::ApplyState(uint32_t /*primType*/) {
    if (!ApplyStateCommon()) return;
    // Indexed draws honour D3D's BaseVertexIndex (set via SetIndices) by shifting
    // the attribute pointers; the indices themselves stay 0-based off that base.
    BindVertexAttributes(static_cast<uintptr_t>(baseVertexIndex_) * streamStride_[0]);
}

void GLES3Device::DrawIndexedPrimitive(uint32_t primType, uint32_t /*minIndex*/,
                                       uint32_t /*numVertices*/, uint32_t startIndex,
                                       uint32_t primCount) {
    if (!indexBuffer_) return;
    ApplyState(primType);

    const GLenum mode = MapPrimitiveType(primType);
    const uint32_t indexCount = PrimitiveCountToIndexCount(primType, primCount);
    const GLenum idxType = indexBuffer_->Is32Bit() ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
    const size_t idxSize = indexBuffer_->Is32Bit() ? 4 : 2;

    // D3D's BaseVertexIndex is applied via the attribute-pointer shift in
    // ApplyState/SetupVertexAttributes (GLES3 core has no glDrawElementsBaseVertex);
    // here only the startIndex selects where in the index buffer to begin.
    const void* offset = reinterpret_cast<const void*>(static_cast<uintptr_t>(startIndex * idxSize));
    glDrawElements(mode, static_cast<GLsizei>(indexCount), idxType, offset);
    ++g_dbgDrawIdx;
}

void GLES3Device::DrawPrimitive(uint32_t primType, uint32_t startVertex, uint32_t primCount) {
    if (!streamVB_[0]) return;
    if (!ApplyStateCommon()) return;
    BindVertexAttributes();   // binds the stream VB + FVF attributes
    // PrimitiveCountToIndexCount gives the index count for a prim count; for a
    // non-indexed draw that's exactly the vertex count consumed.
    const GLsizei vertCount = static_cast<GLsizei>(PrimitiveCountToIndexCount(primType, primCount));
    glDrawArrays(MapPrimitiveType(primType), static_cast<GLint>(startVertex), vertCount);
    ++g_dbgDrawArr;
}

void GLES3Device::DrawPrimitiveUP(uint32_t primType, uint32_t primCount,
                                  const void* vertexData, uint32_t stride) {
    if (!vertexData || !stride) return;
    if (!ApplyStateCommon()) return;

    const GLsizei vertCount = static_cast<GLsizei>(PrimitiveCountToIndexCount(primType, primCount));
    EnsureUPBuffers();
    glBindBuffer(GL_ARRAY_BUFFER, upVBO_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertCount) * stride,
                 vertexData, GL_STREAM_DRAW);
    // No element buffer for a non-indexed draw; clear any stale VAO binding.
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    SetupVertexAttributes(static_cast<int>(stride));
    glDrawArrays(MapPrimitiveType(primType), 0, vertCount);
    ++g_dbgDrawUP;
}

void GLES3Device::DrawIndexedPrimitiveUP(uint32_t primType, uint32_t minVertexIndex,
                                         uint32_t numVertices, uint32_t primCount,
                                         const void* indexData, uint32_t indexFmt,
                                         const void* vertexData, uint32_t stride) {
    if (!vertexData || !indexData || !stride) return;
    if (!ApplyStateCommon()) return;

    // D3DFMT_INDEX32 == 102, D3DFMT_INDEX16 == 101.
    const bool is32 = (indexFmt == 102);
    const GLenum idxType = is32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT;
    const size_t idxSize = is32 ? 4 : 2;
    const GLsizei indexCount = static_cast<GLsizei>(PrimitiveCountToIndexCount(primType, primCount));

    EnsureUPBuffers();
    // The UP vertex pointer addresses vertex 0; indices are absolute, so upload
    // through the highest referenced vertex (minVertexIndex + numVertices).
    const GLsizeiptr vbBytes = static_cast<GLsizeiptr>(minVertexIndex + numVertices) * stride;
    glBindBuffer(GL_ARRAY_BUFFER, upVBO_);
    glBufferData(GL_ARRAY_BUFFER, vbBytes, vertexData, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, upIBO_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexCount) * idxSize,
                 indexData, GL_STREAM_DRAW);
    SetupVertexAttributes(static_cast<int>(stride));
    glDrawElements(MapPrimitiveType(primType), indexCount, idxType, nullptr);
    ++g_dbgDrawIdxUP;
}

} // namespace d3d8gles3
