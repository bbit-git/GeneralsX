/*
** d3d8gles3 — FFP -> GLSL ES 3.00 generator implementation.
**
** The generated shaders reproduce the subset of D3D8 fixed-function that C&C
** Generals / Zero Hour actually exercises: world/view/proj transform, per-vertex
** directional+point lighting with ambient, the multi-stage texture-op cascade,
** linear/exp/exp2 fog, and alpha test via discard. Texture ops and args outside
** that subset fall back to a documented default and log once.
*/
#include "ffp_shader_gen.h"

#include <cstdio>
#include <cstring>

namespace d3d8gles3 {

// ---- D3D enums we reference -------------------------------------------------
enum { // D3DTEXTUREOP
    TOP_DISABLE=1, TOP_SELECTARG1=2, TOP_SELECTARG2=3, TOP_MODULATE=4,
    TOP_MODULATE2X=5, TOP_MODULATE4X=6, TOP_ADD=7, TOP_ADDSIGNED=8,
    TOP_ADDSIGNED2X=9, TOP_SUBTRACT=10, TOP_ADDSMOOTH=11,
    TOP_BLENDDIFFUSEALPHA=12, TOP_BLENDTEXTUREALPHA=13, TOP_BLENDFACTORALPHA=14,
    TOP_BLENDTEXTUREALPHAPM=15, TOP_BLENDCURRENTALPHA=16, TOP_DOTPRODUCT3=24
};
enum { // D3DTA (low 3 bits) + modifier flags
    TA_DIFFUSE=0, TA_CURRENT=1, TA_TEXTURE=2, TA_TFACTOR=3, TA_SPECULAR=4,
    TA_SELECTMASK=0x7, TA_COMPLEMENT=0x10, TA_ALPHAREPLICATE=0x20
};

// ============================================================================
// Key equality / hash
// ============================================================================
bool FFPKey::operator==(const FFPKey& o) const {
    return std::memcmp(this, &o, sizeof(FFPKey)) == 0;
}
std::size_t FFPKey::Hash() const {
    // FNV-1a over the raw bytes. POD with deterministic init, no indeterminate
    // padding because every field is explicitly initialised.
    const auto* p = reinterpret_cast<const uint8_t*>(this);
    std::size_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < sizeof(FFPKey); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// ============================================================================
// Vertex shader
// ============================================================================
std::string FFPShaderCache::GenerateVertexShader(const FFPKey& k) {
    std::string s = "#version 300 es\n";
    s += "precision highp float;\n";

    // Attributes — locations fixed by the device's vertex-attrib binding.
    s += "layout(location=0) in vec4 aPos;\n";
    if (k.hasNormal)   s += "layout(location=1) in vec3 aNormal;\n";
    if (k.hasDiffuse)  s += "layout(location=2) in vec4 aDiffuse;\n";
    if (k.hasSpecular) s += "layout(location=3) in vec4 aSpecular;\n";
    for (int i = 0; i < k.numTexCoords; ++i)
        s += "layout(location=" + std::to_string(4+i) + ") in vec2 aTex" + std::to_string(i) + ";\n";

    s += "uniform mat4 uWorld;\nuniform mat4 uView;\nuniform mat4 uProj;\n";
    if (k.isPretransformed) s += "uniform vec4 uViewport;\n"; // (x,y,w,h) in pixels
    // Per-stage texture-transform matrix, declared only for stages that generate
    // or transform their coords (e.g. projected shadow decals).
    for (int i = 0; i < kMaxStages; ++i)
        if (k.stages[i].colorOp != TOP_DISABLE && (k.stages[i].texGen || k.stages[i].texXform))
            s += "uniform mat4 uTexMatrix" + std::to_string(i) + ";\n";

    // varyings
    s += "out vec4 vColor;\nout vec4 vSpecular;\n";
    for (int i = 0; i < kMaxStages; ++i)
        if (k.stages[i].colorOp != TOP_DISABLE)
            s += "out vec2 vTex" + std::to_string(i) + ";\n";
    if (k.fogEnabled) s += "out float vFogCoord;\n";

    if (!k.isPretransformed) {
        if (k.lightingEnabled) {
            s += "uniform vec4 uMaterialAmbient;\nuniform vec4 uMaterialDiffuse;\n";
            s += "uniform vec4 uMaterialEmissive;\nuniform vec4 uGlobalAmbient;\n";
            s += "uniform vec3 uLightDir[" + std::to_string(kMaxLights) + "];\n";
            s += "uniform vec3 uLightPos[" + std::to_string(kMaxLights) + "];\n";
            s += "uniform vec4 uLightDiffuse[" + std::to_string(kMaxLights) + "];\n";
            s += "uniform vec4 uLightAmbient[" + std::to_string(kMaxLights) + "];\n";
            s += "uniform vec3 uLightAtten[" + std::to_string(kMaxLights) + "];\n"; // const,linear,quad
        }
    }

    // Does any stage generate coords from camera-space position (projected
    // shadows)? If so we hoist the eye-space position to function scope.
    bool needCS = false;
    for (int i = 0; i < kMaxStages; ++i)
        if (k.stages[i].colorOp != TOP_DISABLE && k.stages[i].texGen == 2) needCS = true;

    s += "void main(){\n";
    if (needCS) s += "  vec4 csPos = vec4(0.0,0.0,0.0,1.0);\n";

    // D3DCOLOR vertex attributes arrive BGRA (little-endian DWORD read as 4
    // normalised bytes), so swizzle .bgra to get RGBA the rest of the shader
    // expects. White/black defaults when the FVF omits the colour.
    s += "  vec4 inDiffuse = "  + std::string(k.hasDiffuse  ? "aDiffuse.bgra"  : "vec4(1.0)") + ";\n";
    s += "  vec4 inSpecular = " + std::string(k.hasSpecular ? "aSpecular.bgra" : "vec4(0.0)") + ";\n";

    if (k.isPretransformed) {
        // XYZRHW: aPos.xy are in render-target *pixel* coordinates (D3D screen
        // space, origin top-left, y down) and aPos.z is depth in [0,1]; .w is 1/w.
        // D3D's viewport transform would map these to the framebuffer for us, but
        // GLES3 has no fixed-function viewport stage, so do it here: convert pixels
        // to clip space against the active viewport rect, flip Y for GL's bottom-
        // left origin, and remap depth [0,1] -> GL NDC [-1,1]. Without this the raw
        // pixel coords (e.g. 400,300) are treated as clip coords and every 2D UI
        // quad lands far outside [-1,1] and is clipped away (invisible menus).
        s += "  float _ndcx = (aPos.x - uViewport.x) / uViewport.z * 2.0 - 1.0;\n";
        s += "  float _ndcy = 1.0 - (aPos.y - uViewport.y) / uViewport.w * 2.0;\n";
        s += "  gl_Position = vec4(_ndcx, _ndcy, aPos.z * 2.0 - 1.0, 1.0);\n";
        s += "  vColor = inDiffuse;\n";
        s += "  vSpecular = inSpecular;\n";
    } else {
        s += "  vec4 worldPos = uWorld * aPos;\n";
        s += "  vec4 viewPos = uView * worldPos;\n";
        s += "  gl_Position = uProj * viewPos;\n";
        if (needCS) s += "  csPos = vec4(viewPos.xyz, 1.0);\n";

        if (k.lightingEnabled && k.hasNormal) {
            s += "  vec3 N = normalize(mat3(uWorld) * aNormal);\n";
            // material diffuse source: vertex color if COLORVERTEX, else material
            std::string matDiff = (k.colorVertex && k.hasDiffuse) ? "inDiffuse" : "uMaterialDiffuse";
            s += "  vec4 matDiffuse = " + matDiff + ";\n";
            s += "  vec3 litColor = uGlobalAmbient.rgb*uMaterialAmbient.rgb + uMaterialEmissive.rgb;\n";
            for (int i = 0; i < k.numLights; ++i) {
                std::string li = std::to_string(i);
                if (k.lightType[i] == 3) { // directional
                    s += "  {\n    vec3 L = normalize(-uLightDir["+li+"]);\n";
                    s += "    float ndl = max(dot(N,L),0.0);\n";
                    s += "    litColor += uLightAmbient["+li+"].rgb*uMaterialAmbient.rgb;\n";
                    s += "    litColor += ndl*uLightDiffuse["+li+"].rgb*matDiffuse.rgb;\n  }\n";
                } else { // point (and spot approximated as point)
                    s += "  {\n    vec3 Lv = uLightPos["+li+"] - worldPos.xyz;\n";
                    s += "    float d = length(Lv);\n    vec3 L = Lv/max(d,1e-4);\n";
                    s += "    float att = 1.0/(uLightAtten["+li+"].x + uLightAtten["+li+"].y*d + uLightAtten["+li+"].z*d*d);\n";
                    s += "    float ndl = max(dot(N,L),0.0);\n";
                    s += "    litColor += att*uLightAmbient["+li+"].rgb*uMaterialAmbient.rgb;\n";
                    s += "    litColor += att*ndl*uLightDiffuse["+li+"].rgb*matDiffuse.rgb;\n  }\n";
                }
            }
            s += "  vColor = vec4(litColor, matDiffuse.a);\n";
        } else {
            s += "  vColor = inDiffuse;\n";
        }
        s += "  vSpecular = inSpecular;\n";

        if (k.fogEnabled) {
            // eye-space distance for fog (positive); pixel fog recomputes factor
            // in the fragment shader, vertex fog could fold here. We pass coord.
            s += "  vFogCoord = " + std::string(k.fogRange ? "length(viewPos.xyz)" : "abs(viewPos.z)") + ";\n";
        }
    }

    // texcoords: each enabled stage either passes a vertex coord set through, or
    // generates coords from camera-space position (projected-shadow texgen), then
    // optionally runs them through the stage's texture-transform matrix.
    for (int i = 0; i < kMaxStages; ++i) {
        if (k.stages[i].colorOp == TOP_DISABLE) continue;
        const std::string si = std::to_string(i);
        // 4-component source coordinate.
        std::string base;
        if (k.stages[i].texGen == 2) {            // TCI_CAMERASPACEPOSITION
            base = "csPos";
        } else {                                   // passthru (TCI 0; normal/reflection
                                                    // fall back to passthru — unused here)
            int src = k.stages[i].texCoordIndex;
            if (src >= k.numTexCoords) src = 0;
            base = (k.numTexCoords > 0)
                 ? ("vec4(aTex" + std::to_string(src) + ", 0.0, 1.0)")
                 : "vec4(0.0, 0.0, 0.0, 1.0)";
        }
        // Texture-transform matrix (COUNT2: take .xy of the transformed coord).
        const std::string coord = k.stages[i].texXform
            ? ("(uTexMatrix" + si + " * " + base + ").xy")
            : (base + ".xy");
        s += "  vTex" + si + " = " + coord + ";\n";
    }
    s += "}\n";
    return s;
}

// ============================================================================
// Fragment shader: the texture-stage cascade + fog + alpha test
// ============================================================================

// vec4 source expression for a texture-stage argument (modifiers applied).
static std::string ArgVec4(uint8_t arg, int stage) {
    std::string base;
    switch (arg & TA_SELECTMASK) {
        case TA_DIFFUSE:  base = "vColor"; break;
        case TA_CURRENT:  base = "current"; break;
        case TA_TEXTURE:  base = "tex" + std::to_string(stage); break;
        case TA_TFACTOR:  base = "uTFactor"; break;
        case TA_SPECULAR: base = "vSpecular"; break;
        default:          base = "current"; break;
    }
    if (arg & TA_ALPHAREPLICATE) base = "vec4(" + base + ".a)";
    if (arg & TA_COMPLEMENT)     base = "(vec4(1.0) - " + base + ")";
    return base;
}

// Emit the GLSL expression for a texture op. 'comp' is ".rgb" (vec3) or ".a"
// (float) so the same op table serves the colour and alpha pipelines.
static std::string OpExpr(uint8_t op, const std::string& a1, const std::string& a2,
                          const std::string& comp, int stage) {
    auto A1 = a1 + comp;
    auto A2 = a2 + comp;
    switch (op) {
        case TOP_SELECTARG1:  return A1;
        case TOP_SELECTARG2:  return A2;
        case TOP_MODULATE:    return A1 + "*" + A2;
        case TOP_MODULATE2X:  return "(" + A1 + "*" + A2 + ")*2.0";
        case TOP_MODULATE4X:  return "(" + A1 + "*" + A2 + ")*4.0";
        case TOP_ADD:         return A1 + "+" + A2;
        case TOP_ADDSIGNED:   return "(" + A1 + "+" + A2 + "-0.5)";
        case TOP_ADDSIGNED2X: return "((" + A1 + "+" + A2 + "-0.5)*2.0)";
        case TOP_SUBTRACT:    return A1 + "-" + A2;
        case TOP_ADDSMOOTH:   return "(" + A1 + "+" + A2 + "-" + A1 + "*" + A2 + ")";
        case TOP_BLENDDIFFUSEALPHA: return "mix(" + A2 + "," + A1 + ",vColor.a)";
        case TOP_BLENDTEXTUREALPHA: return "mix(" + A2 + "," + A1 + ",tex" + std::to_string(stage) + ".a)";
        case TOP_BLENDTEXTUREALPHAPM: return "(" + A1 + "+" + A2 + "*(1.0-tex" + std::to_string(stage) + ".a))";
        case TOP_BLENDFACTORALPHA:  return "mix(" + A2 + "," + A1 + ",uTFactor.a)";
        case TOP_BLENDCURRENTALPHA: return "mix(" + A2 + "," + A1 + ",current.a)";
        case TOP_DOTPRODUCT3:
            // result replicated across channels; only meaningful for .rgb
            return (comp == ".a")
                ? "clamp(4.0*dot(" + a1 + ".rgb-0.5," + a2 + ".rgb-0.5),0.0,1.0)"
                : "vec3(clamp(4.0*dot(" + a1 + ".rgb-0.5," + a2 + ".rgb-0.5),0.0,1.0))";
        default:
            // Unhandled op: behave like MODULATE (safe, visible) — see STATUS.md.
            return A1 + "*" + A2;
    }
}

std::string FFPShaderCache::GenerateFragmentShader(const FFPKey& k) {
    std::string s = "#version 300 es\n";
    s += "precision highp float;\n";
    s += "in vec4 vColor;\nin vec4 vSpecular;\n";
    for (int i = 0; i < kMaxStages; ++i)
        if (k.stages[i].colorOp != TOP_DISABLE)
            s += "in vec2 vTex" + std::to_string(i) + ";\n";
    if (k.fogEnabled) s += "in float vFogCoord;\n";

    for (int i = 0; i < kMaxStages; ++i)
        if (k.stages[i].colorOp != TOP_DISABLE && k.stages[i].textureBound)
            s += "uniform sampler2D uSampler" + std::to_string(i) + ";\n";

    s += "uniform vec4 uTFactor;\n";
    if (k.fogEnabled) s += "uniform vec4 uFogColor;\nuniform float uFogStart;\nuniform float uFogEnd;\nuniform float uFogDensity;\n";
    if (k.alphaTestEnabled) s += "uniform float uAlphaRef;\n";

    s += "out vec4 fragColor;\n";
    s += "void main(){\n";
    s += "  vec4 current = vColor;\n";

    for (int i = 0; i < kMaxStages; ++i) {
        const StageKey& st = k.stages[i];
        if (st.colorOp == TOP_DISABLE) break; // cascade stops at first disabled stage
        std::string si = std::to_string(i);
        if (st.textureBound)
            s += "  vec4 tex" + si + " = texture(uSampler" + si + ", vTex" + si + ");\n";
        else
            s += "  vec4 tex" + si + " = vec4(1.0);\n";

        // colour pipe
        std::string c = OpExpr(st.colorOp, ArgVec4(st.colorArg1,i), ArgVec4(st.colorArg2,i), ".rgb", i);
        // alpha pipe (SELECTARG1 default if alphaOp DISABLE-but-color-active)
        uint8_t aop = (st.alphaOp == TOP_DISABLE) ? TOP_SELECTARG1 : st.alphaOp;
        std::string a = OpExpr(aop, ArgVec4(st.alphaArg1,i), ArgVec4(st.alphaArg2,i), ".a", i);
        s += "  current = vec4(clamp(" + c + ",0.0,1.0), clamp(" + a + ",0.0,1.0));\n";
    }

    // specular add (D3D adds specular after the texture cascade)
    s += "  current.rgb += vSpecular.rgb;\n";

    if (k.alphaTestEnabled && k.alphaTestFunc != 8 /*ALWAYS*/) {
        const char* cmp = "<";
        switch (k.alphaTestFunc) {
            case 1: s += "  discard;\n"; break;                 // NEVER
            case 2: cmp = ">="; break;                          // LESS: keep a<ref -> discard a>=ref
            case 3: cmp = "!="; break;                          // EQUAL
            case 4: cmp = ">";  break;                          // LESSEQUAL
            case 5: cmp = "<="; break;                          // GREATER
            case 6: cmp = "=="; break;                          // NOTEQUAL
            case 7: cmp = "<";  break;                          // GREATEREQUAL
        }
        if (k.alphaTestFunc != 1)
            s += "  if (current.a " + std::string(cmp) + " uAlphaRef) discard;\n";
    }

    if (k.fogEnabled) {
        if (k.fogMode == 2)      // EXP
            s += "  float f = exp(-uFogDensity*vFogCoord);\n";
        else if (k.fogMode == 3) // EXP2
            s += "  float f = exp(-(uFogDensity*vFogCoord)*(uFogDensity*vFogCoord));\n";
        else                     // LINEAR
            s += "  float f = (uFogEnd - vFogCoord)/(uFogEnd - uFogStart);\n";
        s += "  f = clamp(f,0.0,1.0);\n";
        s += "  current.rgb = mix(uFogColor.rgb, current.rgb, f);\n";
    }

    s += "  fragColor = current;\n";
    s += "}\n";
    return s;
}

// ============================================================================
// Compile / link / cache
// ============================================================================
static GLuint CompileStage(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* p = src.c_str();
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0;
        glGetShaderInfoLog(sh, sizeof(log), &n, log);
        std::fprintf(stderr, "[d3d8gles3] FFP %s compile failed:\n%s\nSOURCE:\n%s\n",
                     type == GL_VERTEX_SHADER ? "VS" : "FS", log, src.c_str());
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

const FFPProgram* FFPShaderCache::GetProgram(const FFPKey& key) {
    auto it = cache_.find(key);
    if (it != cache_.end()) return &it->second;

    GLuint vs = CompileStage(GL_VERTEX_SHADER,   GenerateVertexShader(key));
    GLuint fs = CompileStage(GL_FRAGMENT_SHADER, GenerateFragmentShader(key));
    if (!vs || !fs) { if (vs) glDeleteShader(vs); if (fs) glDeleteShader(fs); return nullptr; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048]; GLsizei n = 0; glGetProgramInfoLog(prog, sizeof(log), &n, log);
        std::fprintf(stderr, "[d3d8gles3] FFP link failed:\n%s\n", log);
        glDeleteProgram(prog);
        return nullptr;
    }

    FFPProgram fp;
    fp.program = prog;
    fp.uWorld = glGetUniformLocation(prog, "uWorld");
    fp.uView  = glGetUniformLocation(prog, "uView");
    fp.uProj  = glGetUniformLocation(prog, "uProj");
    fp.uViewport = glGetUniformLocation(prog, "uViewport");
    fp.uMaterialAmbient  = glGetUniformLocation(prog, "uMaterialAmbient");
    fp.uMaterialDiffuse  = glGetUniformLocation(prog, "uMaterialDiffuse");
    fp.uMaterialEmissive = glGetUniformLocation(prog, "uMaterialEmissive");
    fp.uGlobalAmbient    = glGetUniformLocation(prog, "uGlobalAmbient");
    fp.uTFactor   = glGetUniformLocation(prog, "uTFactor");
    fp.uLightDir  = glGetUniformLocation(prog, "uLightDir");
    fp.uLightPos  = glGetUniformLocation(prog, "uLightPos");
    fp.uLightDiffuse = glGetUniformLocation(prog, "uLightDiffuse");
    fp.uLightAmbient = glGetUniformLocation(prog, "uLightAmbient");
    fp.uLightAtten   = glGetUniformLocation(prog, "uLightAtten");
    fp.uFogColor   = glGetUniformLocation(prog, "uFogColor");
    fp.uFogStart   = glGetUniformLocation(prog, "uFogStart");
    fp.uFogEnd     = glGetUniformLocation(prog, "uFogEnd");
    fp.uFogDensity = glGetUniformLocation(prog, "uFogDensity");
    fp.uAlphaRef   = glGetUniformLocation(prog, "uAlphaRef");
    for (int i = 0; i < kMaxStages; ++i) {
        std::string n = "uSampler" + std::to_string(i);
        fp.uSampler[i] = glGetUniformLocation(prog, n.c_str());
        std::string tm = "uTexMatrix" + std::to_string(i);
        fp.uTexMatrix[i] = glGetUniformLocation(prog, tm.c_str());
    }

    auto res = cache_.emplace(key, fp);
    return &res.first->second;
}

void FFPShaderCache::Clear() {
    for (auto& kv : cache_) if (kv.second.program) glDeleteProgram(kv.second.program);
    cache_.clear();
}

} // namespace d3d8gles3
