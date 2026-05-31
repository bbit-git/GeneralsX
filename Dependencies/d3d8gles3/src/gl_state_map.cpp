/*
** d3d8gles3 — D3D8 enum -> GLES3 enum translation (see gl_state_map.h).
*/
#include "gl_state_map.h"

// DXT internal formats come from GL_EXT_texture_compression_s3tc. The enum
// values are stable regardless of whether the extension is present at runtime;
// MapTextureFormat reports compressed==true and the device checks the extension
// before relying on it (falling back to CPU decompression otherwise).
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT   0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT  0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT  0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT  0x83F3
#endif

namespace d3d8gles3 {

// ---- D3DBLEND (1..17) -------------------------------------------------------
enum {
    D3DBLEND_ZERO=1, D3DBLEND_ONE=2, D3DBLEND_SRCCOLOR=3, D3DBLEND_INVSRCCOLOR=4,
    D3DBLEND_SRCALPHA=5, D3DBLEND_INVSRCALPHA=6, D3DBLEND_DESTALPHA=7,
    D3DBLEND_INVDESTALPHA=8, D3DBLEND_DESTCOLOR=9, D3DBLEND_INVDESTCOLOR=10,
    D3DBLEND_SRCALPHASAT=11, D3DBLEND_BOTHSRCALPHA=12, D3DBLEND_BOTHINVSRCALPHA=13
};

GLenum MapBlendFactor(uint32_t b) {
    switch (b) {
        case D3DBLEND_ZERO:          return GL_ZERO;
        case D3DBLEND_ONE:           return GL_ONE;
        case D3DBLEND_SRCCOLOR:      return GL_SRC_COLOR;
        case D3DBLEND_INVSRCCOLOR:   return GL_ONE_MINUS_SRC_COLOR;
        case D3DBLEND_SRCALPHA:      return GL_SRC_ALPHA;
        case D3DBLEND_INVSRCALPHA:   return GL_ONE_MINUS_SRC_ALPHA;
        case D3DBLEND_DESTALPHA:     return GL_DST_ALPHA;
        case D3DBLEND_INVDESTALPHA:  return GL_ONE_MINUS_DST_ALPHA;
        case D3DBLEND_DESTCOLOR:     return GL_DST_COLOR;
        case D3DBLEND_INVDESTCOLOR:  return GL_ONE_MINUS_DST_COLOR;
        case D3DBLEND_SRCALPHASAT:   return GL_SRC_ALPHA_SATURATE;
        // BOTHSRCALPHA / BOTHINVSRCALPHA set src AND dst together; the device
        // expands these into a glBlendFuncSeparate pair. Return the src side.
        case D3DBLEND_BOTHSRCALPHA:    return GL_SRC_ALPHA;
        case D3DBLEND_BOTHINVSRCALPHA: return GL_ONE_MINUS_SRC_ALPHA;
        default:                     return GL_ONE;
    }
}

// ---- D3DBLENDOP (1..5) ------------------------------------------------------
GLenum MapBlendOp(uint32_t op) {
    switch (op) {
        case 1: return GL_FUNC_ADD;
        case 2: return GL_FUNC_SUBTRACT;
        case 3: return GL_FUNC_REVERSE_SUBTRACT;
        case 4: return GL_MIN;
        case 5: return GL_MAX;
        default: return GL_FUNC_ADD;
    }
}

// ---- D3DCMPFUNC (1..8) ------------------------------------------------------
GLenum MapCompareFunc(uint32_t c) {
    switch (c) {
        case 1: return GL_NEVER;
        case 2: return GL_LESS;
        case 3: return GL_EQUAL;
        case 4: return GL_LEQUAL;
        case 5: return GL_GREATER;
        case 6: return GL_NOTEQUAL;
        case 7: return GL_GEQUAL;
        case 8: return GL_ALWAYS;
        default: return GL_ALWAYS;
    }
}

// ---- D3DSTENCILOP (1..8) ----------------------------------------------------
GLenum MapStencilOp(uint32_t s) {
    switch (s) {
        case 1: return GL_KEEP;
        case 2: return GL_ZERO;
        case 3: return GL_REPLACE;
        case 4: return GL_INCR;        // INCRSAT (clamped) -> GL_INCR
        case 5: return GL_DECR;        // DECRSAT (clamped) -> GL_DECR
        case 6: return GL_INVERT;
        case 7: return GL_INCR_WRAP;   // INCR (wrap)
        case 8: return GL_DECR_WRAP;   // DECR (wrap)
        default: return GL_KEEP;
    }
}

// ---- D3DCULL (1..3) ---------------------------------------------------------
void MapCull(uint32_t cull, bool& enable, GLenum& face) {
    switch (cull) {
        case 1: enable = false; face = GL_BACK; return;   // D3DCULL_NONE
        case 2: enable = true;  face = GL_BACK; return;   // D3DCULL_CW
        case 3: enable = true;  face = GL_FRONT; return;  // D3DCULL_CCW
        default: enable = false; face = GL_BACK; return;
    }
}

// ---- D3DPRIMITIVETYPE (1..6) ------------------------------------------------
GLenum MapPrimitiveType(uint32_t p) {
    switch (p) {
        case 1: return GL_POINTS;
        case 2: return GL_LINES;
        case 3: return GL_LINE_STRIP;
        case 4: return GL_TRIANGLES;
        case 5: return GL_TRIANGLE_STRIP;
        case 6: return GL_TRIANGLE_FAN;
        default: return GL_TRIANGLES;
    }
}

uint32_t PrimitiveCountToIndexCount(uint32_t p, uint32_t n) {
    switch (p) {
        case 1: return n;          // points
        case 2: return n * 2;      // line list
        case 3: return n + 1;      // line strip
        case 4: return n * 3;      // triangle list
        case 5: return n + 2;      // triangle strip
        case 6: return n + 2;      // triangle fan
        default: return n * 3;
    }
}

// ---- D3DTEXTUREADDRESS (1..5) -----------------------------------------------
GLenum MapTextureAddress(uint32_t a) {
    switch (a) {
        case 1: return GL_REPEAT;            // WRAP
        case 2: return GL_MIRRORED_REPEAT;   // MIRROR
        case 3: return GL_CLAMP_TO_EDGE;     // CLAMP
        case 4: return GL_CLAMP_TO_EDGE;     // BORDER (no border in GLES3 core)
        case 5: return GL_MIRRORED_REPEAT;   // MIRRORONCE (approx)
        default: return GL_REPEAT;
    }
}

// ---- D3DTEXTUREFILTERTYPE (0..3) --------------------------------------------
// 0=NONE 1=POINT 2=LINEAR 3=ANISOTROPIC
GLenum MapMagFilter(uint32_t f) {
    return (f >= 2) ? GL_LINEAR : GL_NEAREST;
}

GLenum MapMinFilter(uint32_t minF, uint32_t mipF) {
    const bool linMin = (minF >= 2);
    switch (mipF) {
        case 0: // no mip
            return linMin ? GL_LINEAR : GL_NEAREST;
        case 1: // point mip
            return linMin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
        default: // linear/aniso mip
            return linMin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    }
}

// ---- D3DFORMAT --------------------------------------------------------------
// Only the formats WW3D actually creates are mapped; the rest report
// supported=false so the texture loader can pick a fallback.
enum {
    D3DFMT_A8R8G8B8=21, D3DFMT_X8R8G8B8=22, D3DFMT_R5G6B5=23,
    D3DFMT_A1R5G5B5=25, D3DFMT_A4R4G4B4=26, D3DFMT_R8G8B8=20,
    D3DFMT_L8=50, D3DFMT_A8=28, D3DFMT_A8L8=51,
    D3DFMT_DXT1=0x31545844, D3DFMT_DXT3=0x33545844, D3DFMT_DXT5=0x35545844
};

GLTexFormat MapTextureFormat(uint32_t fmt) {
    GLTexFormat t{};
    t.supported = true;
    // Identity swizzle by default (only used when needsBGRASwizzle is set).
    t.swizzle[0] = GL_RED; t.swizzle[1] = GL_GREEN; t.swizzle[2] = GL_BLUE; t.swizzle[3] = GL_ALPHA;
    switch (fmt) {
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            // D3D byte order is BGRA in memory; swap R<->B. X8 has no alpha -> force 1.
            t.internalFormat = GL_RGBA8; t.format = GL_RGBA; t.type = GL_UNSIGNED_BYTE;
            t.needsBGRASwizzle = true;
            t.swizzle[0] = GL_BLUE; t.swizzle[2] = GL_RED;
            if (fmt == D3DFMT_X8R8G8B8) t.swizzle[3] = GL_ONE; break;
        case D3DFMT_R8G8B8:
            t.internalFormat = GL_RGB8; t.format = GL_RGB; t.type = GL_UNSIGNED_BYTE;
            t.needsBGRASwizzle = true;
            t.swizzle[0] = GL_BLUE; t.swizzle[2] = GL_RED; break;
        case D3DFMT_R5G6B5:
            // GL 5_6_5 packing matches D3D R5G6B5 exactly; no swizzle.
            t.internalFormat = GL_RGB565; t.format = GL_RGB; t.type = GL_UNSIGNED_SHORT_5_6_5; break;
        case D3DFMT_A1R5G5B5:
            // NOTE: GL 5_5_5_1 puts the 1-bit alpha at the LSB while D3D A1R5G5B5 puts
            // it at the MSB, so the bit fields don't line up and a channel swizzle can't
            // fully correct it (would need CPU expansion to RGBA8). Left as the legacy
            // R<->B approximation; revisit if a 1555 texture shows wrong colors.
            t.internalFormat = GL_RGB5_A1; t.format = GL_RGBA; t.type = GL_UNSIGNED_SHORT_5_5_5_1;
            t.needsBGRASwizzle = true;
            t.swizzle[0] = GL_BLUE; t.swizzle[2] = GL_RED; break;
        case D3DFMT_A4R4G4B4:
            // GL 4_4_4_4 reads the word as R4 G4 B4 A4 (MSB->LSB); D3D A4R4G4B4 is
            // A4 R4 G4 B4. So GL.{R,G,B,A} = D3D.{A,R,G,B}: rotate to recover RGBA.
            t.internalFormat = GL_RGBA4; t.format = GL_RGBA; t.type = GL_UNSIGNED_SHORT_4_4_4_4;
            t.needsBGRASwizzle = true;
            t.swizzle[0] = GL_GREEN; t.swizzle[1] = GL_BLUE; t.swizzle[2] = GL_ALPHA; t.swizzle[3] = GL_RED; break;
        case D3DFMT_L8:
            t.internalFormat = GL_R8; t.format = GL_RED; t.type = GL_UNSIGNED_BYTE; break;
        case D3DFMT_A8:
            t.internalFormat = GL_R8; t.format = GL_RED; t.type = GL_UNSIGNED_BYTE; break;
        case D3DFMT_A8L8:
            t.internalFormat = GL_RG8; t.format = GL_RG; t.type = GL_UNSIGNED_BYTE; break;
        case D3DFMT_DXT1:
            t.internalFormat = t.format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
            t.compressed = true; break;
        case D3DFMT_DXT3:
            t.internalFormat = t.format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
            t.compressed = true; break;
        case D3DFMT_DXT5:
            t.internalFormat = t.format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
            t.compressed = true; break;
        default:
            t.supported = false; break;
    }
    return t;
}

} // namespace d3d8gles3
