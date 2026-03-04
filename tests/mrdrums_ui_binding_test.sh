#!/usr/bin/env bash
set -euo pipefail

file="src/ui.js"

rg -q "function activePadKey" "$file"
rg -q "padStart\(2, '0'\)" "$file"
rg -q 'p\$\{String\(state.currentPad\)' "$file"
rg -q "function encoderIndexForCc" "$file"
rg -F -q "if (cc >= 14 && cc <= 21) return cc - 14;" "$file"
rg -F -q "if (cc >= 71 && cc <= 78) return cc - 71;" "$file"
rg -q "suffix: 'pan'.*step: 0.1" "$file"
rg -q "suffix: 'tune'.*step: 1.0" "$file"
rg -q "suffix: 'start'.*step: 0.01" "$file"
rg -q "suffix: 'decay_ms'.*step: 5.0" "$file"
