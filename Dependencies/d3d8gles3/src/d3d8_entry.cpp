/*
** d3d8gles3 — COM ABI shim + Direct3DCreate8 entry point.
**
** The ONLY file that depends on the D3D8 COM headers (<d3d8.h>, supplied via
** DXVK_INCLUDE_DIR -> mingw-directx-headers). It implements the abstract
** IDirect3D*8 interfaces by forwarding into the header-free GLES3 core
** (device_gles3 / resources_gles3). Method signatures are taken verbatim from
** the d3d8.h this build compiles against.
**
** Coverage: the hot-path methods WW3D2 actually issues are implemented; the
** long tail (state blocks, palettes, software shaders, ProcessVertices, gamma)
** returns D3D_OK / D3DERR_INVALIDCALL as appropriate. See STATUS.md.
*/
#include "device_gles3.h"
#include "resources_gles3.h"

// Compile the COM body only where the D3D8 ABI headers are reachable (real
// Android/host-GLES build). Host unit tests for the generator/state-maps don't
// need it, so keep the lib linkable without the header.
#if defined(__has_include)
#  if __has_include(<d3d8.h>)
#    define D3D8GLES3_HAVE_D3D8_HEADERS 1
#  endif
#endif

#if D3D8GLES3_HAVE_D3D8_HEADERS

// On non-Windows, the DXVK native include dir provides a <windows.h> shim that
// defines the Win32 base types (ULONG/DWORD/HRESULT/WINBOOL/REFIID/…) and the
// STDMETHOD/STDMETHODCALLTYPE/WINAPI macros that <d3d8.h> relies on. Include it
// first so the interface declarations resolve regardless of include order.
#include <windows.h>
#include <d3d8.h>
#include <new>
#include <cstring>
#include <cstdlib>

namespace {
using namespace d3d8gles3;

// ---------------------------------------------------------------------------
// Shared IUnknown refcount + IDirect3DResource8 boilerplate
// ---------------------------------------------------------------------------
template <class IFace>
class Unknown : public IFace {
public:
    ULONG STDMETHODCALLTYPE AddRef() override  { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** ppv) override {
        // Single-inheritance layer: every interface shares the IUnknown base, so
        // returning `this` is ABI-correct for the IIDs the engine asks for.
        if (!ppv) return E_POINTER;
        *ppv = this; AddRef(); return S_OK;
    }
    virtual ~Unknown() = default;
protected:
    ULONG ref_ = 1;
};

// IDirect3DResource8 tail shared by buffers/textures/surfaces.
#define RESOURCE_STUBS(DEVRET)                                                            \
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** d) override { *d = DEVRET; return S_OK; } \
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return S_OK; } \
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override { return E_FAIL; }   \
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return S_OK; }                   \
    DWORD   STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }                            \
    DWORD   STDMETHODCALLTYPE GetPriority() override { return 0; }                                 \
    void    STDMETHODCALLTYPE PreLoad() override {}

class Device8;   // fwd
class Surface8;  // fwd

// ---------------------------------------------------------------------------
// Vertex / Index buffers
// ---------------------------------------------------------------------------
class VertexBuffer8 final : public Unknown<IDirect3DVertexBuffer8> {
public:
    VertexBuffer8(Device8* dev, uint32_t bytes, DWORD fvf, bool dynamic)
        : dev_(dev), fvf_(fvf) { buf_.Create(GL_ARRAY_BUFFER, bytes, dynamic); }
    ~VertexBuffer8() override { buf_.Destroy(); }
    GLBuffer* Gl() { return &buf_; }

