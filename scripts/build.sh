#!/usr/bin/env bash
# Builds the plugin into dist/ and zips it.
# Needs: node/npm, x86_64-w64-mingw32-gcc (apt: gcc-mingw-w64-x86-64), zip
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(node -p "require('./package.json').version")
BUILD=${VERSION//./}
NAME=com.ulanzi.pctools.ulanziPlugin
OUT=dist/$NAME

rm -rf dist && mkdir -p "$OUT/plugin" "$OUT/libs"
cp -r plugin/$NAME/. "$OUT/"
cp -r sdk/plugin-common-html/{js,css,assets} "$OUT/libs/"

# native helper (versioned file name so an update never collides with a running, locked helper)
x86_64-w64-mingw32-gcc -Os -s -static -Wall -Wl,--no-insert-timestamp -o "$OUT/plugin/pchelper-$BUILD.exe" src/helper/pchelper.c -lole32 -luuid

# plugin service, bundled into a single file
npx esbuild src/app.js --bundle --platform=node --target=node20 --format=cjs \
  --external:bufferutil --external:utf-8-validate --log-level=warning \
  --define:__HELPER_NAME__="\"pchelper-$BUILD.exe\"" --outfile="$OUT/plugin/app.js"

node -e "const f='$OUT/manifest.json',m=require('./'+f);m.Version='$VERSION';require('fs').writeFileSync(f,JSON.stringify(m,null,2))"

(cd dist && zip -qr "PCTools-$VERSION.zip" "$NAME")
echo "Built dist/PCTools-$VERSION.zip"
