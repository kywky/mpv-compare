#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export DISPLAY="${DISPLAY:-:0}"
export XAUTHORITY="${XAUTHORITY:-$HOME/.Xauthority}"
exec "$DIR/mpv-compare" "$@"
