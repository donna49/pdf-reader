#!/usr/bin/env bash
# Cross-compile FeatherPDF for Windows (ARM64 and/or x64) from Linux.
#
#   ./build.sh            # builds both arm64 and x64
#   ./build.sh arm64      # Windows on ARM (Snapdragon, e.g. Surface Laptop 7)
#   ./build.sh x64        # Intel/AMD Windows
#
# Requires: curl, tar, xz, make, bash. The llvm-mingw toolchain and the MuPDF
# source are downloaded (and hash-verified) into ./deps on first run.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DEPS="$ROOT/deps"
OUT="$ROOT/dist"
JOBS="$(nproc 2>/dev/null || echo 4)"

MUPDF_VER="1.26.10"
MUPDF_URL="https://mupdf.com/downloads/archive/mupdf-${MUPDF_VER}-source.tar.gz"
MUPDF_SHA="1653f35bd8fbd970f05523efdc7f86e41e9728e2564a3295296e03cf59a51437"

LLVM_MINGW_VER="20251021"
LLVM_MINGW_NAME="llvm-mingw-${LLVM_MINGW_VER}-ucrt-ubuntu-22.04-x86_64"
LLVM_MINGW_URL="https://github.com/mstorsjo/llvm-mingw/releases/download/${LLVM_MINGW_VER}/${LLVM_MINGW_NAME}.tar.xz"
LLVM_MINGW_SHA="7b7f3acb6e735a9fabfaa3c8ff551e9937a5f2f298c2cd12796b357f7d1fb6a8"

fetch() { # url sha dest
	if [ ! -f "$3" ]; then
		echo ">> downloading $1"
		curl -fL --retry 5 --retry-delay 3 -o "$3.part" "$1"
		mv "$3.part" "$3"
	fi
	echo "$2  $3" | sha256sum -c -
}

mkdir -p "$DEPS" "$OUT"

if [ ! -d "$DEPS/$LLVM_MINGW_NAME" ]; then
	fetch "$LLVM_MINGW_URL" "$LLVM_MINGW_SHA" "$DEPS/llvm-mingw.tar.xz"
	tar -xJf "$DEPS/llvm-mingw.tar.xz" -C "$DEPS"
fi
export PATH="$DEPS/$LLVM_MINGW_NAME/bin:$PATH"

MUPDF="$DEPS/mupdf-${MUPDF_VER}-source"
if [ ! -d "$MUPDF" ]; then
	fetch "$MUPDF_URL" "$MUPDF_SHA" "$DEPS/mupdf.tar.gz"
	tar -xzf "$DEPS/mupdf.tar.gz" -C "$DEPS"
fi

# Trim MuPDF down to a PDF-only engine. Fonts that are almost always embedded
# in real PDFs (CJK fallback, emoji, historic scripts...) are left out.
MUPDF_FLAGS=(
	OS=MINGW build=release
	HAVE_X11=no HAVE_GLUT=no HAVE_CURL=no HAVE_PTHREAD=no HAVE_OBJCOPY=no
	html=no xps=no svg=no extract=no mujs=no brotli=no barcode=no tesseract=no
)
MUPDF_XCFLAGS="-DTOFU -DTOFU_CJK -DTOFU_SIL -DTOFU_EMOJI -DTOFU_HISTORIC \
 -DFZ_ENABLE_CBZ=0 -DFZ_ENABLE_IMG=0 -DFZ_ENABLE_OCR_OUTPUT=0 -DFZ_ENABLE_ODT_OUTPUT=0 \
 -DFZ_ENABLE_DOCX_OUTPUT=0 -Wno-unused-but-set-variable"

build_arch() {
	local arch="$1" triple archflags
	case "$arch" in
		arm64) triple=aarch64-w64-mingw32; archflags="" ;;
		x64)   triple=x86_64-w64-mingw32; archflags="-msse4.1" ;;  # every Windows 11 x64 CPU has SSE4.1
		*) echo "unknown arch $arch"; exit 1 ;;
	esac
	echo ">> building MuPDF for $arch"
	make -C "$MUPDF" -j"$JOBS" "${MUPDF_FLAGS[@]}" \
		OUT="build/$arch" \
		CC="$triple-clang" CXX="$triple-clang++" AR="$triple-ar" RANLIB="$triple-ranlib" \
		XCFLAGS="$MUPDF_XCFLAGS $archflags" libs

	echo ">> building FeatherPDF for $arch"
	local obj="$OUT/obj-$arch"
	mkdir -p "$obj" "$OUT/$arch"
	"$triple-windres" -I "$ROOT/src" -o "$obj/feather.res.o" "$ROOT/src/feather.rc"
	"$triple-clang" -std=gnu11 -O2 -Wall -Wno-unused-parameter -municode -mwindows \
		-ffunction-sections -fdata-sections \
		-I "$MUPDF/include" \
		"$ROOT/src/feather.c" "$obj/feather.res.o" \
		-L "$MUPDF/build/$arch" -lmupdf -lmupdf-third \
		-lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32 -ladvapi32 \
		-static -Wl,--gc-sections -s \
		-o "$OUT/$arch/FeatherPDF.exe"
	ls -l "$OUT/$arch/FeatherPDF.exe"
}

ARCHES=("${@:-arm64 x64}")
for a in ${ARCHES[@]}; do build_arch "$a"; done
