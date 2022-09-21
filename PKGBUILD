# Maintainer: E. Almqvist <elalmqvist@gmail.com>
pkgname=ewm
pkgver=1.0
pkgrel=1
pkgdesc="Epsilons Window Manager, a tiling window manager based of suckless' dynamic window manager."
arch=('i686' 'x86_64')
url="https://github.com/E-Almqvist/ewm"
license=('GPL-3.0')
depends=('libx11' 'libxinerama' 'libxft' 'freetype2' 'alacritty')
optdepends=('rofi' 'polybar')
options=(zipman)
source=(ewm.desktop)

build() {
	cd "$srcdir/"
	make X11INC=/usr/include/X11 X11LIB=/usr/lib/X11 FREETYPEINC=/usr/include/freetype2
}

package() {
	cd "$srcdir"
	make PREFIX=/usr DESTDIR="$pkgdir" install
	install -Dm644 LICENSE "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
	#install -Dm644 README.md "$pkgdir/usr/share/doc/$pkgname/README.md"
	install -Dm644 "$srcdir/ewm.desktop" "$pkgdir/usr/share/xsessions/ewm.desktop"
}
sha256sums=('4434a1f7047cfc847a21f70674111e68002aea80a9dc08f8b61f55d8f88794c3')
