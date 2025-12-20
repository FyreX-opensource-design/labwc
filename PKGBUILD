# Maintainer: Your Name <your.email@example.com>
# Contributor: Your Name <your.email@example.com>
# Git-based PKGBUILD for FyreX fork with device blacklisting features
pkgname=labwc-fyrex-git
pkgver=0.9.2.r114.gcd3c126c
pkgrel=1
pkgdesc="A wlroots-based window-stacking compositor for Wayland, inspired by Openbox (FyreX fork with device blacklisting features)"
arch=('x86_64' 'aarch64')
url="https://github.com/FyreX-opensource-design/labwc"
license=('GPL-2.0-only')
depends=(
  'wlroots0.19'
  'wayland'
  'libinput'
  'libxkbcommon'
  'libxml2'
  'glib2'
  'cairo'
  'pango'
  'pixman'
  'libpng'
  'libdrm'
)
makedepends=(
  'git'
  'meson'
  'ninja'
  'wayland-protocols'
  'scdoc'
)
optdepends=(
  'librsvg: SVG window button support'
  'libsfdo: Window icon support'
  'xorg-xwayland: XWayland support for X11 applications'
  'xcb-util-wm: XWayland support'
)
provides=('labwc')
conflicts=('labwc' 'labwc-git')
source=("git+https://github.com/FyreX-opensource-design/labwc.git")
sha256sums=('SKIP')

pkgver() {
  cd "$srcdir/labwc"
  git describe --long --tags 2>/dev/null | sed 's/^v//;s/\([^-]*-g\)/r\1/;s/-/./g' || echo "0.9.2.r$(git rev-list --count HEAD).g$(git rev-parse --short HEAD)"
}

build() {
  arch-meson labwc build \
    -Dman-pages=disabled \
    -Dxwayland=auto \
    -Dsvg=enabled \
    -Dicon=enabled \
    -Dnls=auto

  meson compile -C build
}

check() {
  meson test -C build --print-errorlogs || true
}

package() {
  DESTDIR="$pkgdir" meson install -C build

  # Install license
  install -Dm644 labwc/LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
}
