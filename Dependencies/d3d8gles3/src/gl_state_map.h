/*
** d3d8gles3 — Direct3D 8 enum → OpenGL ES 3.0 enum translation.
**
** Pure functions, no GL state touched here; callers feed the results to
** glBlendFunc / glDepthFunc / glStencilOp / glTexParameteri / glDrawElements.
**
** D3D enum numeric values are the canonical ones from the DirectX 8 SDK
** (d3d8types.h); we hardcode the mappings rather than depend on those headers
** so this file is self-contained.
*/
#pragma once

#include <GLES3/gl3.h>
#include <cstdint>

namespace d3d8gles3 {

// --- D3DBLEND -> GL blend factor (glBlendFunc / glBlendFuncSeparate) ---------
GLenum MapBlendFactor(uint32_t d3dBlend);

// --- D3DBLENDOP -> GL blend equation (glBlendEquation) -----------------------
GLenum MapBlendOp(uint32_t d3dBlendOp);

// --- D3DCMPFUNC -> GL compare func (glDepthFunc / glStencilFunc / alpha test) -
GLenum MapCompareFunc(uint32_t d3dCmp);

// --- D3DSTENCILOP -> GL stencil op (glStencilOp) -----------------------------
GLenum MapStencilOp(uint32_t d3dStencilOp);

// --- D3DCULL -> GL cull face; returns false in 'enable' for D3DCULL_NONE ------
// NOTE: D3D and GL disagree on winding under a flipped projection; the chosen
// front-face winding is configured once at device init, not here.
void   MapCull(uint32_t d3dCull, bool& outEnable, GLenum& outCullFace);

// --- D3DPRIMITIVETYPE -> GL primitive mode + index-count-from-primitive-count -
GLenum MapPrimitiveType(uint32_t d3dPrim);
uint32_t PrimitiveCountToIndexCount(uint32_t d3dPrim, uint32_t primCount);

// --- D3DTEXTUREADDRESS -> GL wrap mode (glTexParameteri GL_TEXTURE_WRAP_*) ----
GLenum MapTextureAddress(uint32_t d3dAddress);

// --- D3DTEXTUREFILTERTYPE -> GL min/mag filter -------------------------------
// magFilter ignores mip; minFilter combines base filter with the mip filter.
GLenum MapMagFilter(uint32_t d3dMagFilter);
GLenum MapMinFilter(uint32_t d3dMinFilter, uint32_t d3dMipFilter);

// --- D3DFORMAT -> GL texture format triple (internalFormat, format, type) ----
// 'compressed' is set for DXT formats; in that case 'glFormat' carries the
// GL_COMPRESSED_* enum and internalFormat==glFormat, type is unused.
struct GLTexFormat {
    GLenum internalFormat;
    GLenum format;
    GLenum type;
    bool   compressed;
    bool   needsBGRASwizzle;   // apply swizzle[] below to remap GL channels -> D3D RGBA
    GLenum swizzle[4];         // GL_TEXTURE_SWIZZLE_{R,G,B,A} sources (when needsBGRASwizzle)
    bool   supported;
};
GLTexFormat MapTextureFormat(uint32_t d3dFormat);

} // namespace d3d8gles3
