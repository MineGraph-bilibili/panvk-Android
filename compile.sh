export ANDROID_NDK_HOME="$HOME/Android/Sdk/ndk/27.0.12077973"
export MESON_WORKING_DIR="$PWD"

export PATH="$MESON_WORKING_DIR/build-host/src/compiler/clc:$MESON_WORKING_DIR/build-host/src/compiler/spirv:$MESON_WORKING_DIR/build-host/src/panfrost/clc:$PATH"

meson compile -C build
