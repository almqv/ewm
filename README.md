# Epsilons Window Manager

## About
**ewm** is a tiling window manager for Xorg, forked from suckless'
[dynamic window manager](https://dwm.suckless.org/). On top of dwm it has:

- configuration in Lua (`~/.config/ewm/config.lua`) that reloads itself when
  you save it; a broken edit is reported in the bar and the previous
  configuration stays active
- plugins: every `*.lua` in `~/.config/ewm/plugins/` is loaded after the
  config and can add bindings, rules, layouts, timers and hooks
  (see [examples/plugins](examples/plugins))
- an IPC socket and `ewm-msg` (the dwm-ipc protocol), gaps, clickable
  status text for dwmblocks, keyboard move/resize of floating windows,
  and an autostart script

## FAQ
### Why?
It is a fork. I maintain it, improve it, and make it actually useable rather than saying that everything is "bloat". If you don't like it: then don't use it.

## Installation

### Nix
```sh
nix run github:E-Almqvist/ewm        # try it
nix profile install github:E-Almqvist/ewm
```

On NixOS, with this repository as the flake input `ewm`:

```nix
{
  imports = [ ewm.nixosModules.default ];
  services.xserver.windowManager.ewm = {
    enable = true;
    # configFile = ./config.lua;  # optional system-wide default
  };
}
```

### From source
Needs a C99 compiler, make, pkg-config, libX11, libXft, libXinerama,
fontconfig, yajl and Lua 5.4.

```sh
make                       # LUA=lua on Arch, LUA=lua54 on Fedora
sudo make install          # PREFIX=/usr/local by default
```

`nix develop` provides all of this plus Xephyr, xdotool and clangd;
`bear -- make` writes `compile_commands.json`.

## Configuration
The installed default is [config.lua](config.lua). Copy it to
`~/.config/ewm/config.lua` and edit it; the API is documented in `ewm(1)`.
Programs to start with the session go in `~/.config/ewm/autostart.sh`
(see [examples/autostart.sh](examples/autostart.sh)).

Try changes safely in a nested X server:

```sh
Xephyr :1 & DISPLAY=:1 build/ewm
```

## License
Licensed under [GNU General Public License v3](https://www.gnu.org/licenses/gpl-3.0.en.html), check [LICENSE](LICENSE).
