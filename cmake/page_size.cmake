# 16 KB ELF segment alignment for Android (16 KB-page devices / Play Store).
#
# Android 15+ devices may use a 16 KB memory page size (e.g. the emu64xa16k
# emulator, Pixel-class API 37 images), and Play Store delivery requires every
# packaged shared library to have its PT_LOAD segments aligned to 16 KB
# (max-page-size = 16384). NDK r26 links with a 4 KB max-page-size by default,
# which loads on lenient images today but is NOT compliant and can fail dlopen
# on stricter 16 KB devices.
#
# NDK r28+ already makes 16 KB the linker default, so this flag is a harmless
# no-op there; we keep it set explicitly so the in-tree libraries (libmain.so,
# libSDL3.so, libSDL3_image.so, libopenal.so, libgamespy.so) are 16 KB-aligned
# regardless of the NDK version in use. The NDK's own prebuilt libc++_shared.so
# is only aligned once the NDK itself is r28+ (it ships pre-built) — see
# ndkVersion in the GeneralsLauncher gradle and ANDROID_NDK_HOME in the Makefile.
#
# Must be included before any add_subdirectory()/FetchContent_MakeAvailable()
# that defines shared targets, so those inherit the flag.

if(ANDROID)
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-z,max-page-size=16384")
    string(APPEND CMAKE_EXE_LINKER_FLAGS    " -Wl,-z,max-page-size=16384")
    message(STATUS "Android: ELF segments aligned to 16 KB (-Wl,-z,max-page-size=16384)")
endif()
