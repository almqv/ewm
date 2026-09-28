self:
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.services.xserver.windowManager.ewm;
in
{
  options.services.xserver.windowManager.ewm = {
    enable = lib.mkEnableOption "ewm, a dwm fork configured in Lua";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "ewm.packages.\${system}.default";
      description = "The ewm package to use.";
    };

    configFile = lib.mkOption {
      type = lib.types.nullOr lib.types.path;
      default = null;
      example = lib.literalExpression "./config.lua";
      description = ''
        System-wide default config.lua, used by users without a
        ~/.config/ewm/config.lua.
      '';
    };
  };

  config = lib.mkIf cfg.enable (
    let
      package =
        if cfg.configFile == null then cfg.package else cfg.package.override { conf = cfg.configFile; };
    in
    {
      services.xserver.windowManager.session = [
        {
          name = "ewm";
          start = ''
            ${package}/bin/ewm &
            waitPID=$!
          '';
        }
      ];
      environment.systemPackages = [ package ];
    }
  );
}
