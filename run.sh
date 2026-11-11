#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}"
export LIBVA_DRIVER_NAME="${LIBVA_DRIVER_NAME:-genbu}"
exec "$DIR/mpv-compare" "$@"
