/*
** d3d8gles3 — GL-backed resources (vertex/index buffers, textures).
**
** Plain GL wrappers with no COM knowledge; the IDirect3DVertexBuffer8 /
** IDirect3DIndexBuffer8 / IDirect3DTexture8 shims wrap instances of these.
*/
#pragma once

#include <GLES3/gl3.h>
#include <cstdint>
#include <vector>

namespace d3d8gles3 {

// Vertex or index buffer. D3D Lock/Unlock is emulated with a CPU shadow copy
// that is uploaded on Unlock (GLES3 lacks the persistent-mapping guarantees the
// engine's lock semantics assume, and shadowing is simplest+correct).
class GLBuffer {
public:
    bool Create(GLenum target, uint32_t bytes, bool dynamic);
    void Destroy();

    // D3D8 IDirect3D*Buffer8::Lock — returns a writable CPU pointer.
    void* Lock(uint32_t offset, uint32_t size, uint32_t d3dLockFlags);
    void  Unlock();   // uploads the dirty range to the GL buffer

    void BindAs(GLenum target) const { glBindBuffer(target, glBuffer_); }
    bool Is32Bit() const { return index32_; }
    void SetIndex32(bool v) { index32_ = v; }
    uint32_t Size() const { return size_; }

private:
    GLuint   glBuffer_ = 0;
    GLenum   target_ = GL_ARRAY_BUFFER;
    uint32_t size_ = 0;
    bool     dynamic_ = false;
    bool     index32_ = false;
    std::vector<uint8_t> shadow_;
    uint32_t lockOffset_ = 0, lockSize_ = 0;
    bool     locked_ = false;
};

// 2D texture (mip chain). Cube/volume handled by sibling classes later.
class GLTexture {
public:
    bool Create(uint32_t width, uint32_t height, uint32_t levels,
                uint32_t d3dFormat, bool s3tcSupported);
    void Destroy();

    // D3D8 IDirect3DTexture8::LockRect — returns a pointer + pitch into a CPU
    // staging buffer for the given mip level; UnlockRect uploads it.
    void* LockRect(uint32_t level, uint32_t& outPitch, uint32_t d3dLockFlags);
    void  UnlockRect(uint32_t level);

    void Bind() const { glBindTexture(GL_TEXTURE_2D, glTexture_); }
    void ApplySamplerState(uint32_t* stageStates); // wrap/filter from D3DTSS

    uint32_t Width()  const { return width_; }
    uint32_t Height() const { return height_; }

private:
    struct Level { uint32_t w=0, h=0, pitch=0; std::vector<uint8_t> staging; };
    GLuint   glTexture_ = 0;
    uint32_t width_ = 0, height_ = 0, levels_ = 1;
    uint32_t d3dFormat_ = 0;
    bool     compressed_ = false;
    bool     needsSwizzle_ = false;
    bool     decompressOnUpload_ = false;  // DXT but no S3TC ext
    bool     expand1555_ = false;          // D3D A1R5G5B5 -> RGBA8 on upload
    std::vector<Level> levelData_;
};

} // namespace d3d8gles3
