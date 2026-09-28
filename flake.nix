{
  description = "ewm - Epsilons Window Manager, a dwm fork configured in Lua";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        ewm = pkgs.callPackage ./nix/package.nix { };
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.ewm;
      });

      overlays.default = final: prev: { ewm = final.callPackage ./nix/package.nix { }; };

      nixosModules.default = import ./nix/module.nix self;

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.ewm ];
          packages = with pkgs; [
            bear # compile_commands.json: bear -- make
            clang-tools
            lua-language-server
            xorg-server # Xephyr for testing: Xephyr :1 & DISPLAY=:1 build/ewm
            xdotool
            xterm
          ];
        };
      });

      checks = forAllSystems (pkgs: {
        # the package, with every compiler warning an error
        build = self.packages.${pkgs.stdenv.hostPlatform.system}.ewm.overrideAttrs (old: {
          makeFlags = old.makeFlags ++ [ "CFLAGS=-O2 -Werror" ];
        });
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
