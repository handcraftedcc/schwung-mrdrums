#!/usr/bin/env bash
set -euo pipefail

file="src/ui.js"

rg -F -q "host_open_file_browser('pad_sample_path');" "$file"
rg -F -q "setParamRaw('ui_current_pad', state.currentPad);" "$file"
rg -F -q "setParamRaw('pad_sample_path', '');" "$file"
if rg -q "resolveFileBrowserStartDir" "$file"; then
  echo "FAIL: UI should not use local file browser start-dir workaround" >&2
  exit 1
fi
if rg -q "fileBrowserTargetKey" "$file"; then
  echo "FAIL: UI should not use fileBrowserTargetKey bridge logic" >&2
  exit 1
fi
if rg -q "seedEmptyFilepathFromLastPath" "$file"; then
  echo "FAIL: UI still seeds empty pad sample_path from previous selection" >&2
  exit 1
fi
