#!/usr/bin/env bash
# The release players get: builds barthes_launcher.exe (Release) and puts in dist/:
#   BarthesLauncher-windows-<ver>.zip         barthes_launcher.exe, README, LICENSE, licenses\
#   BarthesLauncher-windows-<ver>.zip.sha256
# Run from an MSYS2 UCRT64 shell: VERSION=1.0.2 tools/package.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
ver="${VERSION:-1.0.2}"
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBARTHES_VERSION="$ver" > /dev/null
cmake --build build-release

# every DLL the exe imports must come with Windows itself (the C/C++ runtimes are linked in statically)
builtin='^(api-ms-win-crt-.*|kernel32|user32|gdi32|shell32|ole32|oleaut32|imm32|dwmapi|d3d11|dxgi|d3dcompiler_47|winhttp|bcrypt|version|advapi32|comdlg32|shlwapi)\.dll$'
objdump -p build-release/barthes_launcher.exe | sed -n 's/^\s*DLL Name: //p' | tr 'A-Z' 'a-z' | sort -u | while read -r dll; do
    grep -Eq "$builtin" <<< "$dll" || { echo "barthes_launcher.exe needs $dll, which Windows does not ship" >&2; exit 1; }
done

stage="$root/dist/BarthesLauncher"
rm -rf "$stage"
mkdir -p "$stage/licenses"
cp build-release/barthes_launcher.exe "$stage/"
cp README.md "$stage/README.md"
cp LICENSE "$stage/licenses/LICENSE.txt"
cp third_party/imgui/LICENSE.txt "$stage/licenses/DearImGui.txt"
zip="BarthesLauncher-windows-$ver.zip"
(cd "$root/dist" && rm -f "$zip" "$zip.sha256" && "$(cygpath "${SYSTEMROOT:-C:/Windows}")/System32/tar.exe" -a -cf "$zip" BarthesLauncher && sha256sum "$zip" > "$zip.sha256")
echo "dist/$zip"
cat "dist/$zip.sha256"
