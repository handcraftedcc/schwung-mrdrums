#!/usr/bin/env bash
set -euo pipefail

file="src/ui.js"

rg -q "PAD_NOTE_MIN = 36" "$file"
rg -q "PAD_NOTE_MAX = 51" "$file"
rg -q "function applyPadNoteSelection" "$file"
rg -q "state.currentPad = pad" "$file"
rg -q "state.selectedPadParamIndex" "$file"
