# d3d8gles3 — Direct3D 8 → OpenGL ES 3.0 translation backend

A drop-in alternative to DXVK for platforms where DXVK is impractical — primarily
**Android** (mobile Vulkan drivers frequently lack `VK_EXT_transform_feedback`,
`VK_KHR_maintenance5/6`, and full Vulkan 1.3 that DXVK 2.x requires), and any device
with only a GLES3 driver.

## Why this and not "rewrite dx8wrapper.cpp"

The WW3D2 engine (and `dx8wrapper.cpp`) is written against the **standard Direct3D 8
COM ABI**: `IDirect3D8`, `IDirect3DDevice8`, `IDirect3DTexture8`,
`IDirect3DVertexBuffer8`, `IDirect3DIndexBuffer8`, `IDirect3DSurface8`, etc. On
Linux/macOS, GeneralsX satisfies that ABI by linking DXVK (D3D8→Vulkan).

So the cheapest, most maintainable "GLES3 path" is **not** to edit `dx8wrapper.cpp` at
all — it is to provide a *second implementation of the same ABI*, backed by GLES3, and
plug it in at the single `Direct3DCreate8()` seam. Everything above the seam
(`dx8wrapper.cpp`, `dx8vertexbuffer.cpp`, `texture.cpp`, the whole renderer) stays
byte-for-byte unchanged.

```
WW3D2 / dx8wrapper.cpp   (unchanged)
        │  IDirect3DDevice8::DrawIndexedPrimitive(), SetRenderState(), ...
        ▼
┌───────────────────────────────────────────────┐
│  backend selected at build time (one of):      │
│    • DXVK            → Vulkan   (desktop)       │
│    • d3d8gles3 (THIS)→ GLES 3.0 (Android/mobile)│
└───────────────────────────────────────────────┘
```

## The ABI contract

We reuse the **same `d3d8.h` declarations** the rest of the tree already compiles
against (the Wine/DXVK-style abstract-interface headers). They declare pure-virtual
COM interfaces and are implementation-agnostic — no Vulkan in them. This backend simply
*implements* those abstract interfaces. Concretely, each `IDirect3D*8` interface gets a
concrete subclass here (`Direct3D8GLES3 : public IDirect3D8`, etc.) and we export our
own `Direct3DCreate8()`.

> Build note: `SAGE_USE_GLES3` makes the include path point at the vendored headers in
> `include/` and links this library *instead of* the DXVK binary. `SAGE_USE_DX8` (native
> Windows) and the DXVK path remain mutually exclusive with it — exactly the
> "one-or-the-other" rule already enforced in `cmake/dx8.cmake`.

## The two hard parts (the actual work)

D3D8 is a **fixed-function** API. GLES3 has **no fixed-function pipeline** — everything
is shaders. So the translation layer's real substance is:

1. **`ffp_shader_gen`** — given the current FVF + 8 texture-stage states + lighting/fog/
   alpha-test render states, synthesise a GLSL ES 3.00 vertex+fragment shader pair that
   reproduces D3D8 fixed-function output, and cache it keyed on that state. This is the
   heart of the port. (`src/ffp_shader_gen.{h,cpp}`)

2. **`gl_state_map`** — translate the D3D enum-valued render states (blend factors,
   compare funcs, cull, stencil ops, fog modes, formats, primitive types, texture
   address/filter) into their `glBlendFunc`/`glDepthFunc`/… equivalents.
   (`src/gl_state_map.{h,cpp}`)

Everything else (buffers, textures, surfaces, clear/present, viewport) is mechanical.

## GLES3 capability mapping — what maps cleanly vs. what needs care

| D3D8 feature                | GLES 3.0 status | Strategy |
|-----------------------------|-----------------|----------|
| Fixed-function T&L          | none            | generated shaders (`ffp_shader_gen`) |
| Up to 8 texture stages + ops| none            | folded into fragment shader cascade |
| Vertex/index buffers        | ✅ `GL_*_BUFFER` | direct (`glBufferData`/`glMapBufferRange`) |
| DXT1/3/5 textures           | via ext         | `GL_EXT_texture_compression_s3tc` if present, else decompress on upload |
| A8R8G8B8 / X8R8G8B8         | ✅ (swizzle)     | upload as `GL_RGBA`, swizzle BGRA→RGBA |
| Cube / volume textures      | ✅ GLES3         | direct |
| Alpha test (`D3DRS_ALPHAREF`)| removed in ES3  | `discard` in fragment shader (lives in FFP gen) |
| Point sprites               | partial         | gl_PointSize path |
| Two-sided stencil           | ✅ GLES3         | `glStencilOpSeparate` |
| `D3DRS_ZBIAS`               | ✅               | `glPolygonOffset` |
| Programmable VS/PS (rare)   | engine uses FFP mostly | translate later; assert for now |

## Status

Scaffold + the two hard cores are landing first; the COM resource interfaces are wired
incrementally behind them. See `STATUS.md` for the live method-by-method checklist.
