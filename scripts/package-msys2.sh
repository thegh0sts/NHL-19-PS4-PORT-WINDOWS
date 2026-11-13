#!/usr/bin/env bash
# SacredFire Windows release archive made ON Windows (MSYS2 CLANG64), after scripts/build-msys2.sh.
# The same archive as scripts/package-windows.sh (which runs on Linux after build-windows.sh), with
# MSYS2's tools and libraries in place of the llvm-mingw toolchain, sysroot and source checkouts:
#   llvm-readobj / llvm-strip      from MSYS2 (mingw-w64-clang-x86_64-llvm, part of the toolchain)
#   the DLL closure                resolved against out/windows/bin and /clang64/bin
#   python-embed, aerolib.inl      downloaded from the URLs in packaging/windows/deps.lock and checked
#                                  against their sha256 (once; kept in out/windows/downloads)
#   third-party licences           from /clang64/share/licenses and the submodules
# Result: dist/UFC3Native-windows-x86_64.zip (+ .sha256, .build.txt). Never: game files, caches, saves.
# A dirty checkout is refused unless BB_ALLOW_DIRTY=1 (as package-windows.sh).
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
[[ ${MSYSTEM:-} == CLANG64 ]] || { echo "package-msys2: run this in the MSYS2 CLANG64 shell" >&2; exit 1; }
root=${BB_WINDOWS_ROOT:-$PWD/out/windows}
product=${BB_PRODUCT:-UFC3Native}          # archive folder and zip name
launcher_exe=${BB_LAUNCHER_EXE:-UFC3.exe}  # what SacredFire.exe is called in the archive
for tool in llvm-readobj llvm-strip python3 sha256sum curl; do
    command -v $tool >/dev/null || { echo "package-msys2: '$tool' is missing" >&2; exit 1; }
done
[[ -f $root/bin/sacredfire-game.exe && -f $root/bin/SacredFire.exe && -f $root/bin/VERSION ]] ||
    { echo 'Build first: bash scripts/build-msys2.sh' >&2; exit 1; }
built_from=$(cat "$root/bin/VERSION")
checkout=$(git describe --tags --always 2>/dev/null || echo unknown)
git update-index -q --refresh 2>/dev/null || true
git diff-index --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null || checkout="$checkout-dirty"
if [[ -z ${BB_ALLOW_DIRTY:-} ]]; then
    [[ $checkout != *-dirty ]] || { echo "The checkout has uncommitted changes ($checkout): commit first (BB_ALLOW_DIRTY=1 for a test archive)." >&2; exit 1; }
    [[ $built_from == "$checkout" ]] || { echo "out/windows/bin was built from $built_from, the checkout is at $checkout: build again." >&2; exit 1; }
fi

