#!/bin/sh
# Rebuild docs/SPEC.pdf from SPEC.md (needs pandoc and Google Chrome).
set -e
cd "$(dirname "$0")/.."
pandoc -f markdown-smart SPEC.md -s --css docs/print.css --embed-resources -o docs/SPEC.html
CHROME="${CHROME:-/Applications/Google Chrome.app/Contents/MacOS/Google Chrome}"
"$CHROME" --headless=new --disable-gpu --no-pdf-header-footer \
    --print-to-pdf=docs/SPEC.pdf "file://$PWD/docs/SPEC.html" 2>/dev/null
