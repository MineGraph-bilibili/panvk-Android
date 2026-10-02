export ANDROID_NDK_HOME="$HOME/Android/Sdk/ndk/27.0.12077973"
export MESON_WORKING_DIR="$PWD"

meson setup build-host \
    -Dplatforms=[] \
    -Dgallium-drivers=[] \
    -Dvulkan-drivers=[] \
    -Dtools=panfrost \
    -Dprecomp-compiler=enabled \
    -Dinstall-precomp-compiler=true \
    -Dllvm=enabled \
    -Dmesa-clc=enabled -Dinstall-mesa-clc=true

ninja -C build-host src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 src/panfrost/clc/panfrost_compile

ln -sf "$MESON_WORKING_DIR/build-host/src/compiler/clc/mesa_clc" "$MESON_WORKING_DIR/build-host/src/compiler/clc/mesa-clc"
ln -sf "$MESON_WORKING_DIR/build-host/src/compiler/spirv/vtn_bindgen2" "$MESON_WORKING_DIR/build-host/src/compiler/spirv/vtn-bindgen2"
ln -sf "$MESON_WORKING_DIR/build-host/src/compiler/spirv/panfrost_compile" "$MESON_WORKING_DIR/build-host/src/compiler/spirv/panfrost-compile"

export PATH="$MESON_WORKING_DIR/build-host/src/compiler/clc:$MESON_WORKING_DIR/build-host/src/compiler/spirv:$MESON_WORKING_DIR/build-host/src/panfrost/clc:$PATH"

$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-ar cr shims/librt.a

export PKG_CONFIG_LIBDIR=$MESON_WORKING_DIR/shims

cat <<EOF > shims/xrandr.pc
Name: xrandr
Description: xrandr
Version: 1.15
Libs: -lXrandr
Cflags:
EOF

cat <<EOF > shims/libdrm.pc
Name: libdrm
Description: libdrm
Version: 2.5
Libs: -ldrm
Cflags:
EOF

cat <<EOF > shims/libx11-xcb.pc
Name: libx11-xcb
Description: libx11-xcb
Version: 2.5
Libs: -lX11-xcb
Cflags:
EOF

#android.hardware.graphics.mapper@4.0
cat <<EOF > shims/android.hardware.graphics.mapper.pc
Name: android.hardware.graphics.mapper
Description: android.hardware.graphics.mapper
Version: 4.0
Libs: -landroid.hardware.graphics.mapper
Cflags:
EOF

for lib in xcb xcb-randr xcb-dri3 xcb-present xcb-shm xcb-sync xshmfence xcb-xfixes gralloctypes hidlbase utils cutils hardware log sync nativewindow ui;
do
cat <<EOF > shims/$lib.pc
Name: $lib
Description: $lib
Version: 1.15
Libs: -l$lib
Cflags:
EOF
done

envsubst < android.toml > /tmp/android_wrapper.toml

meson setup build --reconfigure \
    --cross-file /tmp/android_wrapper.toml \
    -Dbuildtype=debug \
    -Dplatforms=android,x11 \
    -Dandroid-stub=true \
    -Dandroid-libbacktrace=disabled \
    -Dplatform-sdk-version=30 \
    -Dglx=disabled \
    -Dgbm=disabled \
    -Degl=disabled \
    -Dopengl=false \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dglvnd=disabled \
    -Dvalgrind=disabled \
    -Dgallium-drivers= \
    -Dshared-glapi=disabled \
    -Dzstd=disabled \
    -Dmesa-clc=system \
    -Dprecomp-compiler=system \
    -Dvulkan-drivers=panfrost \
    -Dllvm=disabled -Dstrip=false -Dpanfrost-kmds=kbase,panthor
