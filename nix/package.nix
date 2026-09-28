{
  lib,
  stdenv,
  pkg-config,
  libx11,
  libxft,
  libxinerama,
  fontconfig,
  yajl,
  lua5_4,
  # path to a config.lua to install as the system-wide default
  conf ? null,
}:

stdenv.mkDerivation {
  pname = "ewm";
  version = "2.0";

  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [
      ../Makefile
      ../src
      ../config.lua
      ../ewm.1
      ../ewm.desktop
    ];
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [
    libx11
    libxft
    libxinerama
    fontconfig
    yajl
    lua5_4
  ];

  makeFlags = [
    "PREFIX=${placeholder "out"}"
    "LUA=lua5.4"
  ];

  postInstall = lib.optionalString (conf != null) ''
    install -Dm644 ${conf} $out/share/ewm/config.lua
  '';

  passthru.providedSessions = [ "ewm" ];

  meta = {
    description = "Epsilons Window Manager, a dwm fork configured in Lua";
    homepage = "https://github.com/E-Almqvist/ewm";
    license = lib.licenses.gpl3Only;
    platforms = lib.platforms.linux;
    mainProgram = "ewm";
  };
}
