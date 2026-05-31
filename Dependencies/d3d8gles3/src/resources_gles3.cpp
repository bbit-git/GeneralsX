/*
** d3d8gles3 — GL-backed resource implementation (see resources_gles3.h).
*/
#include "resources_gles3.h"
#include "gl_state_map.h"
#include <cstring>
#include <cstdint>
#include <vector>

namespace d3d8gles3 {
namespace {

// D3D DXT FourCCs (little-endian 'DXTn').
constexpr uint32_t FOURCC_DXT1 = 0x31545844;
constexpr uint32_t FOURCC_DXT2 = 0x32545844;
constexpr uint32_t FOURCC_DXT3 = 0x33545844;
constexpr uint32_t FOURCC_DXT4 = 0x34545844;
constexpr uint32_t FOURCC_DXT5 = 0x35545844;

inline void Rgb565(uint16_t c, int& r, int& g, int& b) {
    r = (c >> 11) & 0x1F; r = (r << 3) | (r >> 2);
    g = (c >> 5)  & 0x3F; g = (g << 2) | (g >> 4);
    b =  c        & 0x1F; b = (b << 3) | (b >> 2);
}

// CPU-decode a whole DXT1/2/3/4/5 mip level (tightly-packed 4x4 blocks in `src`)
// to RGBA8. GLES3 has no S3TC on many devices/emulators, so this is the fallback
// for compressed textures. `srcBytes` guards against a short staging buffer.
void DecodeDXT(const uint8_t* src, size_t srcBytes, uint32_t W, uint32_t H,
               uint32_t fmt, std::vector<uint8_t>& out) {
    out.assign(static_cast<size_t>(W) * H * 4, 0);
    const bool dxt1 = (fmt == FOURCC_DXT1);
    const bool dxt3 = (fmt == FOURCC_DXT2 || fmt == FOURCC_DXT3); // explicit 4-bit alpha
    const bool dxt5 = (fmt == FOURCC_DXT4 || fmt == FOURCC_DXT5); // interpolated alpha
    const uint32_t blockBytes = dxt1 ? 8u : 16u;
    const uint32_t bw = (W + 3) / 4, bh = (H + 3) / 4;

    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const size_t boff = (static_cast<size_t>(by) * bw + bx) * blockBytes;
            if (boff + blockBytes > srcBytes) return; // truncated input — bail safely
            const uint8_t* blk   = src + boff;
            const uint8_t* aBlk  = blk;                 // alpha bytes (DXT2-5)
            const uint8_t* color = dxt1 ? blk : blk + 8;

            // DXT5 interpolated alpha palette
            uint8_t aTab[8] = {0};
            if (dxt5) {
                aTab[0] = aBlk[0]; aTab[1] = aBlk[1];
                if (aTab[0] > aTab[1]) {
                    for (int i = 1; i < 7; ++i) aTab[i + 1] = static_cast<uint8_t>(((7 - i) * aTab[0] + i * aTab[1]) / 7);
                } else {
                    for (int i = 1; i < 5; ++i) aTab[i + 1] = static_cast<uint8_t>(((5 - i) * aTab[0] + i * aTab[1]) / 5);
                    aTab[6] = 0; aTab[7] = 255;
                }
            }

            const uint16_t c0 = static_cast<uint16_t>(color[0] | (color[1] << 8));
            const uint16_t c1 = static_cast<uint16_t>(color[2] | (color[3] << 8));
            int r0, g0, b0, r1, g1, b1;
            Rgb565(c0, r0, g0, b0); Rgb565(c1, r1, g1, b1);
            int pr[4], pg[4], pb[4], pa[4] = {255, 255, 255, 255};
            pr[0] = r0; pg[0] = g0; pb[0] = b0;
            pr[1] = r1; pg[1] = g1; pb[1] = b1;
            const bool oneBitAlpha = dxt1 && (c0 <= c1);
            if (oneBitAlpha) {
                pr[2] = (r0 + r1) / 2; pg[2] = (g0 + g1) / 2; pb[2] = (b0 + b1) / 2;
                pr[3] = 0; pg[3] = 0; pb[3] = 0; pa[3] = 0;
            } else {
                pr[2] = (2 * r0 + r1) / 3; pg[2] = (2 * g0 + g1) / 3; pb[2] = (2 * b0 + b1) / 3;
                pr[3] = (r0 + 2 * r1) / 3; pg[3] = (g0 + 2 * g1) / 3; pb[3] = (b0 + 2 * b1) / 3;
            }
            const uint32_t cidx = color[4] | (color[5] << 8) | (color[6] << 16) | (static_cast<uint32_t>(color[7]) << 24);

            uint64_t a5 = 0;
            if (dxt5) for (int i = 2; i < 8; ++i) a5 |= static_cast<uint64_t>(aBlk[i]) << (8 * (i - 2));

            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const uint32_t x = bx * 4 + px, y = by * 4 + py;
                    if (x >= W || y >= H) continue;
                    const int pix = py * 4 + px;
                    const int ci = (cidx >> (2 * pix)) & 3;
                    int A = 255;
                    if (dxt1)      A = pa[ci];
                    else if (dxt3) { uint8_t bytev = aBlk[pix / 2]; int nib = (pix & 1) ? (bytev >> 4) : (bytev & 0xF); A = nib * 17; }
                    else if (dxt5) A = aTab[(a5 >> (3 * pix)) & 7];
                    const size_t o = (static_cast<size_t>(y) * W + x) * 4;
                    out[o] = static_cast<uint8_t>(pr[ci]);
                    out[o + 1] = static_cast<uint8_t>(pg[ci]);
                    out[o + 2] = static_cast<uint8_t>(pb[ci]);
                    out[o + 3] = static_cast<uint8_t>(A);
                }
            }
        }
    }
}

} // namespace
} // namespace d3d8gles3

