#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

# Ebiten needs X11 development headers on Linux.
if ! [ -f /usr/include/X11/extensions/Xrandr.h ]; then
    echo "ERROR: X11 development headers are missing."
    echo "Install them with:"
    echo "  sudo apt-get install -y libxrandr-dev libxcursor-dev libxinerama-dev libxi-dev libgl1-mesa-dev libxxf86vm-dev"
    exit 1
fi

echo "Downloading Go dependencies..."
go mod tidy

echo "Building type_it..."
go build -o type_it .

echo "Build complete: ./type_it"
