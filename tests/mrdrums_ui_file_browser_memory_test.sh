#!/usr/bin/env bash
set -euo pipefail

file="src/ui.js"

rg -q "function resolveFileBrowserStartDir" "$file"
rg -q "ui_last_sample_dir" "$file"
rg -q "host_open_file_browser\(key, '\\.wav', resolveFileBrowserStartDir\(key\)\)" "$file"
rg -q "function rememberLastSampleDir" "$file"
rg -F -q "if (currentPath) return currentPath;" "$file"
rg -F -q "const lastPath = getParamRaw('ui_last_sample_dir');" "$file"
rg -F -q "if (lastPath) return lastPath;" "$file"
rg -F -q "setParamRaw('ui_last_sample_dir', samplePath);" "$file"
rg -F -q "seedEmptyFilepathFromLastPath(key);" "$file"
rg -F -q "function seedEmptyFilepathFromLastPath(key) {" "$file"
rg -F -q "if (!getParamRaw(key) && lastPath) {" "$file"
rg -F -q "setParamRaw(key, lastPath);" "$file"
