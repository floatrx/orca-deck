#!/bin/sh
# Renders Icon/apex.svg to the watch app icon: 1024×1024, opaque (Apple rejects alpha in app icons).
# Uses Quick Look (WebKit) for the SVG and ImageMagick to drop the alpha channel.
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
OUT="$DIR/../Apex/Assets.xcassets/AppIcon.appiconset/AppIcon.png"
TMP=$(mktemp -d)
qlmanage -t -s 1024 -o "$TMP" "$DIR/apex.svg" >/dev/null 2>&1
magick "$TMP/apex.svg.png" -background '#071624' -alpha remove -alpha off "$OUT"
rm -r "${TMP:?}"
echo "wrote $OUT"
