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
      ../defaults
      ../data
      ../examples
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

  makeFlags = [ "PREFIX=${placeholder "out"}" ];

  postInstall = lib.optionalString (conf != null) ''
    install -Dm644 ${conf} $out/share/ewm/config.lua
  '';

  passthru.providedSessions = [ "ewm" ];

  meta = {
    description = "Epsilons Window Manager, a dwm fork configured in Lua";
    homepage = "https://github.com/almqv/ewm";
    license = lib.licenses.gpl3Only;
    platforms = lib.platforms.linux;
    mainProgram = "ewm";
  };
}
