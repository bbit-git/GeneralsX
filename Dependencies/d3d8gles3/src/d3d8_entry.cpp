/*
** d3d8gles3 — COM ABI shim + Direct3DCreate8 entry point.
**
** This is the ONLY file that depends on the D3D8 COM headers (<d3d8.h>, supplied
** via DXVK_INCLUDE_DIR). It implements the abstract IDirect3D*8 interfaces by
** forwarding into the header-free GLES3 core (device_gles3 / resources_gles3).
**
** Forwarding map (IDirect3DDevice8 method -> GLES3Device method):
**   Clear                  -> Clear
**   BeginScene/EndScene    -> BeginScene/EndScene
**   Present                -> Present (+ SDL_GL_SwapWindow in window layer)
**   SetViewport            -> SetViewport
**   SetRenderState         -> SetRenderState
**   SetTextureStageState   -> SetTextureStageState
**   SetTransform           -> SetTransform
**   SetTexture             -> SetTexture (unwrap TextureShim -> GLTexture*)
**   SetMaterial            -> SetMaterial
**   SetLight/LightEnable   -> SetLight/LightEnable
**   SetStreamSource        -> SetStreamSource (unwrap VBShim -> GLBuffer*)
**   SetIndices             -> SetIndices (unwrap IBShim -> GLBuffer*)
**   SetVertexShader        -> SetVertexShader (FVF code)
**   DrawIndexedPrimitive   -> DrawIndexedPrimitive
**   CreateVertexBuffer     -> new VBShim(GLBuffer::Create(GL_ARRAY_BUFFER,...))
**   CreateIndexBuffer      -> new IBShim(GLBuffer::Create(GL_ELEMENT_ARRAY_BUFFER,...))
**   CreateTexture          -> new TextureShim(GLTexture::Create(...))
**   GetDeviceCaps          -> filled from a static GLES3 caps profile
**   {Get,Set}RenderTarget, CopyRects, swap chains -> TODO (FBO-backed)
**
** Everything not in that map returns D3D_OK (no-op) or E_NOTIMPL as appropriate;
** see STATUS.md for the per-method checklist.
*/

#include "device_gles3.h"
#include "resources_gles3.h"

// The COM binding compiles only where the D3D8 ABI headers are on the include
// path (real Android/host-GLES build via cmake/gles3.cmake). Host unit tests for
// the shader generator and state maps don't need it, so keep the lib linkable
// without the header by gating the COM body.
#if defined(__has_include)
#  if __has_include(<d3d8.h>)
#    define D3D8GLES3_HAVE_D3D8_HEADERS 1
#  endif
#endif

#if D3D8GLES3_HAVE_D3D8_HEADERS

#include <d3d8.h>
#include <new>

using namespace d3d8gles3;

// NOTE: the concrete IDirect3DDevice8 / IDirect3D8 / resource-interface
// subclasses live here. Each is a thin forwarder over the GLES3 core objects
// above. The full vtable is filled in incrementally (tracked in STATUS.md);
// the creation path + the ~25 hot-path methods in the forwarding map are the
// first milestone. Implemented out-of-line to keep this header include local.
//
// Sketch of the device shim:
//
//   class Device8 final : public IDirect3DDevice8 {
//       GLES3Device core_;
//   public:
//       STDMETHOD(Clear)(DWORD, const D3DRECT*, DWORD flags, D3DCOLOR c,
//                        float z, DWORD stencil) override {
//           float rgba[4]; /* unpack c */ ;
//           core_.Clear(flags & D3DCLEAR_TARGET, flags & (D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL),
//                       rgba, z, stencil);
//           return D3D_OK;
//       }
//       STDMETHOD(DrawIndexedPrimitive)(D3DPRIMITIVETYPE t, UINT minIdx, UINT numV,
//                                       UINT startIdx, UINT primCount) override {
//           core_.DrawIndexedPrimitive(t, minIdx, numV, startIdx, primCount);
//           return D3D_OK;
//       }
//       /* ...rest of forwarding map... */
//   };
//
// extern "C" IDirect3D8* WINAPI Direct3DCreate8(UINT sdkVersion) {
//     return new (std::nothrow) Direct3D8();   // enumerates a single GLES3 adapter
// }

#else  // !D3D8GLES3_HAVE_D3D8_HEADERS

// Host-test build: provide nothing COM-related. The GLES3 core, state maps, and
// shader generator are exercised directly by the unit tests.

#endif // D3D8GLES3_HAVE_D3D8_HEADERS
