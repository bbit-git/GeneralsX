#!/usr/bin/env bash
# Rebuild libmain.so, repackage+install APK, launch, dump tail logs + tombstone.
set -e
export ANDROID_NDK_HOME=$HOME/Android/Sdk/ndk/26.1.10909125
export ANDROID_HOME=$HOME/Android/Sdk
export PATH=$PATH:$HOME/Android/Sdk/platform-tools
ROOT=/run/media/andrzej/99e49ca5-2450-4dbb-be4b-d85dbaf9c9bd/home/workspace.local/BlueOpsDevstack
cd "$ROOT/Engine/EA/generals"
echo ">>> build"
cmake --build build/android-gles3-x64 --target g_generals -j$(nproc) 2>&1 | tail -2
cd "$ROOT"
echo ">>> install"
make install-generals-android 2>&1 | tail -2
echo ">>> launch"
adb logcat -c
adb shell am start -n com.bigbangit.bronzeops.engine/com.bigbangit.blueops.engine.GeneralsActivity >/dev/null
sleep 12
echo "=== last 25 GeneralsX ==="
adb logcat -d -s GeneralsX:* 2>&1 | tail -25
echo "=== tombstone ==="
adb logcat -d -s DEBUG:* 2>&1 | grep -iE "signal|fault addr|Cause|rip|rdi|rsi|#00" | tail -10
