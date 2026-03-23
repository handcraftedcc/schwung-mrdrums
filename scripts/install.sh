#!/bin/bash
# Install mrdrums module to Move
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"

cd "$REPO_ROOT"

if [ ! -d "dist/mrdrums" ]; then
    echo "Error: dist/mrdrums not found. Run ./scripts/build.sh first."
    exit 1
fi

echo "=== Installing mrdrums Module ==="

echo "Copying module to Move..."
ssh ableton@move.local "mkdir -p /data/UserData/schwung/modules/sound_generators/mrdrums"
scp -r dist/mrdrums/* ableton@move.local:/data/UserData/schwung/modules/sound_generators/mrdrums/

echo "Setting permissions..."
ssh ableton@move.local "chmod -R a+rw /data/UserData/schwung/modules/sound_generators/mrdrums"

echo ""
echo "=== Install Complete ==="
echo "Module installed to: /data/UserData/schwung/modules/sound_generators/mrdrums/"
echo ""
echo "Restart Schwung to load the new module."
