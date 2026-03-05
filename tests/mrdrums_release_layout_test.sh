#!/usr/bin/env bash
set -euo pipefail

[ -f dist/mrdrums/module.json ]
[ -f dist/mrdrums/dsp.so ]
[ -f dist/mrdrums/ui.js ]

rg -q '"id"\s*:\s*"mrdrums"' dist/mrdrums/module.json
rg -q '"name"\s*:\s*"MrDrums"' dist/mrdrums/module.json
