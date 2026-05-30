set(GS_OPENSSL FALSE)
set(GAMESPY_SERVER_NAME "server.cnc-online.net")

FetchContent_Declare(
    gamespy
    GIT_REPOSITORY https://github.com/TheAssemblyArmada/GamespySDK.git
    GIT_TAG        07e3d15c500415abc281efb74322ab6d9c857eb8
)

FetchContent_MakeAvailable(gamespy)

# Android (bionic) has no pthread_cancel, which GameSpy's gsthreadlinux.c calls.
# Force-include a no-op shim into its common lib rather than patching upstream.
if(ANDROID AND TARGET gscommon)
    target_compile_options(gscommon PRIVATE
        -include ${CMAKE_CURRENT_LIST_DIR}/android-pthread-compat.h)
endif()
