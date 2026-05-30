/*
** d3d8gles3 — GL-backed resource implementation (see resources_gles3.h).
*/
#include "resources_gles3.h"
#include "gl_state_map.h"
#include <cstring>

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

    glGenTextures(1, &glTexture_);
    glBindTexture(GL_TEXTURE_2D, glTexture_);

    // Allocate immutable-ish storage for the uncompressed path; compressed and
    // CPU-decompressed paths upload per level in UnlockRect.
    if (!compressed_ && fmt.supported) {
        glTexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(levels_), fmt.internalFormat,
                       static_cast<GLsizei>(w), static_cast<GLsizei>(h));
        if (needsSwizzle_) {
            // GLES3 supports per-texture swizzle: present D3D BGRA as RGBA.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
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
    L.staging.resize(bytes);
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
        // TODO(android): CPU DXT decode -> RGBA8 then glTexImage2D. Until then,
        // upload a magenta placeholder so missing-codec surfaces are obvious.
        std::vector<uint8_t> rgba(L.w * L.h * 4, 0);
        for (size_t p = 0; p < rgba.size(); p += 4) { rgba[p]=255; rgba[p+2]=255; rgba[p+3]=255; }
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
