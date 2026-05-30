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
    TSS_ALPHAARG1=5, TSS_ALPHAARG2=6, TSS_TEXCOORDINDEX=11
};
// D3DTRANSFORMSTATETYPE
enum { TS_VIEW=2, TS_PROJECTION=3, TS_WORLD=256 };

static inline void U8x4FromARGB(uint32_t c, float out[4]) {
    out[3] = ((c >> 24) & 0xFF) / 255.0f; // A
    out[0] = ((c >> 16) & 0xFF) / 255.0f; // R
    out[1] = ((c >> 8)  & 0xFF) / 255.0f; // G
    out[2] = ((c)       & 0xFF) / 255.0f; // B
}

// ============================================================================
bool GLES3Device::Init(int w, int h) {
    bbW_ = w; bbH_ = h;
    glGenVertexArrays(1, &vao_);
    // S3TC availability decides DXT upload vs CPU decompress (resources layer).
    const char* exts = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    s3tcSupported_ = exts && std::strstr(exts, "GL_EXT_texture_compression_s3tc");
    // D3D's default winding: clockwise == front under its left-handed projection.
    // The engine flips the projection for GL conventions, so front-face is CW here.
    glFrontFace(GL_CW);
    initialized_ = true;
    return true;
}

void GLES3Device::Shutdown() {
    ffpCache_.Clear();
    if (vao_) { glDeleteVertexArrays(1, &vao_); vao_ = 0; }
    initialized_ = false;
}

void GLES3Device::OnContextLost() {
    ffpCache_.Clear();
    vao_ = 0;
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
    if (mask) glClear(mask);
}

void GLES3Device::BeginScene() { /* nothing: GLES has no scene begin */ }
void GLES3Device::EndScene()   { glFlush(); }
void GLES3Device::Present()    { /* SDL_GL_SwapWindow done by window layer */ }

void GLES3Device::SetViewport(int x, int y, int w, int h, float, float) {
    // D3D viewport origin is top-left; GL is bottom-left. Flip Y.
    glViewport(x, bbH_ - (y + h), w, h);
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
    // D3DLIGHT8: Type(uint), Diffuse(4f), Specular(4f), Ambient(4f),
    // Position(3f), Direction(3f), Range,Falloff,Att0,Att1,Att2,Theta,Phi...
    const auto* u = static_cast<const uint32_t*>(p);
    const float* f = reinterpret_cast<const float*>(u);
    Light& L = lights_[index];
    L.type = static_cast<uint8_t>(u[0]);             // 1 point, 2 spot, 3 directional
    std::memcpy(L.diffuse, f + 1, sizeof(float)*4);
    std::memcpy(L.ambient, f + 9, sizeof(float)*4);
    std::memcpy(L.pos,     f + 13, sizeof(float)*3);
    std::memcpy(L.dir,     f + 16, sizeof(float)*3);
    L.atten[0] = f[20]; L.atten[1] = f[21]; L.atten[2] = f[22];
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
        s.texCoordIndex = static_cast<uint8_t>(stageStates_[i][TSS_TEXCOORDINDEX] & 0xFF);
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

void GLES3Device::BindVertexAttributes(const FFPProgram&) {
    // Bind stream 0 and configure attribute pointers from the FVF layout.
    // (Stream layout decode lives in resources_gles3 alongside the FVF parser;
    // this is the integration point — see STATUS.md item "FVF attribute binding".)
    if (streamVB_[0]) streamVB_[0]->BindAs(GL_ARRAY_BUFFER);
    if (indexBuffer_) indexBuffer_->BindAs(GL_ELEMENT_ARRAY_BUFFER);
    // TODO(android): glVertexAttribPointer per FVF field using streamStride_[0].
}

void GLES3Device::ApplyState(uint32_t /*primType*/) {
    glBindVertexArray(vao_);
    ApplyBlendDepthStencilCull();

    FFPKey key = BuildFFPKey();
    const FFPProgram* prog = ffpCache_.GetProgram(key);
    if (!prog) return;                 // generation failed; skip draw (logged)
    glUseProgram(prog->program);

    // bind textures + samplers
    for (int i = 0; i < kMaxStages; ++i) {
        if (key.stages[i].colorOp == 1) break;
        if (boundTex_[i] && prog->uSampler[i] >= 0) {
            glActiveTexture(GL_TEXTURE0 + i);
            boundTex_[i]->Bind();
            glUniform1i(prog->uSampler[i], i);
        }
    }

    ApplyFixedFunctionUniforms(*prog);
    BindVertexAttributes(*prog);
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

    // baseVertexIndex offset is folded into the index buffer bind on D3D8; GLES3
    // has no glDrawElementsBaseVertex in core, so the engine's base offset is
    // applied when the index data is uploaded (resources layer).
    const void* offset = reinterpret_cast<const void*>(static_cast<uintptr_t>(startIndex * idxSize));
    glDrawElements(mode, static_cast<GLsizei>(indexCount), idxType, offset);
}

} // namespace d3d8gles3