# Downloads named in deps.lock, verified by sha256.
fetch() {  # fetch <lock name> -> prints the local path
    local name=$1 url sha file
    url=$(awk -v n="$name" '$1 == n && $2 == "archive" {print $3}' packaging/windows/deps.lock)
    sha=$(awk -v n="$name" '$1 == n && $2 == "archive" {print $4}' packaging/windows/deps.lock); sha=${sha#sha256:}
    [[ -n $url ]] || { echo "package-msys2: $name is not in packaging/windows/deps.lock" >&2; exit 1; }
    file=$root/downloads/${url##*/}
    mkdir -p "$root/downloads"
    if [[ ! -f $file ]]; then
        echo "package-msys2: downloading $url" >&2
        curl -fsSL -o "$file.part" "$url" && mv "$file.part" "$file"
    fi
    if [[ $sha != - && $(sha256sum "$file" | cut -d' ' -f1) != "$sha" ]]; then
        echo "package-msys2: $file does not match its sha256 in deps.lock" >&2; rm -f "$file"; exit 1
    fi
    printf '%s\n' "$file"
}
python_zip=$(fetch python-embed)
aerolib=$(fetch aerolib.inl)

stage=$root/stage/$product
rm -rf "$root/stage"
mkdir -p "$stage/bin" "$stage/python" "$stage/share" "$stage/licenses" "$stage/tools"
llvm-strip --strip-debug -o "$stage/$launcher_exe" "$root/bin/SacredFire.exe"
for exe in sacredfire-game.exe bb-gpu-capabilities.exe; do llvm-strip --strip-debug -o "$stage/bin/$exe" "$root/bin/$exe"; done
# The new launcher window (launcher/imgui): bin\NHL19Launcher.exe beside SDL3.dll; Play there starts
# NHL19.exe --play. NHL19.exe stays the main launcher while the new one is tested.
new_launcher=bin/${launcher_exe%.exe}Launcher.exe
[[ -f $root/bin/nhl-launcher.exe ]] && llvm-strip --strip-debug -o "$stage/$new_launcher" "$root/bin/nhl-launcher.exe"

# DLL closure from each PE's import table; Windows' own DLLs are not shipped.
declare -A seen
resolve() {
    local pe=$1
    while read -r dll; do
        local lower=${dll,,}
        case $lower in
        kernel32.dll|user32.dll|gdi32.dll|comctl32.dll|advapi32.dll|shell32.dll|ole32.dll|oleaut32.dll|shlwapi.dll|ws2_32.dll|bcrypt.dll|ntdll.dll|\
        winmm.dll|imm32.dll|version.dll|setupapi.dll|cfgmgr32.dll|dwmapi.dll|uxtheme.dll|crypt32.dll|msvcrt.dll|vulkan-1.dll|\
        api-ms-win-*|ext-ms-*|dbghelp.dll|synchronization.dll|onecore.dll|comdlg32.dll|hid.dll|xinput*.dll|dinput8.dll|\
        d3d11.dll|d3d12.dll|dxgi.dll|opengl32.dll|wininet.dll|urlmon.dll|iphlpapi.dll|dxcore.dll|windows.*.dll|avrt.dll|mmdevapi.dll|\
        userenv.dll|secur32.dll|ncrypt.dll|mfplat.dll|mfreadwrite.dll|mf.dll|propsys.dll|winhttp.dll)
            ;;
        *)
            [[ -n ${seen[$lower]:-} ]] && continue
            seen[$lower]=1
            local found=
            for dir in "$root/bin" /clang64/bin; do
                if [[ -f $dir/$dll ]]; then found=$dir/$dll; break; fi
            done
            [[ -n $found ]] || { echo "package-msys2: $pe imports $dll, which is neither a Windows DLL nor in out/windows/bin or /clang64/bin" >&2; exit 1; }
            llvm-readobj --file-headers "$found" | grep -q IMAGE_FILE_MACHINE_AMD64 || { echo "package-msys2: $found is not an AMD64 DLL" >&2; exit 1; }
            cp "$found" "$stage/bin/$dll"
            echo "  $dll <- $found"
            resolve "$found"
            ;;
        esac
    done < <(llvm-readobj --coff-imports "$pe" | sed -n 's/^ *Name: \(.*\)$/\1/p' | tr -d '\r')
}
echo 'package-msys2: DLL closure'
resolve "$stage/bin/sacredfire-game.exe"; resolve "$stage/bin/bb-gpu-capabilities.exe"; resolve "$stage/$launcher_exe"
[[ -f $stage/$new_launcher ]] && resolve "$stage/$new_launcher"

# Embeddable Python + the preparation scripts (as package-windows.sh).
python3 - "$python_zip" "$stage/python" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for member in z.infolist():
        if '..' in member.filename or member.filename.startswith('/'): raise SystemExit('unsafe zip member ' + member.filename)
    z.extractall(sys.argv[2])
