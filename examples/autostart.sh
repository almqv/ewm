#!/bin/sh
# Example ~/.config/ewm/autostart.sh (must be executable).
# ewm runs it once in the background after startup; $EWM_SOCKET is set.

# Status bar text, see dwmblocks(1)
pkill -x dwmblocks
dwmblocks >/tmp/dwmblocks.log 2>&1 &

# Compositor
pkill -x picom
while pgrep -u "$(id -u)" -x picom >/dev/null; do sleep 1; done
picom &

# Wallpaper
nitrogen --restore &

# Keyboard layout
# setxkbmap us &

# Display settings
# "$HOME/.screenlayout/layout.sh" &

# GNOME Keyring
# eval "$(gnome-keyring-daemon --start)"
# export SSH_AUTH_SOCK
