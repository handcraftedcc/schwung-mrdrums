#!/usr/bin/env bash
set -euo pipefail

file="src/ui.js"

rg -q "function activePadKey" "$file"
rg -q "padStart\(2, '0'\)" "$file"
rg -q 'p\$\{String\(state.currentPad\)' "$file"
