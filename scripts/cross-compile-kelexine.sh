#!/bin/sh
# Cross-compile engine + udf2disk for Android 6+ (API 23).
set -e
ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/engine"
OUT="$ROOT/module/libs"
if [ -n "$ANDROID_NDK_HOME" ]; then
  NDK="$ANDROID_NDK_HOME"
elif [ -n "$ANDROID_NDK_ROOT" ]; then
  NDK="$ANDROID_NDK_ROOT"
else
  NDK="$HOME/.cache/android-ndk-r26d"
fi
case "$(uname -s)" in
  Linux*) HOST_TAG=linux-x86_64 ;;
  Darwin*) HOST_TAG=darwin-x86_64 ;;
  MINGW*|MSYS*|CYGWIN*) HOST_TAG=windows-x86_64 ;;
  *) HOST_TAG=linux-x86_64 ;;
esac
PRE="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
if [ ! -f "$PRE/clang++" ] && [ ! -f "$PRE/clang++.exe" ]; then
  for alt in linux-x86_64 darwin-x86_64 windows-x86_64; do
    if [ -f "$NDK/toolchains/llvm/prebuilt/$alt/bin/clang++" ]; then
      HOST_TAG="$alt"
      PRE="$NDK/toolchains/llvm/prebuilt/$alt/bin"
      break
    fi
  done
fi
[ -f "$PRE/clang++" ] || [ -f "$PRE/clang++.exe" ] || { echo "NDK clang++ not found under $NDK"; exit 1; }
STRIP="$PRE/llvm-strip"
if [ ! -f "$STRIP" ] && [ -f "$STRIP.exe" ]; then STRIP="$STRIP.exe"; fi
[ -f "$SRC/src/main.cpp" ] || { echo "engine sources missing at $SRC"; exit 1; }

compile() {
  abi=$1; triple=$2; api=$3
  dest="$OUT/$abi"
  mkdir -p "$dest"
  cxx="$PRE/${triple}${api}-clang++"
  cc="$PRE/${triple}${api}-clang"
  echo "=== $abi api$api isodrive ==="
  "$cxx" -std=c++17 -O2 -fPIE -pie -static-libstdc++ \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$SRC/src/include" \
    "$SRC/src/main.cpp" "$SRC/src/util.cpp" "$SRC/src/logger.cpp" \
    "$SRC/src/configfsisomanager.cpp" "$SRC/src/androidusbisomanager.cpp" \
    "$SRC/src/uiserver.cpp" \
    -o "$dest/isodrive"
  "$STRIP" "$dest/isodrive"

  echo "=== $abi api$api udf2disk ==="
  obj="$dest/obj"
  mkdir -p "$obj"
  "$cc" -x c -std=c11 -O2 -fPIC -D_FILE_OFFSET_BITS=64 -I"$SRC/third_party/fatfs" \
    -c "$SRC/third_party/fatfs/ff.c" -o "$obj/ff.o"
  "$cc" -x c -std=c11 -O2 -fPIC -D_FILE_OFFSET_BITS=64 -I"$SRC/third_party/fatfs" \
    -c "$SRC/third_party/fatfs/ffunicode.c" -o "$obj/ffunicode.o"
  "$cc" -x c -std=c11 -O2 -fPIC -D_FILE_OFFSET_BITS=64 -I"$SRC/third_party/fatfs" \
    -c "$SRC/third_party/fatfs/diskio_file.c" -o "$obj/diskio.o"
  "$cxx" -std=c++17 -O2 -fPIE -pie -static-libstdc++ -D_FILE_OFFSET_BITS=64 \
    -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I"$SRC/udf" -I"$SRC/third_party/fatfs" \
    "$SRC/udf/udf2disk.cpp" "$SRC/udf/udf.cpp" \
    "$obj/ff.o" "$obj/ffunicode.o" "$obj/diskio.o" \
    -o "$dest/udf2disk"
  rm -rf "$obj"
  "$STRIP" "$dest/udf2disk"
  file "$dest/isodrive" "$dest/udf2disk"
}

rm -rf "$OUT"
compile arm64-v8a aarch64-linux-android 23
compile armeabi-v7a armv7a-linux-androideabi 23
compile x86 i686-linux-android 23
compile x86_64 x86_64-linux-android 23
echo "OK -> $OUT"