    RESOURCE_STUBS(reinterpret_cast<IDirect3DDevice8*>(dev_))
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_VERTEXBUFFER; }

    HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, BYTE** ppb, DWORD flags) override {
        *ppb = static_cast<BYTE*>(buf_.Lock(off, size, flags));
        return *ppb ? S_OK : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { buf_.Unlock(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DVERTEXBUFFER_DESC* d) override {
        if (!d) return E_POINTER;
        std::memset(d, 0, sizeof(*d));
        d->Type = D3DRTYPE_VERTEXBUFFER; d->FVF = fvf_; d->Size = buf_.Size();
        return S_OK;
    }
private:
    Device8*  dev_;
    GLBuffer  buf_;
    DWORD     fvf_;
};

class IndexBuffer8 final : public Unknown<IDirect3DIndexBuffer8> {
public:
    IndexBuffer8(Device8* dev, uint32_t bytes, D3DFORMAT fmt, bool dynamic)
        : dev_(dev), fmt_(fmt) {
        buf_.Create(GL_ELEMENT_ARRAY_BUFFER, bytes, dynamic);
        buf_.SetIndex32(fmt == D3DFMT_INDEX32);
    }
    ~IndexBuffer8() override { buf_.Destroy(); }
    GLBuffer* Gl() { return &buf_; }

    RESOURCE_STUBS(reinterpret_cast<IDirect3DDevice8*>(dev_))
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }

    HRESULT STDMETHODCALLTYPE Lock(UINT off, UINT size, BYTE** ppb, DWORD flags) override {
        *ppb = static_cast<BYTE*>(buf_.Lock(off, size, flags));
        return *ppb ? S_OK : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE Unlock() override { buf_.Unlock(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* d) override {
        if (!d) return E_POINTER;
        std::memset(d, 0, sizeof(*d));
        d->Type = D3DRTYPE_INDEXBUFFER; d->Format = fmt_; d->Size = buf_.Size();
        return S_OK;
    }
private:
    Device8*  dev_;
    GLBuffer  buf_;
    D3DFORMAT fmt_;
};

// ---------------------------------------------------------------------------
// Texture (2D). Cube/volume are separate follow-ups (STATUS.md).
// ---------------------------------------------------------------------------
class Texture8 final : public Unknown<IDirect3DTexture8> {
public:
    Texture8(Device8* dev, UINT w, UINT h, UINT levels, D3DFORMAT fmt, bool s3tc)
        : dev_(dev), fmt_(fmt) { tex_.Create(w, h, levels ? levels : 1, fmt, s3tc); }
    ~Texture8() override { tex_.Destroy(); }
    GLTexture* Gl() { return &tex_; }

    RESOURCE_STUBS(reinterpret_cast<IDirect3DDevice8*>(dev_))
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
    DWORD STDMETHODCALLTYPE SetLOD(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetLOD() override { return 0; }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* d) override {
        if (!d) return E_POINTER;
        std::memset(d, 0, sizeof(*d));
        d->Format = fmt_; d->Type = D3DRTYPE_SURFACE;
        d->Width = tex_.Width() >> level; d->Height = tex_.Height() >> level;
        return S_OK;
    }
    // Returns a lightweight Surface8 view onto this texture's mip level. WW3D2
    // uses it to read back a level's D3DSURFACE_DESC (width/height/format) right
    // after loading a texture (TextureClass::Apply_New_Surface) and to lock a
    // level for CPU upload — both of which we can satisfy without a real
    // render-target surface. Defined out-of-line below once Surface8 is visible.
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface8** s) override;
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* lr,
                                       const RECT*, DWORD flags) override {
        if (!lr) return E_POINTER;
        uint32_t pitch = 0;
        lr->pBits = tex_.LockRect(level, pitch, flags);
        lr->Pitch = static_cast<INT>(pitch);
        return lr->pBits ? S_OK : D3DERR_INVALIDCALL;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override { tex_.UnlockRect(level); return S_OK; }
    HRESULT STDMETHODCALLTYPE AddDirtyRect(const RECT*) override { return S_OK; }
private:
    Device8*  dev_;
    GLTexture tex_;
    D3DFORMAT fmt_;
};

// ---------------------------------------------------------------------------
// Surface — a thin view onto one mip level of a Texture8. d3d8gles3 has no
// standalone render-target surfaces; this exists so the WW3D2 paths that fetch
// a texture's surface (to read its desc or lock it for upload) work. GetDesc
// and LockRect/UnlockRect delegate to the owning texture's level methods.
// ---------------------------------------------------------------------------
class Surface8 final : public Unknown<IDirect3DSurface8> {
public:
    // Texture-level view: GetDesc/LockRect delegate to the owning texture.
    Surface8(Device8* dev, Texture8* tex, UINT level)
        : dev_(dev), tex_(tex), level_(level) { if (tex_) tex_->AddRef(); }
    // Standalone surface (back buffer / render target / image surface). There is
    // no GL render-target object behind it; it just reports a description and,
    // if locked, hands out a throwaway scratch buffer so CPU read-back/copy
    // paths don't crash. width/height/format come from the caller.
    Surface8(Device8* dev, UINT w, UINT h, D3DFORMAT fmt)
        : dev_(dev), w_(w), h_(h), fmt_(fmt) {}
    ~Surface8() override { if (tex_) tex_->Release(); std::free(scratch_); }

    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice8** d) override {
        *d = reinterpret_cast<IDirect3DDevice8*>(dev_); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, const void*, DWORD, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override { return E_FAIL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetContainer(REFIID, void** ppc) override {
        if (!ppc) return E_POINTER;
        *ppc = static_cast<IDirect3DTexture8*>(tex_);
        if (tex_) tex_->AddRef();
        return tex_ ? S_OK : E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* d) override {
        if (tex_) return tex_->GetLevelDesc(level_, d);
        if (!d) return E_POINTER;
        std::memset(d, 0, sizeof(*d));
        d->Format = fmt_; d->Type = D3DRTYPE_SURFACE;
        d->Width = w_; d->Height = h_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* lr, const RECT* r, DWORD flags) override {
        if (tex_) return tex_->LockRect(level_, lr, r, flags);
        if (!lr) return E_POINTER;
        // Hand out a zeroed scratch buffer sized to the surface (4 bpp upper bound).
        const size_t pitch = static_cast<size_t>(w_) * 4u;
        if (!scratch_) scratch_ = std::calloc(pitch ? pitch : 4u, h_ ? h_ : 1u);
        lr->pBits = scratch_;
        lr->Pitch = static_cast<INT>(pitch);
        return scratch_ ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect() override {
        return (tex_) ? tex_->UnlockRect(level_) : S_OK;
    }
private:
    Device8*  dev_   = nullptr;
    Texture8* tex_   = nullptr;
    UINT      level_ = 0;
    UINT      w_ = 0, h_ = 0;
    D3DFORMAT fmt_ = D3DFMT_A8R8G8B8;
    void*     scratch_ = nullptr;
};

HRESULT STDMETHODCALLTYPE Texture8::GetSurfaceLevel(UINT level, IDirect3DSurface8** s) {
    if (!s) return E_POINTER;
    *s = nullptr;
    // Reject levels past the mip count. Callers like D3DXFilterTexture walk levels
    // (GetSurfaceLevel(1), (2), ...) until this *fails* to find the end of the mip
    // chain — vending a surface for every level would spin forever.
    if (level >= GetLevelCount()) return D3DERR_INVALIDCALL;
    *s = new (std::nothrow) Surface8(dev_, this, level);
    return *s ? S_OK : E_OUTOFMEMORY;
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------
class Device8 final : public Unknown<IDirect3DDevice8> {
public:
    Device8(IDirect3D8* parent, const D3DPRESENT_PARAMETERS& pp, HWND focusWindow) : parent_(parent) {
        int w = pp.BackBufferWidth  ? static_cast<int>(pp.BackBufferWidth)  : 800;
        int h = pp.BackBufferHeight ? static_cast<int>(pp.BackBufferHeight) : 600;
        // The HWND is the SDL_Window* on the SDL3 ports. Prefer the present
        // params' device window, else the CreateDevice focus window. Present()
        // uses it for SDL_GL_SwapWindow.
        void* win = pp.hDeviceWindow ? reinterpret_cast<void*>(pp.hDeviceWindow)
                                     : reinterpret_cast<void*>(focusWindow);
        core_.SetSwapWindow(win);
        core_.Init(w, h);
        s3tc_ = false; // queried inside core_.Init via GL extensions
        bbW_ = static_cast<UINT>(w);
        bbH_ = static_cast<UINT>(h);
        bbFmt_ = pp.BackBufferFormat ? pp.BackBufferFormat : D3DFMT_A8R8G8B8;
    }
    ~Device8() override { core_.Shutdown(); }

    // ---- frame ----
    HRESULT STDMETHODCALLTYPE BeginScene() override { core_.BeginScene(); return S_OK; }
    HRESULT STDMETHODCALLTYPE EndScene() override   { core_.EndScene();   return S_OK; }
    HRESULT STDMETHODCALLTYPE Clear(DWORD, const D3DRECT*, DWORD flags, D3DCOLOR c,
                                    float z, DWORD stencil) override {
        float rgba[4] = { ((c>>16)&0xFF)/255.f, ((c>>8)&0xFF)/255.f,
                          (c&0xFF)/255.f, ((c>>24)&0xFF)/255.f };
        core_.Clear(flags & D3DCLEAR_TARGET,
                    (flags & (D3DCLEAR_ZBUFFER|D3DCLEAR_STENCIL)) != 0, rgba, z, stencil);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Present(const RECT*, const RECT*, HWND, const RGNDATA*) override {
        core_.Present(); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT8* vp) override {
        if (vp) core_.SetViewport(vp->X, vp->Y, vp->Width, vp->Height, vp->MinZ, vp->MaxZ);
        return S_OK;
    }

    // ---- state ----
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE s, DWORD v) override {
        core_.SetRenderState(s, v); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD st, D3DTEXTURESTAGESTATETYPE t, DWORD v) override {
        core_.SetTextureStageState(st, t, v); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE t, const D3DMATRIX* m) override {
        if (m) core_.SetTransform(t, &m->_11); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL8* m) override {
        if (m) core_.SetMaterial(m); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD i, const D3DLIGHT8* l) override {
        if (l) core_.SetLight(i, l); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD i, WINBOOL e) override {
        core_.LightEnable(i, e != 0); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture8* t) override {
        core_.SetTexture(stage, t ? static_cast<Texture8*>(t)->Gl() : nullptr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(DWORD handle) override {
        core_.SetVertexShader(handle); return S_OK; // engine passes an FVF code
    }
    HRESULT STDMETHODCALLTYPE SetStreamSource(UINT s, IDirect3DVertexBuffer8* vb, UINT stride) override {
        core_.SetStreamSource(s, vb ? static_cast<VertexBuffer8*>(vb)->Gl() : nullptr, stride);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer8* ib, UINT base) override {
        core_.SetIndices(ib ? static_cast<IndexBuffer8*>(ib)->Gl() : nullptr, base);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE t, UINT minIdx, UINT numV,
                                                   UINT startIdx, UINT primCount) override {
        core_.DrawIndexedPrimitive(t, minIdx, numV, startIdx, primCount); return S_OK;
    }

    // ---- resource creation ----
    HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT len, DWORD usage, DWORD fvf, D3DPOOL,
                                                 IDirect3DVertexBuffer8** out) override {
        *out = new (std::nothrow) VertexBuffer8(this, len, fvf, (usage & D3DUSAGE_DYNAMIC) != 0);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL,
                                                IDirect3DIndexBuffer8** out) override {
        *out = new (std::nothrow) IndexBuffer8(this, len, fmt, (usage & D3DUSAGE_DYNAMIC) != 0);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels, DWORD, D3DFORMAT fmt,
                                            D3DPOOL, IDirect3DTexture8** out) override {
        *out = new (std::nothrow) Texture8(this, w, h, levels, fmt, s3tc_);
        return *out ? S_OK : E_OUTOFMEMORY;
    }

    // ---- caps / queries the engine reads ----
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS8* c) override { FillCaps(c); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D8** d) override { *d = parent_; return S_OK; }
    HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return S_OK; }
    UINT    STDMETHODCALLTYPE GetAvailableTextureMem() override { return 256u*1024*1024; }
    HRESULT STDMETHODCALLTYPE ResourceManagerDiscardBytes(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD* passes) override { if (passes) *passes = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE* m) override {
        if (m) { std::memset(m,0,sizeof(*m)); m->Format = D3DFMT_X8R8G8B8; }
        return S_OK;
    }

    static void FillCaps(D3DCAPS8* c) {
        if (!c) return;
        std::memset(c, 0, sizeof(*c));
        c->DeviceType = D3DDEVTYPE_HAL;
        c->MaxTextureWidth = c->MaxTextureHeight = 4096;
        c->MaxTextureBlendStages   = kMaxStages;
        c->MaxSimultaneousTextures = kMaxStages;
        c->MaxActiveLights = kMaxLights;
        c->MaxStreams = 2;
        c->VertexShaderVersion = 0;   // fixed-function only
        c->PixelShaderVersion  = 0;
        c->TextureCaps   = D3DPTEXTURECAPS_ALPHA | D3DPTEXTURECAPS_MIPMAP;
        c->RasterCaps    = D3DPRASTERCAPS_FOGVERTEX | D3DPRASTERCAPS_FOGTABLE | D3DPRASTERCAPS_ZTEST;
        c->ZCmpCaps      = 0xFF;      // all compare funcs
        c->SrcBlendCaps  = 0x1FFF;    // all blend factors
        c->DestBlendCaps = 0x1FFF;
        c->AlphaCmpCaps  = 0xFF;
        c->StencilCaps   = 0xFF;
        c->TextureAddressCaps = D3DPTADDRESSCAPS_WRAP | D3DPTADDRESSCAPS_MIRROR | D3DPTADDRESSCAPS_CLAMP;
        c->TextureFilterCaps  = D3DPTFILTERCAPS_MINFLINEAR | D3DPTFILTERCAPS_MAGFLINEAR | D3DPTFILTERCAPS_MIPFLINEAR;
    }

    // ---- long tail: safe no-ops / not-implemented (see STATUS.md) ----
    HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* p) override { if (p) std::memset(p,0,sizeof(*p)); return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT, UINT, IDirect3DSurface8*) override { return S_OK; }
    void    STDMETHODCALLTYPE SetCursorPosition(UINT, UINT, DWORD) override {}
    WINBOOL STDMETHODCALLTYPE ShowCursor(WINBOOL) override { return FALSE; }
    HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS*, IDirect3DSwapChain8** s) override { if (s) *s = nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS*) override { core_.OnContextLost(); return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface8** s) override {
        if (!s) return E_POINTER;
        *s = new (std::nothrow) Surface8(this, bbW_, bbH_, bbFmt_);
        return *s ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS* r) override { if (r) std::memset(r,0,sizeof(*r)); return S_OK; }
    void    STDMETHODCALLTYPE SetGammaRamp(DWORD, const D3DGAMMARAMP*) override {}
    void    STDMETHODCALLTYPE GetGammaRamp(D3DGAMMARAMP*) override {}
    HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DVolumeTexture8** o) override { if(o)*o=nullptr; return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DCubeTexture8** o) override { if(o)*o=nullptr; return D3DERR_INVALIDCALL; }
    // Standalone surfaces (render target / depth-stencil / lockable image). No GL
    // render-target object backs them; they report a description and hand out a
    // CPU scratch buffer when locked. That satisfies WW3D paths that allocate an
    // offscreen surface and read/write its pixels (e.g. the fog-of-war shroud's
    // _Create_DX8_Surface src surface) without a real RT.
    HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT w,UINT h,D3DFORMAT fmt,D3DMULTISAMPLE_TYPE,WINBOOL,IDirect3DSurface8** o) override {
        if (!o) return E_POINTER;
        *o = new (std::nothrow) Surface8(this, w, h, fmt);
        return *o ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT w,UINT h,D3DFORMAT fmt,D3DMULTISAMPLE_TYPE,IDirect3DSurface8** o) override {
        if (!o) return E_POINTER;
        *o = new (std::nothrow) Surface8(this, w, h, fmt);
        return *o ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE CreateImageSurface(UINT w,UINT h,D3DFORMAT fmt,IDirect3DSurface8** o) override {
        if (!o) return E_POINTER;
        *o = new (std::nothrow) Surface8(this, w, h, fmt);
        return *o ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE CopyRects(IDirect3DSurface8*, const RECT*, UINT, IDirect3DSurface8*, const POINT*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture8*, IDirect3DBaseTexture8*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetFrontBuffer(IDirect3DSurface8*) override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetRenderTarget(IDirect3DSurface8*, IDirect3DSurface8*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRenderTarget(IDirect3DSurface8** s) override {
        if (!s) return E_POINTER;
        *s = new (std::nothrow) Surface8(this, bbW_, bbH_, bbFmt_);
        return *s ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface8** s) override {
        if (!s) return E_POINTER;
        *s = new (std::nothrow) Surface8(this, bbW_, bbH_, D3DFMT_D24S8);
        return *s ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE, D3DMATRIX*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE, const D3DMATRIX*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT8*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL8*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetLight(DWORD, D3DLIGHT8*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD, WINBOOL* e) override { if (e) *e = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD, const float*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD, float*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE, DWORD* v) override { if (v) *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE EndStateBlock(DWORD* t) override { if (t) *t = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE ApplyStateBlock(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CaptureStateBlock(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteStateBlock(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE, DWORD* t) override { if (t) *t = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetClipStatus(const D3DCLIPSTATUS8*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS8*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTexture(DWORD, IDirect3DBaseTexture8** t) override { if (t) *t = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD, D3DTEXTURESTAGESTATETYPE, DWORD* v) override { if (v) *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetInfo(DWORD, void*, DWORD) override { return S_FALSE; }
    HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT, const PALETTEENTRY*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT, PALETTEENTRY*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT* p) override { if (p) *p = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE t, UINT startVertex, UINT primCount) override {
        core_.DrawPrimitive(t, startVertex, primCount); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE t, UINT primCount,
                                              const void* vtx, UINT stride) override {
        core_.DrawPrimitiveUP(t, primCount, vtx, stride); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE t, UINT minVtxIdx, UINT numVtx,
                                                     UINT primCount, const void* idx, D3DFORMAT idxFmt,
                                                     const void* vtx, UINT stride) override {
        core_.DrawIndexedPrimitiveUP(t, minVtxIdx, numVtx, primCount, idx, idxFmt, vtx, stride); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ProcessVertices(UINT, UINT, UINT, IDirect3DVertexBuffer8*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CreateVertexShader(const DWORD*, const DWORD*, DWORD*, DWORD) override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE GetVertexShader(DWORD* h) override { if (h) *h = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE DeleteVertexShader(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstant(DWORD, const void*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderConstant(DWORD, void*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderDeclaration(DWORD, void*, DWORD*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetVertexShaderFunction(DWORD, void*, DWORD*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetStreamSource(UINT, IDirect3DVertexBuffer8** vb, UINT* s) override { if (vb) *vb = nullptr; if (s) *s = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer8** ib, UINT* b) override { if (ib) *ib = nullptr; if (b) *b = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE CreatePixelShader(const DWORD*, DWORD*) override { return D3DERR_INVALIDCALL; }
    HRESULT STDMETHODCALLTYPE SetPixelShader(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelShader(DWORD* h) override { if (h) *h = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE DeletePixelShader(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstant(DWORD, const void*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderConstant(DWORD, void*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelShaderFunction(DWORD, void*, DWORD*) override { return S_OK; }

    // Higher-order surface patches — unused by WW3D2; no-op to complete the vtable.
    HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT, const float*, const D3DRECTPATCH_INFO*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT, const float*, const D3DTRIPATCH_INFO*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DeletePatch(UINT) override { return S_OK; }

private:
    IDirect3D8* parent_;
    GLES3Device core_;
    bool        s3tc_ = false;
    UINT        bbW_ = 0, bbH_ = 0;
    D3DFORMAT   bbFmt_ = D3DFMT_A8R8G8B8;
};

// ---------------------------------------------------------------------------
// IDirect3D8 — single GLES3 adapter
// ---------------------------------------------------------------------------
class Direct3D8 final : public Unknown<IDirect3D8> {
public:
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void*) override { return D3DERR_INVALIDCALL; }
    UINT    STDMETHODCALLTYPE GetAdapterCount() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT, DWORD, D3DADAPTER_IDENTIFIER8* id) override {
        if (!id) return E_POINTER;
        std::memset(id, 0, sizeof(*id));
        std::strncpy(id->Driver,      "d3d8gles3",            sizeof(id->Driver)-1);
        std::strncpy(id->Description, "OpenGL ES 3.0 (d3d8gles3)", sizeof(id->Description)-1);
        return S_OK;
    }
    UINT    STDMETHODCALLTYPE GetAdapterModeCount(UINT) override { return 1; }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT, UINT, D3DDISPLAYMODE* m) override {
        if (m) { m->Width = 1920; m->Height = 1080; m->RefreshRate = 60; m->Format = D3DFMT_X8R8G8B8; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT, D3DDISPLAYMODE* m) override {
        if (m) { m->Width = 1920; m->Height = 1080; m->RefreshRate = 60; m->Format = D3DFMT_X8R8G8B8; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, WINBOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT, D3DDEVTYPE, D3DFORMAT, WINBOOL, D3DMULTISAMPLE_TYPE) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT, D3DDEVTYPE, D3DCAPS8* c) override { Device8::FillCaps(c); return S_OK; }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT) override { return nullptr; }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT, D3DDEVTYPE, HWND hFocusWindow, DWORD,
                                           D3DPRESENT_PARAMETERS* pp,
                                           IDirect3DDevice8** out) override {
        if (!out || !pp) return E_POINTER;
        *out = new (std::nothrow) Device8(this, *pp, hFocusWindow);
        return *out ? S_OK : E_OUTOFMEMORY;
    }
};

} // anonymous namespace

extern "C" IDirect3D8* WINAPI Direct3DCreate8(UINT /*SDKVersion*/) {
    return new (std::nothrow) Direct3D8();
}

#endif // D3D8GLES3_HAVE_D3D8_HEADERS