namespace d3d8gles3 {

// ============================================================================
// GLBuffer
// ============================================================================
bool GLBuffer::Create(GLenum target, uint32_t bytes, bool dynamic) {
    target_ = target; size_ = bytes; dynamic_ = dynamic;
    glGenBuffers(1, &glBuffer_);
    glBindBuffer(target_, glBuffer_);
    glBufferData(target_, bytes, nullptr, dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    shadow_.resize(bytes);
    return glBuffer_ != 0;
}

void GLBuffer::Destroy() {
    if (glBuffer_) { glDeleteBuffers(1, &glBuffer_); glBuffer_ = 0; }
    shadow_.clear();
}

void* GLBuffer::Lock(uint32_t offset, uint32_t size, uint32_t /*flags*/) {
    if (offset > size_) return nullptr;
    if (size == 0) size = size_ - offset;     // D3D: size 0 == whole buffer
    if (offset + size > size_) size = size_ - offset;
    lockOffset_ = offset; lockSize_ = size; locked_ = true;
    return shadow_.data() + offset;
}

void GLBuffer::Unlock() {
    if (!locked_) return;
    glBindBuffer(target_, glBuffer_);
    glBufferSubData(target_, lockOffset_, lockSize_, shadow_.data() + lockOffset_);
    locked_ = false;
}

// ============================================================================
// GLTexture
// ============================================================================
static uint32_t BytesPerPixel(const GLTexFormat& f) {
    if (f.type == GL_UNSIGNED_BYTE) {
        if (f.format == GL_RGBA) return 4;
        if (f.format == GL_RGB)  return 3;
        if (f.format == GL_RG)   return 2;
        if (f.format == GL_RED)  return 1;
    }
    return 2; // packed 16-bit formats
}

bool GLTexture::Create(uint32_t w, uint32_t h, uint32_t levels,
                       uint32_t d3dFormat, bool s3tcSupported) {
    width_ = w; height_ = h; levels_ = levels ? levels : 1; d3dFormat_ = d3dFormat;

    GLTexFormat fmt = MapTextureFormat(d3dFormat);
    compressed_   = fmt.compressed;
    needsSwizzle_ = fmt.needsBGRASwizzle;
    decompressOnUpload_ = compressed_ && !s3tcSupported;
    // GLES3 has no packed type matching D3D A1R5G5B5 (its 1-bit alpha is at the
    // word's MSB, GL's UNSIGNED_SHORT_5_5_5_1 puts it at the LSB), so the bit
    // fields don't line up and a channel swizzle can't fix it. Expand to RGBA8.
    expand1555_ = (d3dFormat == 25 /*D3DFMT_A1R5G5B5*/);

    glGenTextures(1, &glTexture_);
    glBindTexture(GL_TEXTURE_2D, glTexture_);

    // Allocate immutable-ish storage for the uncompressed path; compressed and
    // CPU-decompressed paths upload per level in UnlockRect.
    if (!compressed_ && !expand1555_ && fmt.supported) {
        glTexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(levels_), fmt.internalFormat,
                       static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        if (needsSwizzle_) {
            // GLES3 per-texture swizzle: remap GL's unpacked channels to D3D RGBA.
            // The mask is format-specific (BGRA byte-swap for 8888, channel rotation
            // for the packed 16-bit ARGB formats) — see MapTextureFormat.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, static_cast<GLint>(fmt.swizzle[0]));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, static_cast<GLint>(fmt.swizzle[1]));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, static_cast<GLint>(fmt.swizzle[2]));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, static_cast<GLint>(fmt.swizzle[3]));
        }
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    levels_ > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    levelData_.resize(levels_);
    uint32_t lw = w, lh = h;
    for (uint32_t i = 0; i < levels_; ++i) {
        levelData_[i].w = lw; levelData_[i].h = lh;
        levelData_[i].pitch = lw * BytesPerPixel(fmt);
        lw = lw > 1 ? lw >> 1 : 1; lh = lh > 1 ? lh >> 1 : 1;
    }
    return glTexture_ != 0;
}

void GLTexture::Destroy() {
    if (glTexture_) { glDeleteTextures(1, &glTexture_); glTexture_ = 0; }
    levelData_.clear();
}

void* GLTexture::LockRect(uint32_t level, uint32_t& outPitch, uint32_t /*flags*/) {
    if (level >= levels_) { outPitch = 0; return nullptr; }
    Level& L = levelData_[level];
    GLTexFormat fmt = MapTextureFormat(d3dFormat_);
    // compressed lock returns the packed block size pitch
    const uint32_t bytes = compressed_
        ? ((L.w + 3) / 4) * ((L.h + 3) / 4) * (d3dFormat_ == 0x31545844 /*DXT1*/ ? 8 : 16)
        : L.pitch * L.h;
    // For uncompressed locks add a slab of slack rows. Some WW3D2 CPU fills write
    // a tile-class "border" a few rows past the logical bottom edge of the
    // surface (TerrainTextureClass::update) — on real D3D the lock buffer has
    // slack, but our staging is exact, so those writes/reads would run off the
    // end. UnlockRect only uploads L.w x L.h, so the extra rows are never sent to
    // GL; they just absorb the engine's out-of-bounds border access.
    L.staging.resize(compressed_ ? bytes : (bytes + L.pitch * L.h));
    outPitch = compressed_ ? ((L.w + 3) / 4) * (d3dFormat_ == 0x31545844 ? 8 : 16) : L.pitch;
    (void)fmt;
    return L.staging.data();
}

void GLTexture::UnlockRect(uint32_t level) {
    if (level >= levels_) return;
    Level& L = levelData_[level];
    if (L.staging.empty()) return;
    glBindTexture(GL_TEXTURE_2D, glTexture_);
    GLTexFormat fmt = MapTextureFormat(d3dFormat_);

    if (compressed_ && !decompressOnUpload_) {
        glCompressedTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), fmt.internalFormat,
                               static_cast<GLsizei>(L.w), static_cast<GLsizei>(L.h), 0,
                               static_cast<GLsizei>(L.staging.size()), L.staging.data());
    } else if (compressed_ && decompressOnUpload_) {
        // No S3TC on this GLES device: CPU-decode the DXT block data to RGBA8.
        std::vector<uint8_t> rgba;
        DecodeDXT(L.staging.data(), L.staging.size(), L.w, L.h, d3dFormat_, rgba);
        glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), GL_RGBA8,
                     static_cast<GLsizei>(L.w), static_cast<GLsizei>(L.h), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    } else if (expand1555_) {
        // D3D A1R5G5B5 [A:1][R:5][G:5][B:5] (MSB->LSB) -> RGBA8 (5-bit expanded to
        // 8 by bit-replication; alpha is the single top bit -> 0 or 255).
        const size_t n = static_cast<size_t>(L.w) * L.h;
        std::vector<uint8_t> rgba(n * 4);
        const uint16_t* src = reinterpret_cast<const uint16_t*>(L.staging.data());
        for (size_t i = 0; i < n; ++i) {
            const uint16_t p = src[i];
            uint8_t r = (p >> 10) & 0x1F, g = (p >> 5) & 0x1F, b = p & 0x1F;
            rgba[i*4+0] = static_cast<uint8_t>((r << 3) | (r >> 2));
            rgba[i*4+1] = static_cast<uint8_t>((g << 3) | (g >> 2));
            rgba[i*4+2] = static_cast<uint8_t>((b << 3) | (b >> 2));
            rgba[i*4+3] = (p & 0x8000) ? 255 : 0;
        }
        glTexImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), GL_RGBA8,
                     static_cast<GLsizei>(L.w), static_cast<GLsizei>(L.h), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(level), 0, 0,
                        static_cast<GLsizei>(L.w), static_cast<GLsizei>(L.h),
                        fmt.format, fmt.type, L.staging.data());
    }
}

void GLTexture::ApplySamplerState(uint32_t* tss) {
    // D3DTSS_ADDRESSU=13, ADDRESSV=14, MAGFILTER=16, MINFILTER=17, MIPFILTER=18
    glBindTexture(GL_TEXTURE_2D, glTexture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, MapTextureAddress(tss[13] ? tss[13] : 1));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, MapTextureAddress(tss[14] ? tss[14] : 1));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, MapMagFilter(tss[16] ? tss[16] : 2));
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                    MapMinFilter(tss[17] ? tss[17] : 2, tss[18]));
}

} // namespace d3d8gles3
