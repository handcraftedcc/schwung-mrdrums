#!/usr/bin/env bash
set -euo pipefail

file="src/ui.js"

rg -q "function resolveFileBrowserStartDir" "$file"
rg -q "ui_last_sample_dir" "$file"
rg -q "host_open_file_browser\(key, '\\.wav', resolveFileBrowserStartDir\(key\)\)" "$file"
rg -q "function rememberLastSampleDir" "$file"
