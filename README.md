# ewm

A tiling window manager for X, forked from [dwm](https://dwm.suckless.org/)
and configured in Lua.

## Install

```sh
curl -fsSL https://raw.githubusercontent.com/almqv/ewm/main/install.sh | sh
```

Or with Nix: `nix profile install github:almqv/ewm`, or on NixOS import
`nixosModules.default` and set `services.xserver.windowManager.ewm.enable = true`.

Then pick "ewm" in your display manager, or put `exec ewm` in `~/.xinitrc`.
To update, run the same command again: a running ewm restarts in place,
keeping your windows.

## Configure

Edit `~/.config/ewm/config.lua` (created on first start from
[defaults/config.lua](defaults/config.lua)). It reloads on save. Plugins go in
`~/.config/ewm/plugins/`, see [examples/plugins](examples/plugins). Reference:
`man ewm`.

## License

[GPLv3](LICENSE)
