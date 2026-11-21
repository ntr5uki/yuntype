#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=${1:-0.1.0}
OUT=$(realpath -m "${2:-dist}")
mkdir -p "$OUT"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

tar --exclude=build --exclude=dist --exclude=.git -cf "$work/fcitx5-llm-sentence-src.tar" .
checksum=$(sha256sum "$work/fcitx5-llm-sentence-src.tar")

{
  printf 'pkgver=%q\n' "$VERSION"
  printf "sha256sums=('%s')\n" "${checksum%% *}"
  cat <<'PKGBUILD'
pkgname=fcitx5-llm-sentence
pkgrel=1
pkgdesc='Background LLM full-sentence pinyin conversion and in-place replacement module for Fcitx 5'
arch=('x86_64')
url='https://github.com/Gloridust/fcitx5-llm-sentence'
license=('MIT')
depends=('fcitx5' 'curl' 'python' 'python-gobject' 'gtk4' 'libadwaita')
makedepends=('cmake' 'pkgconf')
options=('!debug')
source=('fcitx5-llm-sentence-src.tar')

build() {
  cd "$srcdir" || return
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_TESTING=ON
  cmake --build build -j"$(nproc)"
}

check() {
  cd "$srcdir" || return
  ./build/test_llm_sentence
  python3 settings/fcitx5-llm-sentence-config --self-test
}

package() {
  cd "$srcdir" || return
  DESTDIR="$pkgdir" cmake --install build
  install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
}
PKGBUILD
} > "$work/PKGBUILD"

cd "$work"
PKGDEST="$OUT" PKGEXT=.pkg.tar.zst BUILDDIR="$work" SRCDEST="$work" \
  SRCPKGDEST="$work" LOGDEST="$work" makepkg --noconfirm --force --nosign -d
file="$OUT/fcitx5-llm-sentence-$VERSION-1-$(uname -m).pkg.tar.zst"
[[ -s "$file" ]] || { echo "makepkg did not generate $file" >&2; exit 1; }
sha256sum "$file" > "$file.sha256"
printf '%s\n' "$file"
