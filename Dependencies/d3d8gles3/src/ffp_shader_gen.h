/*
** d3d8gles3 — Fixed-Function Pipeline -> GLSL ES 3.00 shader generator.
**
** GLES3 has no fixed-function pipeline. D3D8 does everything (transform,
** lighting, the 8-stage texture cascade, fog, alpha test) with state, not
** shaders. This module captures the relevant FFP state into a compact, hashable
** key (FFPKey) and synthesises a matching GLSL ES vertex+fragment program,
** cached so each unique state combination is compiled once.
**
** The device calls BuildKey() while flushing deferred state before a draw, then
** GetProgram() to fetch (and lazily compile) the GL program for that key.
*/
#pragma once

#include <GLES3/gl3.h>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace d3d8gles3 {

constexpr int kMaxStages = 8;   // D3D MAX_TEXTURE_STAGES
constexpr int kMaxLights = 4;   // RenderStateStruct stores Lights[4]

// Per-texture-stage fixed-function description (mirrors D3DTSS_* state).
struct StageKey {
    uint8_t  colorOp   = 1;   // D3DTEXTUREOP, 1 == DISABLE
    uint8_t  colorArg1 = 2;   // D3DTA_TEXTURE
    uint8_t  colorArg2 = 1;   // D3DTA_CURRENT
    uint8_t  alphaOp   = 1;   // DISABLE
    uint8_t  alphaArg1 = 2;   // TEXTURE
    uint8_t  alphaArg2 = 1;   // CURRENT
    uint8_t  texCoordIndex = 0;
    uint8_t  textureBound  = 0; // 0/1 — is a texture set on this stage
    // D3DTSS_TEXCOORDINDEX high word: 0=passthru, 1=camera-space normal,
    // 2=camera-space position, 3=camera-space reflection. Projected shadows use 2.
    uint8_t  texGen        = 0;
    // D3DTSS_TEXTURETRANSFORMFLAGS count (0=disabled, 2=COUNT2, 3=COUNT3, 4=COUNT4);
    // when non-zero the generated/passthru coords are run through uTexMatrix[i].
    uint8_t  texXform      = 0;
};

// Whole-pipeline fixed-function key. Trivially memcmp/hash-able (no padding gaps
// that matter because we hash the byte span explicitly).
struct FFPKey {
    // vertex format (decoded from FVF)
    uint8_t  hasPosition   = 1;
    uint8_t  isPretransformed = 0; // XYZRHW: skip transform & lighting
    uint8_t  hasNormal     = 0;
    uint8_t  hasDiffuse    = 0;
    uint8_t  hasSpecular   = 0;
    uint8_t  numTexCoords  = 0;

    // lighting
    uint8_t  lightingEnabled = 0;
    uint8_t  numLights       = 0;
    uint8_t  lightType[kMaxLights] = {0,0,0,0}; // 1=point 2=spot 3=directional
    uint8_t  colorVertex     = 0; // D3DRS_COLORVERTEX: use vertex diffuse as material

    // fog
    uint8_t  fogEnabled = 0;
    uint8_t  fogMode    = 0;   // 0 none, 1 linear, 2 exp, 3 exp2 (pixel fog)
    uint8_t  fogRange   = 0;

    // alpha test (folded into fragment shader; GLES3 has no fixed alpha test)
    uint8_t  alphaTestEnabled = 0;
    uint8_t  alphaTestFunc    = 8; // D3DCMPFUNC, 8 == ALWAYS

    StageKey stages[kMaxStages];

    bool operator==(const FFPKey& o) const;
    std::size_t Hash() const;
};

// A compiled, linked GL program plus the resolved uniform locations the device
// needs to push per-draw state into.
struct FFPProgram {
    GLuint program = 0;

    // transforms
    GLint  uWorld = -1, uView = -1, uProj = -1;
    GLint  uViewport = -1;         // (x,y,w,h) of the active viewport, for XYZRHW
    GLint  uTexMatrix[kMaxStages];

    // material / global
    GLint  uMaterialAmbient = -1, uMaterialDiffuse = -1;
    GLint  uMaterialSpecular = -1, uMaterialEmissive = -1, uMaterialPower = -1;
    GLint  uGlobalAmbient = -1;
    GLint  uTFactor = -1;          // D3DRS_TEXTUREFACTOR

    // lights (arrays of kMaxLights)
    GLint  uLightDir = -1, uLightPos = -1, uLightDiffuse = -1;
    GLint  uLightAmbient = -1, uLightAtten = -1;

    // fog
    GLint  uFogColor = -1, uFogStart = -1, uFogEnd = -1, uFogDensity = -1;

    // alpha test
    GLint  uAlphaRef = -1;

    // samplers
    GLint  uSampler[kMaxStages];

    FFPProgram() {
        for (int i = 0; i < kMaxStages; ++i) { uTexMatrix[i] = -1; uSampler[i] = -1; }
    }
};

class FFPShaderCache {
public:
    // Returns a compiled program for the key, compiling+caching on first use.
    // Returns nullptr if generation failed (logged); caller should skip the draw.
    const FFPProgram* GetProgram(const FFPKey& key);

    // Emit GLSL source for a key without compiling — used by tests and for
    // dumping shaders when DXVK-style HUD debugging is enabled.
    static std::string GenerateVertexShader(const FFPKey& key);
    static std::string GenerateFragmentShader(const FFPKey& key);

    void Clear(); // drop all programs (e.g. on context loss)

private:
    struct KeyHash { std::size_t operator()(const FFPKey& k) const { return k.Hash(); } };
    std::unordered_map<FFPKey, FFPProgram, KeyHash> cache_;
};

} // namespace d3d8gles3
