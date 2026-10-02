#!/bin/sh
# Copyright (C) 2026 Olivier Moulin
# Part of SIEOS, released under the GNU General Public License version 3
# (GPL-3.0); see the LICENSE file.
#
# host-mesa-clc.sh TREE OUT - Mesa's mesa_clc and vtn_bindgen2 for the build host.
#
# NVK's internal shaders are OpenCL C (src/nouveau/vulkan/cl), compiled to
# SPIR-V when Mesa is built, by mesa_clc (clang, LLVM's SPIR-V translator,
# SPIRV-Tools), then to C by vtn_bindgen2: tools that run on the build host,
# which a cross-build of Mesa takes as "system" ones (-Dmesa-clc=system).
# Clang, LLVM and the translator 20.1 come from conda-forge (a conda or
# mamba on the PATH, or miniforge's; an environment in OUT/env), SPIRV-Tools
# from its Vulkan SDK release (checked against ports/SHA256SUMS: conda-forge's
# is too old for Mesa); then Mesa, built natively for those two programs only:
# OUT/bin/mesa_clc, OUT/bin/vtn_bindgen2.
set -e
TREE=$(cd "$1" && pwd)
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
MESA_TXZ=$TREE/build/ports/dl/mesa-26.2.4.tar.xz
SDK=vulkan-sdk-1.4.321.0

# the conda environment: clang, LLVM and the SPIR-V translator
CONDA=
for c in mamba conda "$HOME/miniforge3/bin/mamba" "$HOME/opt/miniforge3/bin/mamba" "$HOME/miniconda3/bin/conda"; do
    command -v "$c" > /dev/null 2>&1 && { CONDA=$c; break; }
done
[ -n "$CONDA" ] || { echo "host-mesa-clc: conda or mamba is needed (miniforge: https://conda-forge.org/download/)" >&2; exit 1; }
[ -x "$OUT/env/bin/llvm-config" ] ||
    "$CONDA" create -q -y -p "$OUT/env" -c conda-forge 'clangdev=20.1' 'llvmdev=20.1' 'libllvmspirv=20.1' zlib zstd > /dev/null

# SPIRV-Tools and its headers
mkdir -p "$OUT/dl"
for r in SPIRV-Tools SPIRV-Headers; do
    ( cd "$OUT/dl" && "$TREE/tools/fetch.sh" https://github.com/KhronosGroup/$r/archive/refs/tags/$SDK.tar.gz $r-$SDK.tar.gz &&
      grep " $r-$SDK.tar.gz\$" "$TREE/ports/SHA256SUMS" | sha256sum -c --quiet )
done
rm -rf "$OUT/spv" && mkdir -p "$OUT/spv"
tar xzf "$OUT/dl/SPIRV-Tools-$SDK.tar.gz" -C "$OUT/spv"
tar xzf "$OUT/dl/SPIRV-Headers-$SDK.tar.gz" -C "$OUT/spv"
cmake -S "$OUT/spv/SPIRV-Tools-$SDK" -B "$OUT/spv/b" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$OUT/spirv" -DSPIRV-Headers_SOURCE_DIR="$OUT/spv/SPIRV-Headers-$SDK" \
    -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON -DSPIRV_WERROR=OFF -DBUILD_SHARED_LIBS=OFF > "$OUT/spv/cmake.log"
ninja -C "$OUT/spv/b" install > "$OUT/spv/ninja.log"
SPC=$(ls -d "$OUT"/spirv/lib*/pkgconfig | head -1)

# Mesa, for the two programs
PY=; for p in python3 /usr/bin/python3; do $p -c 'import mako, yaml' 2>/dev/null && PY=$(command -v $p) && break; done
[ -n "$PY" ] || { echo "host-mesa-clc: a Python 3 with mako and yaml is needed" >&2; exit 1; }
rm -rf "$OUT/mesa" && mkdir -p "$OUT/mesa" && tar xJf "$MESA_TXZ" -C "$OUT/mesa" --strip-components=1
printf '[binaries]\npython = %s\nllvm-config = %s\n' "'$PY'" "'$OUT/env/bin/llvm-config'" > "$OUT/mesa/native.ini"
( cd "$OUT/mesa" &&
  PATH=$OUT/env/bin:$PATH PKG_CONFIG_PATH=$SPC:$OUT/env/lib/pkgconfig LDFLAGS="-Wl,-rpath,$OUT/env/lib" \
  meson setup b . --native-file native.ini --prefix="$OUT/inst" -Dbuildtype=release \
      -Dmesa-clc=enabled -Dinstall-mesa-clc=true -Dprecomp-compiler=enabled -Dinstall-precomp-compiler=true \
      -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
      -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Dllvm=enabled -Dshared-llvm=enabled -Dbuild-tests=false \
      -Dvalgrind=disabled -Dlibunwind=disabled -Dzstd=disabled -Dexpat=disabled -Dxmlconfig=disabled \
      -Dvideo-codecs= > b.log &&
  ninja -C b src/compiler/clc/mesa_clc src/compiler/spirv/vtn_bindgen2 > ninja.log )
mkdir -p "$OUT/bin"
cp "$OUT/mesa/b/src/compiler/clc/mesa_clc" "$OUT/mesa/b/src/compiler/spirv/vtn_bindgen2" "$OUT/bin/"
echo "host-mesa-clc: $OUT/bin/mesa_clc, $OUT/bin/vtn_bindgen2"
