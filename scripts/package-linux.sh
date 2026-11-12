#!/usr/bin/env bash
# NHL 19: dist/NHL19Native-linux-x86_64.tar.gz from the current Linux build (bash build.sh first).
# The program's libraries (an SDL3 built here, fmt, miniz, xxhash, Zydis, libstdc++, libgcc_s) go
# to bin/lib; glibc, Vulkan and the window system come from the system. Every ELF is checked for
# its newest glibc symbol version (BB_GLIBC_MAX, default 2.38; BB_ALLOW_NEWER_GLIBC=1 only warns).
# A dirty or untagged checkout is refused unless BB_ALLOW_DIRTY=1.
#   BB_SDL_SRC   SDL source tree (default out/windows/deps/src/SDL, from scripts/windows/fetch-deps.sh)
#   BB_AEROLIB   aerolib.inl to ship (default: out/windows/downloads/aerolib.inl or out/aerolib.inl)
#   BB_PRODUCT   the package's name (default NHL19Native)
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
product=${BB_PRODUCT:-NHL19Native}
[[ -f out/sacredfire && -f out/gpu/libbbgpu.so && -f out/VERSION ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
built_from=$(cat out/VERSION)
checkout=$(git describe --tags --match 'v*' --always 2>/dev/null || echo unknown)
git update-index -q --refresh 2>/dev/null || true
git diff-index --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null || checkout="$checkout-dirty"
if [[ -z ${BB_ALLOW_DIRTY:-} ]]; then
    [[ $checkout != *-dirty ]] || { echo "package-linux: uncommitted changes ($checkout): commit and tag first" >&2; exit 1; }
    [[ $built_from == "$checkout" ]] || { echo "package-linux: out/ was built from $built_from, the checkout is at $checkout: run build.sh again" >&2; exit 1; }
fi
set_rpath() { if type -P patchelf >/dev/null; then patchelf --set-rpath "$@"; else uvx --quiet --from patchelf patchelf --set-rpath "$@"; fi; }
work=out/linux-release
mkdir -p "$work"
# SDL3 built here with its float math bound to old glibc symbol versions (packaging/linux/glibc_compat.c).
sdl_src=${BB_SDL_SRC:-out/windows/deps/src/SDL}
if [[ ! -f $work/sdl-build/libSDL3.so.0 ]]; then
    [[ -d $sdl_src ]] || { echo "package-linux: no SDL source in $sdl_src (set BB_SDL_SRC)" >&2; exit 1; }
    gcc -O2 -fPIC -c packaging/linux/glibc_compat.c -o "$work/glibc_compat.o"
    wrap="-Wl,--wrap=acosf -Wl,--wrap=asinf -Wl,--wrap=sqrtf -Wl,--wrap=log10f -Wl,--wrap=atan2f $PWD/$work/glibc_compat.o"
    cmake -S "$sdl_src" -B "$work/sdl-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL=OFF "-DCMAKE_SHARED_LINKER_FLAGS=$wrap" > "$work/sdl-cmake.log"
    ninja -C "$work/sdl-build" > "$work/sdl-build.log"
fi
stage=$work/stage/$product
rm -rf "$work/stage"
mkdir -p "$stage/bin/gpu" "$stage/bin/lib" "$stage/scripts" "$stage/tools" "$stage/share" "$stage/patches" "$stage/licenses"
install -m755 out/sacredfire out/bb-gpu-capabilities "$stage/bin/"
install -m755 out/gpu/libbbgpu.so "$stage/bin/gpu/"
strip --strip-debug "$stage/bin/sacredfire" "$stage/bin/bb-gpu-capabilities" "$stage/bin/gpu/libbbgpu.so"
cp -L "$work/sdl-build/libSDL3.so.0" "$stage/bin/lib/"
ldd out/sacredfire out/gpu/libbbgpu.so > "$work/ldd.txt"
for lib in libfmt.so libminiz.so libxxhash.so libZydis.so libZycore.so libstdc++.so.6 libgcc_s.so.1; do
    path=$(awk -v l="$lib" 'index($1, l) == 1 && !found {print $3; found = 1}' "$work/ldd.txt")
    if [[ -z $path ]]; then
        [[ $lib == libZycore.so ]] && continue   # linked into libZydis on some systems
        echo "package-linux: $lib not found" >&2; exit 1
    fi
    cp -L "$path" "$stage/bin/lib/$(basename "$path")"
done
# Relative library paths (no build-machine path in the package).
set_rpath '$ORIGIN/gpu:$ORIGIN/lib' "$stage/bin/sacredfire"
set_rpath '$ORIGIN/lib' "$stage/bin/bb-gpu-capabilities"
set_rpath '$ORIGIN/../lib' "$stage/bin/gpu/libbbgpu.so"
for f in "$stage"/bin/lib/*.so*; do set_rpath '$ORIGIN' "$f"; done
install -m755 run.sh packaging/linux/nhl19.sh "$stage/"
cp packaging/linux/nhl19.conf.example packaging/linux/README-Linux.md LICENSE "$stage/"
cp out/VERSION "$stage/VERSION"
cp scripts/*.py "$stage/scripts/"; cp tools/gen_import_names.py tools/module_imports.py "$stage/tools/"
find patches -maxdepth 1 -type f -name '*.xml' -exec cp {} "$stage/patches/" \;
cp share/skip_movie.mkv "$stage/share/"
aerolib=${BB_AEROLIB:-}
[[ -n $aerolib ]] || for candidate in out/windows/downloads/aerolib.inl out/aerolib.inl; do [[ -f $candidate ]] && { aerolib=$candidate; break; }; done
[[ -n $aerolib && -f $aerolib ]] || { echo 'package-linux: no aerolib.inl (set BB_AEROLIB)' >&2; exit 1; }
cp "$aerolib" "$stage/share/aerolib.inl"
cp gpu/third_party/imgui/LICENSE.txt "$stage/licenses/Dear-ImGui-LICENSE.txt"
cp third_party/LibAtrac9/LICENSE* "$stage/licenses/" 2>/dev/null || true
cp third_party/minimp3/LICENSE "$stage/licenses/minimp3-LICENSE" 2>/dev/null || true
cp "$sdl_src/LICENSE.txt" "$stage/licenses/SDL3-LICENSE.txt" 2>/dev/null || true
cp packaging/windows/THIRD-PARTY.md "$stage/licenses/THIRD-PARTY.md"
# Scripts with Windows line endings do not run on Linux: the shipped text files get Unix ones.
find "$stage" -type f \( -name '*.sh' -o -name '*.py' -o -name '*.md' -o -name '*.example' -o -name '*.inl' -o -name VERSION \) \
    -exec sed -i 's/\r$//' {} +
# Checks: the newest glibc symbol; no build-machine paths; no game data.
glibc_max=${BB_GLIBC_MAX:-2.38}
newest=$(find "$stage/bin" -type f \( -name '*.so*' -o -perm -u+x \) -exec objdump -T {} \; 2>/dev/null |
         grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)
if [[ $(printf '%s\n' "$newest" "GLIBC_$glibc_max" | sort -V | tail -1) != "GLIBC_$glibc_max" ]]; then
    if [[ ${BB_ALLOW_NEWER_GLIBC:-} == 1 ]]; then echo "package-linux: warning: needs $newest (newer than $glibc_max)" >&2
    else echo "package-linux: needs $newest (> $glibc_max; BB_ALLOW_NEWER_GLIBC=1 to package anyway)" >&2; exit 1; fi
fi
if grep -rlq "$HOME" "$stage/bin" "$stage"/*.sh 2>/dev/null; then echo "package-linux: a build-machine path is in the package:" >&2; grep -rl "$HOME" "$stage/bin" >&2; fi
mkdir -p dist
tar -C "$work/stage" -czf "dist/$product-linux-x86_64.tar.gz" "$product"
(cd dist && sha256sum "$product-linux-x86_64.tar.gz" > "$product-linux-x86_64.tar.gz.sha256")
printf '%s %s\ncommit %s\nnewest glibc symbol %s\n' "$product" "$built_from" "$(git rev-parse HEAD)" "${newest:-?}" > "dist/$product-linux-x86_64.build.txt"
echo "package-linux: dist/$product-linux-x86_64.tar.gz ($built_from; newest glibc symbol $newest)"
ls -lh "dist/$product-linux-x86_64.tar.gz"
