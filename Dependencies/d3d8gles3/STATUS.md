# d3d8gles3 — implementation status

Legend: ✅ done · 🟡 partial · ⬜ not started

## Core (header-free, host-testable)
- ✅ `gl_state_map` — blend / cmp / cull / stencil / blendop / primitive / texture
  address+filter / D3DFORMAT → GLES3. Unit-tested.
- ✅ `ffp_shader_gen` — FFPKey (FVF + 8 stages + lights + fog + alpha test),
  GLSL ES 3.00 VS+FS generation, program cache + uniform resolution.
  Unit-tested (string output). 🟡 texture-op coverage: see "Texture ops" below.
- 🟡 `device_gles3` — state caching, blend/depth/stencil/cull apply, FFP uniform
  upload (transforms, material, lights, fog, tfactor, alpha-ref), Clear, draw
  dispatch, ✅ FVF→`glVertexAttribPointer` binding (position incl. XYZRHW/blend
  weights, normal, D3DCOLOR diffuse/specular with BGRA swizzle, N texcoord sets
  with per-set size). **Gaps:** texture-matrix uniforms, point-sprite path,
  vertex fog factor in VS.
- 🟡 `resources_gles3` — VB/IB create+lock/unlock (CPU-shadow upload), texture
  create+lock/unlock, sampler state, BGRA swizzle. **Gaps:** CPU DXT decode when
  `GL_EXT_texture_compression_s3tc` absent (currently magenta placeholder);
  cube/volume textures; mip auto-gen.

## COM ABI shim (`d3d8_entry.cpp`, needs `<d3d8.h>`)
- ✅ `Direct3DCreate8` + `IDirect3D8` (single GLES3 adapter, caps, EnumAdapterModes,
  CheckDevice* permissive).
- ✅ `IDirect3DDevice8` — full vtable (all ~90 methods, exact signatures from
  mingw-directx-headers). Hot path forwards to the GLES3 core; long tail
  (state blocks, palettes, software/programmable shaders, ProcessVertices,
  gamma, DrawPrimitiveUP) is safe no-op / `D3DERR_INVALIDCALL`.
- ✅ Resource wrappers: `VertexBuffer8`, `IndexBuffer8`, `Texture8` (refcounted
  `Unknown<>` base, forward Lock/Unlock/LockRect/GetDesc to GL resources).
- ⬜ `Surface8` wrapper + render-to-texture / `SetRenderTarget` / `CopyRects`
  via FBOs (currently `D3DERR_INVALIDCALL`).
- ⬜ Cube/volume textures (`CreateCubeTexture`/`CreateVolumeTexture` stubbed).
- ⬜ Additional swap chains (windowed multi-view) — likely unsupported on Android.
- ⚠️ Not yet compiled against real `<d3d8.h>` (needs the NDK/host-GLES build with
  DXVK_INCLUDE_DIR populated) — signatures authored from the verbatim header but
  unverified by a compiler. First on-target build will shake out any mismatches.

## Texture ops (D3DTEXTUREOP) coverage in `ffp_shader_gen`
- ✅ DISABLE, SELECTARG1/2, MODULATE/2X/4X, ADD, ADDSIGNED/2X, SUBTRACT,
  ADDSMOOTH, BLEND{DIFFUSE,TEXTURE,FACTOR,CURRENT}ALPHA, BLENDTEXTUREALPHAPM,
  DOTPRODUCT3.
- ⬜ MODULATEALPHA_ADDCOLOR, MODULATECOLOR_ADDALPHA, MODULATEINVALPHA_ADDCOLOR,
  MODULATEINVCOLOR_ADDALPHA, BUMPENVMAP(LUMINANCE), MULTIPLYADD, LERP.
  (Unhandled ops fall back to MODULATE and should be logged once.)

## Integration with the engine / build
- ✅ `SAGE_USE_GLES3` option, `cmake/gles3.cmake`, module `CMakeLists.txt`,
  `core_config` link, mutual-exclusion vs DX8/MoltenVK.
- 🟡 D3D8 ABI headers reused from DXVK via FetchContent (headers-only).
  TODO: vendor the headers for offline NDK builds.
- ⬜ NDK toolchain file + SDL3 Android `SDLActivity`/GL context creation.
- ⬜ Wire `dx8wrapper`'s device-create path to prefer our `Direct3DCreate8`
  under `SAGE_USE_GLES3` (it already calls `Direct3DCreate8`, so this is mostly
  link-order + caps reporting).

## Known correctness risks to validate on-device
- Front-face winding vs the engine's flipped projection (`glFrontFace(GL_CW)`
  assumption in `Init`).
- Viewport Y-flip interaction with render-to-texture (FBOs are not Y-flipped).
- `baseVertexIndex` handling (no `glDrawElementsBaseVertex` in GLES3 core — folded
  at index upload time; verify against the engine's dynamic IB usage).
- 16-bit vs 32-bit index buffers (`GLBuffer::SetIndex32`).
