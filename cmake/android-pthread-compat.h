/*
** android-pthread-compat.h — bionic pthread_cancel shim.
**
** Android's libc (bionic) does not provide pthread_cancel. The GameSpy SDK
** (fetched verbatim from upstream) calls it in gsiCancelThread, which fails to
** compile for arm64-android with -Wimplicit-function-declaration. Forced thread
** cancellation is unsupported on Android, so this provides a no-op shim that
** reports success. Force-included into the affected GameSpy TUs via -include
** (see cmake/gamespy.cmake) so no upstream source is patched.
*/
#pragma once

#if defined(__ANDROID__)
#include <pthread.h>

static inline int pthread_cancel(pthread_t thread)
{
    (void)thread;
    return 0; /* PTHREAD_NO_ERROR == 0; caller treats this as success */
}
#endif
