#!/bin/sh
# Clones the pinned dependencies into deps/ (skips ones already there), applies
# patches/<dep>/*.patch to fresh clones, and downloads the SDL2 Windows dev zip. Then build N64Recomp:
#   tools\vsenv.cmd cmake -S deps/N64Recomp -B deps/N64Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   tools\vsenv.cmd cmake --build deps/N64Recomp/build
set -e
cd "$(dirname "$0")/.."
mkdir -p deps

clone() { # dir url commit
    if [ ! -d "deps/$1" ]; then
        git clone "$2" "deps/$1"
        git -C "deps/$1" checkout "$3"
        git -C "deps/$1" submodule update --init --recursive
        for p in patches/"$1"/*.patch; do
            [ -f "$p" ] && git -C "deps/$1" apply "$PWD/$p"
        done
    fi
}

clone N64Recomp https://github.com/N64Recomp/N64Recomp.git ffb39cdad1da5de07eaaa48bd1db4a89a7986771
clone N64ModernRuntime https://github.com/N64Recomp/N64ModernRuntime.git cdf5abbd5026fef5c364c676e4667c45e42b6863
clone rt64 https://github.com/rt64/rt64.git 23cab603c4f9f4a8b369b38e036f1aa484603878
# Overlay menu (src/ui/): RmlUi, and FreeType prebuilt for Windows.
clone RmlUi https://github.com/mikke89/RmlUi.git 7a06f27db04fe5d13a5dacc19b2b4544673a4eca
clone freetype-windows-binaries https://github.com/ubawurinna/freetype-windows-binaries.git d6fb49d11a9d0011bf4ecfe7e570beaaa189838a
# Only its signature file (src/builtin_signatures.sig) is used.
clone n64sym https://github.com/shygoo/n64sym.git ccf4600f3389f1a84bde23339225cf372fdf7712

# CMake's own downloader can fail TLS on this machine, so fetch with curl.
[ -f deps/SDL2-devel-2.30.3-VC.zip ] || curl -L -o deps/SDL2-devel-2.30.3-VC.zip \
    https://github.com/libsdl-org/SDL/releases/download/release-2.30.3/SDL2-devel-2.30.3-VC.zip
