# Maintainer: E. Almqvist <elalmqvist@gmail.com>
pkgname=ewm
pkgver=1.1
pkgrel=4
pkgdesc="Epsilons Window Manager, a tiling window manager based of the suckless dynamic window manager."
arch=('i686' 'x86_64')
url="https://github.com/E-Almqvist/ewm"
license=('GPL-3.0')
depends=('libx11' 'libxinerama' 'libxft' 'freetype2' 'alacritty' 'yajl')
optdepends=('rofi' 'polybar')
options=(zipman)
install=ewm.install
source=(ewm.desktop)

build() {
	cd "$srcdir/"
	make X11INC=/usr/include/X11 X11LIB=/usr/lib/X11 FREETYPEINC=/usr/include/freetype2
}

package() {
	cd "$srcdir"
	make PREFIX=/usr DESTDIR="$pkgdir" install
	cd "$startdir"
	install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
	install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
	install -Dm644 ewm.desktop "$pkgdir/usr/share/xsessions/ewm.desktop"
	install -Dm755 ewm-start "$pkgdir/usr/local/bin/ewm-start"
}
sha256sums=('17a4add029baca2f90087beea0e1a45c92efd2618ba02297b8cb259fc03f6632')
