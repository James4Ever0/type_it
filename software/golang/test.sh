#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

if ! [ -f /usr/include/X11/extensions/Xrandr.h ]; then
    echo "ERROR: X11 development headers are missing."
    echo "Install them with:"
    echo "  sudo apt-get install -y libxrandr-dev libxcursor-dev libxinerama-dev libxi-dev libgl1-mesa-dev libxxf86vm-dev"
    exit 1
fi

echo "Running go vet..."
go vet ./...

echo "Building..."
go build -o type_it .

if command -v xvfb-run >/dev/null 2>&1; then
    echo "Starting app headless for 2 seconds..."
    xvfb-run -a timeout 2s ./type_it || true
else
    echo "Skipping headless run (xvfb-run not found)."
fi

echo "Test complete"