PY
pth=$(ls "$stage"/python/python*._pth | head -1)
printf 'python%s.zip\n.\n..\\scripts\n..\\tools\n' "$(basename "$pth" | sed 's/python\([0-9]*\)\._pth/\1/')" > "$pth"
mkdir -p "$stage/scripts"; cp scripts/*.py "$stage/scripts/"
cp tools/gen_import_names.py "$stage/tools/"
for f in tools/ufc3_server.py tools/blaze_tdf.py tools/ufc3_auth.py tools/ufc3_games.py; do [[ -f $f ]] && cp "$f" "$stage/tools/"; done
mkdir -p "$stage/patches"
find patches -maxdepth 1 -type f -name '*.xml' -exec cp {} "$stage/patches/" \; 2>/dev/null || true
if [[ -d patches/experiments ]]; then
    find patches/experiments -mindepth 2 -maxdepth 2 -type f \( -name '*.xml' -o -name '*.json' \) | while read -r f; do
        mkdir -p "$stage/$(dirname "$f")"; cp "$f" "$stage/$f"
    done
fi
cp "$aerolib" "$stage/share/aerolib.inl"
[[ -f share/skip_movie.mkv ]] && cp share/skip_movie.mkv "$stage/share/skip_movie.mkv"
cp LICENSE "$stage/LICENSE"; cp "$root/bin/VERSION" "$stage/VERSION"
cp packaging/windows/README-Windows.md "$stage/README-Windows.md"

# Licences: the submodules, then MSYS2's copies for the libraries it supplied.
cp gpu/third_party/imgui/LICENSE.txt "$stage/licenses/Dear-ImGui-LICENSE.txt"
cp third_party/LibAtrac9/LICENSE "$stage/licenses/LibAtrac9-LICENSE.txt" 2>/dev/null || cp third_party/LibAtrac9/LICENSE.md "$stage/licenses/LibAtrac9-LICENSE.md" 2>/dev/null || true
cp gpu/third_party/fsr-vulkan/LICENSE* "$stage/licenses/" 2>/dev/null || true
cp gpu/third_party/sirit/LICENSE.txt "$stage/licenses/sirit-LICENSE.txt" 2>/dev/null || true
cp gpu/third_party/half/LICENSE.txt "$stage/licenses/half-LICENSE.txt" 2>/dev/null || true
lic=/clang64/share/licenses
for pair in "sdl3:SDL3" "fmt:fmt" "xxhash:xxHash" "zydis:Zydis" "zycore:Zycore" "miniz:miniz" \
            "vulkan-memory-allocator:VulkanMemoryAllocator" "magic_enum:magic_enum" "robin-map:robin-map" \
            "vulkan-headers:Vulkan-Headers" "boost:Boost" "libc++:libc++" "libunwind:libunwind" \
            "winpthreads:winpthreads" "libwinpthread:winpthreads" "xbyak:xbyak"; do
    IFS=: read -r pkg name <<< "$pair"
    if [[ -d $lic/$pkg ]]; then
        for f in "$lic/$pkg"/*; do [[ -f $f ]] && cp "$f" "$stage/licenses/$name-$(basename "$f")"; done
    fi
done
cp "$stage/python/LICENSE.txt" "$stage/licenses/Python-LICENSE.txt" 2>/dev/null || true
cp packaging/windows/THIRD-PARTY.md "$stage/licenses/THIRD-PARTY.md"
echo "package-msys2: licences: $(ls "$stage/licenses" | wc -l) files (check that every shipped DLL has one)"

# Inspection: every PE is AMD64; no build-host paths; no game data.
while IFS= read -r -d '' f; do
    case $f in *.exe|*.dll|*.pyd) llvm-readobj --file-headers "$f" | grep -q IMAGE_FILE_MACHINE_AMD64 || { echo "package-msys2: $f is not AMD64" >&2; exit 1; } ;; esac
done < <(find "$stage" -type f -print0)
if grep -rl -- "$PWD" "$stage/bin" "$stage/$launcher_exe" 2>/dev/null | head -1 | grep -q .; then
    echo "package-msys2: warning: a staged binary contains the build path $PWD" >&2
fi
for forbidden in eboot.bin '*.prx' '*.sav' 'boot*.bin' '*.spv'; do
    if find "$stage" -iname "$forbidden" | grep -q .; then echo "package-msys2: $forbidden inside the archive" >&2; exit 1; fi
done
find "$stage" -name __pycache__ -prune -exec rm -r {} +

mkdir -p dist
archive=dist/$product-windows-x86_64.zip
archive_path=$PWD/$archive
rm -f "$archive"
(cd "$root/stage" && python3 -c "
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1], 'w', zipfile.ZIP_DEFLATED) as z:
    for base, dirs, files in sorted(os.walk(sys.argv[2])):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(base, name)
            info = zipfile.ZipInfo(path.replace(os.sep, '/'), date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = (0o755 if name.endswith('.exe') else 0o644) << 16
            with open(path, 'rb') as f: z.writestr(info, f.read())
" "$archive_path" "$product")
(cd dist && sha256sum "$product-windows-x86_64.zip" > "$product-windows-x86_64.zip.sha256")
{
    printf 'SacredFire %s (Windows x86-64, built in MSYS2 CLANG64)\nsource commit %s\ncompiler %s\nbuild options: BB_LTO=%s BB_AVPLAYER=%s\n' \
        "$built_from" "$(git rev-parse HEAD)" "$(clang --version | head -1)" "${BB_LTO:-OFF}" "${BB_AVPLAYER:-OFF}"
    echo 'files:'
    (cd "$root/stage" && find "$product" -type f | sort | while read -r f; do printf '  %s %s\n' "$(sha256sum "$f" | cut -c1-16)" "$f"; done)
} > "dist/$product-windows-x86_64.zip.build.txt"
ls -lh "$archive"
