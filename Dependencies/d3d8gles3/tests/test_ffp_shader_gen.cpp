/*
** Host unit tests for the FFP shader generator and enum mapping tables.
** No GL context required: only string generation and pure mapping functions.
*/
#include "ffp_shader_gen.h"
#include "gl_state_map.h"

#include <cassert>
#include <cstdio>
#include <string>

using namespace d3d8gles3;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while(0)
#define CONTAINS(hay, needle) CHECK((hay).find(needle) != std::string::npos)

static FFPKey BasicLitTexturedKey() {
    FFPKey k;
    k.hasPosition = 1; k.hasNormal = 1; k.numTexCoords = 1;
    k.lightingEnabled = 1; k.numLights = 1; k.lightType[0] = 3; // directional
    k.stages[0].colorOp = 4 /*MODULATE*/; k.stages[0].colorArg1 = 2 /*TEXTURE*/;
    k.stages[0].colorArg2 = 0 /*DIFFUSE*/; k.stages[0].textureBound = 1;
    k.stages[0].alphaOp = 4; k.stages[0].alphaArg1 = 2; k.stages[0].alphaArg2 = 0;
    return k;
}

static void TestVertexShader() {
    auto vs = FFPShaderCache::GenerateVertexShader(BasicLitTexturedKey());
    CONTAINS(vs, "#version 300 es");
    CONTAINS(vs, "uProj * viewPos");          // transform path
    CONTAINS(vs, "normalize(mat3(uWorld)");   // normal transform for lighting
    CONTAINS(vs, "uLightDiffuse[0]");         // one directional light unrolled
    CONTAINS(vs, "vTex0 = aTex0");            // texcoord routing
}

static void TestFragmentShader() {
    auto fs = FFPShaderCache::GenerateFragmentShader(BasicLitTexturedKey());
    CONTAINS(fs, "uniform sampler2D uSampler0");
    CONTAINS(fs, "texture(uSampler0, vTex0)");
    CONTAINS(fs, "tex0.rgb*vColor.rgb");      // MODULATE(TEXTURE, DIFFUSE)
    CONTAINS(fs, "fragColor = current");
}

static void TestAlphaTestAndFog() {
    FFPKey k = BasicLitTexturedKey();
    k.alphaTestEnabled = 1; k.alphaTestFunc = 7 /*GREATEREQUAL*/;
    k.fogEnabled = 1; k.fogMode = 1 /*linear*/;
    auto fs = FFPShaderCache::GenerateFragmentShader(k);
    CONTAINS(fs, "discard");                  // alpha-test discard emitted
    CONTAINS(fs, "uFogColor");                // fog blend emitted
    CONTAINS(fs, "mix(uFogColor.rgb");
}

static void TestKeyHashEquality() {
    FFPKey a = BasicLitTexturedKey();
    FFPKey b = BasicLitTexturedKey();
    CHECK(a == b);
    CHECK(a.Hash() == b.Hash());
    b.stages[0].colorOp = 7; // ADD
    CHECK(!(a == b));
}

static void TestEnumMaps() {
    CHECK(MapBlendFactor(5) == GL_SRC_ALPHA);          // D3DBLEND_SRCALPHA
    CHECK(MapBlendFactor(6) == GL_ONE_MINUS_SRC_ALPHA);// D3DBLEND_INVSRCALPHA
    CHECK(MapCompareFunc(4) == GL_LEQUAL);             // D3DCMP_LESSEQUAL
    CHECK(MapPrimitiveType(4) == GL_TRIANGLES);
    CHECK(PrimitiveCountToIndexCount(4, 2) == 6);      // 2 tris -> 6 indices
    CHECK(PrimitiveCountToIndexCount(5, 2) == 4);      // tri strip
    CHECK(MapTextureAddress(1) == GL_REPEAT);
    auto f = MapTextureFormat(21 /*A8R8G8B8*/);
    CHECK(f.supported && f.needsBGRASwizzle && f.format == GL_RGBA);
    auto dxt = MapTextureFormat(0x31545844 /*DXT1*/);
    CHECK(dxt.supported && dxt.compressed);
}

int main() {
    TestVertexShader();
    TestFragmentShader();
    TestAlphaTestAndFog();
    TestKeyHashEquality();
    TestEnumMaps();
    if (g_fail == 0) { std::printf("d3d8gles3: all tests passed\n"); return 0; }
    std::fprintf(stderr, "d3d8gles3: %d checks failed\n", g_fail);
    return 1;
}
