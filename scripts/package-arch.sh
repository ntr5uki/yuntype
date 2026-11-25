#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

RAW_VERSION=${1:-0.1.19-1}
if [[ "$RAW_VERSION" == *-* ]]; then
  VERSION="${RAW_VERSION%-*}"
  PKGREL="${RAW_VERSION##*-}"
else
  VERSION="$RAW_VERSION"
  PKGREL="${3:-1}"
fi
OUT=$(realpath -m "${2:-dist}")
mkdir -p "$OUT"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

tar --exclude=build --exclude=dist --exclude=.git --exclude=skills -cf "$work/fcitx5-yuntype-src.tar" .
checksum=$(sha256sum "$work/fcitx5-yuntype-src.tar")

{
  printf 'pkgver=%q\n' "$VERSION"
  printf 'pkgrel=%q\n' "$PKGREL"
  printf "sha256sums=('%s')\n" "${checksum%% *}"
  cat <<'PKGBUILD'
pkgname=fcitx5-yuntype
pkgdesc='YunType: background LLM full-sentence pinyin conversion and in-place replacement module for Fcitx 5'
arch=('x86_64')
url='https://github.com/Gloridust/fcitx5-yuntype'
license=('MIT')
depends=('fcitx5' 'curl' 'python' 'python-gobject' 'gtk4' 'libadwaita')
makedepends=('cmake' 'pkgconf')
options=('!debug')
source=('fcitx5-yuntype-src.tar')

build() {
  cd "$srcdir" || return
  if command -v cmake >/dev/null 2>&1; then
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTING=ON
    cmake --build build -j"$(nproc)"
  else
    mkdir -p build
    local cflags libs fcitx_ver fcitx_minor
    cflags="$(pkg-config --cflags Fcitx5Core Fcitx5Config Fcitx5Utils)"
    libs="$(pkg-config --libs Fcitx5Core Fcitx5Config Fcitx5Utils)"
    fcitx_ver="$(pkg-config --modversion Fcitx5Core)"
    fcitx_minor="$(printf '%s' "$fcitx_ver" | sed -E 's/^([0-9]+\.[0-9]+).*/\1/')"
    # shellcheck disable=SC2086
    g++ -std=c++20 -O2 -fPIC -shared -Icore -Isrc -DFCITX5_YUNTYPE_VERSION="\"$pkgver\"" \
      $cflags src/yuntype.cpp core/llm_client.cpp $libs -pthread \
      -o build/libyuntype.so
    # shellcheck disable=SC2086
    g++ -std=c++20 -O2 -Icore -Isrc -DFCITX5_YUNTYPE_VERSION="\"$pkgver\"" \
      $cflags test/test_yuntype.cpp src/yuntype.cpp core/llm_client.cpp $libs -pthread \
      -o build/test_yuntype
    g++ -std=c++20 -O2 -Icore \
      test/test_core.cpp core/llm_client.cpp -pthread \
      -o build/test_core
    g++ -std=c++20 -O2 -Icore \
      test/test_win_config.cpp windows/src/win_config.cpp core/llm_client.cpp -pthread \
      -o build/test_win_config
    sed -e "s|@PROJECT_VERSION@|$pkgver|g" \
        -e "s|@FCITX_CORE_DEP@|${fcitx_minor}.0|g" \
        conf/yuntype.conf.in > build/yuntype.conf
    sed -e "s|@VERSION@|$pkgver|g" \
        -e "s|@CMAKE_INSTALL_FULL_DATADIR@|/usr/share|g" \
        settings/fcitx5-yuntype-config > build/fcitx5-yuntype-config
  fi
}

check() {
  cd "$srcdir" || return
  ./build/test_core
  ./build/test_win_config
  ./build/test_yuntype
  python3 settings/fcitx5-yuntype-config --self-test
}

package() {
  cd "$srcdir" || return
  if command -v cmake >/dev/null 2>&1 && [[ -f build/CMakeCache.txt ]]; then
    DESTDIR="$pkgdir" cmake --install build
  else
    install -Dm755 build/libyuntype.so "$pkgdir/usr/lib/fcitx5/libyuntype.so"
    install -Dm644 build/yuntype.conf "$pkgdir/usr/share/fcitx5/addon/yuntype.conf"
    install -Dm755 build/fcitx5-yuntype-config "$pkgdir/usr/bin/fcitx5-yuntype-config"
    install -Dm644 data/sentence-prompt.txt "$pkgdir/usr/share/fcitx5-yuntype/sentence-prompt.txt"
    install -Dm644 settings/org.fcitx.Fcitx5.YunType.Config.desktop \
      "$pkgdir/usr/share/applications/org.fcitx.Fcitx5.YunType.Config.desktop"
  fi
  install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
}
PKGBUILD
} > "$work/PKGBUILD"

cd "$work"
PKGDEST="$OUT" PKGEXT=.pkg.tar.zst BUILDDIR="$work" SRCDEST="$work" \
  SRCPKGDEST="$work" LOGDEST="$work" makepkg --noconfirm --force --nosign -d
file="$OUT/fcitx5-yuntype-$VERSION-$PKGREL-$(uname -m).pkg.tar.zst"
[[ -s "$file" ]] || { echo "makepkg did not generate $file" >&2; exit 1; }
sha256sum "$file" > "$file.sha256"
printf '%s\n' "$file"
