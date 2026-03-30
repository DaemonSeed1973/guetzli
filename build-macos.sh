#!/usr/bin/env bash
# build-macos.sh — Build guetzli for modern macOS (arm64 / x86_64 / universal)
#
# Usage:
#   ./build-macos.sh                  # native arch (default)
#   ./build-macos.sh arm64            # force arm64
#   ./build-macos.sh x86_64           # force x86_64
#   ./build-macos.sh universal        # fat binary (arm64 + x86_64)
#
# Requirements:
#   - Xcode Command Line Tools (xcode-select --install)
#   - libpng  (brew install libpng)

set -euo pipefail

ARCH="${1:-native}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$REPO_ROOT/build/macos"

echo "==> guetzli macOS build  (arch: $ARCH)"

# ---------------------------------------------------------------------------
# Dependency check
# ---------------------------------------------------------------------------
if ! command -v pkg-config &>/dev/null; then
  echo "error: pkg-config not found — brew install pkg-config" >&2
  exit 1
fi

if ! pkg-config --exists libpng; then
  echo "error: libpng not found — brew install libpng" >&2
  exit 1
fi

PNG_CFLAGS="$(pkg-config --cflags libpng)"
PNG_LIBS="$(pkg-config --libs libpng)"

# ---------------------------------------------------------------------------
# Build helpers
# ---------------------------------------------------------------------------
build_arch() {
  local arch="$1"
  local obj_dir="$REPO_ROOT/build/obj/$arch"
  local bin_dir="$REPO_ROOT/build/bin/$arch"
  local target="$bin_dir/guetzli"

  echo "  -> building $arch"
  mkdir -p "$obj_dir" "$bin_dir"

  local arch_flag="-arch $arch"
  local min_ver="-mmacosx-version-min=12.0"
  local cxxflags="-std=c++17 -O3 $arch_flag $min_ver $PNG_CFLAGS \
    -I$REPO_ROOT -I$REPO_ROOT/third_party/butteraugli"
  local ldflags="$arch_flag $min_ver $PNG_LIBS"

  # Collect sources
  local sources=()
  while IFS= read -r -d '' f; do
    sources+=("$f")
  done < <(find "$REPO_ROOT/guetzli" "$REPO_ROOT/third_party/butteraugli" \
    -name '*.cc' -print0 2>/dev/null)

  # Compile each source
  local objects=()
  for src in "${sources[@]}"; do
    local obj="$obj_dir/$(basename "${src%.cc}").o"
    objects+=("$obj")
    c++ $cxxflags -MMD -MP -c "$src" -o "$obj"
  done

  # Link
  c++ $ldflags "${objects[@]}" -o "$target"
  echo "  -> $target"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
mkdir -p "$OUT_DIR"

case "$ARCH" in
  native)
    native_arch="$(uname -m)"
    build_arch "$native_arch"
    cp "$REPO_ROOT/build/bin/$native_arch/guetzli" "$OUT_DIR/guetzli"
    ;;
  arm64|x86_64)
    build_arch "$ARCH"
    cp "$REPO_ROOT/build/bin/$ARCH/guetzli" "$OUT_DIR/guetzli"
    ;;
  universal)
    build_arch "arm64"
    build_arch "x86_64"
    lipo -create \
      "$REPO_ROOT/build/bin/arm64/guetzli" \
      "$REPO_ROOT/build/bin/x86_64/guetzli" \
      -output "$OUT_DIR/guetzli"
    echo "  -> universal binary: $OUT_DIR/guetzli"
    ;;
  *)
    echo "error: unknown arch '$ARCH' — use arm64, x86_64, universal, or native" >&2
    exit 1
    ;;
esac

# Verify
echo "==> Build complete"
file "$OUT_DIR/guetzli"
echo ""
echo "Binary: $OUT_DIR/guetzli"
