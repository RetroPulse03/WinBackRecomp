#!/bin/sh
# Renders src/main/icon/icon.svg into the exe icon (icon.ico) and the GitHub
# social preview (docs/images/social_preview.png). Needs ImageMagick 7.
# The outputs are committed, so this only runs when the artwork changes.
set -e
cd "$(dirname "$0")/.."
SRC=src/main/icon/icon.svg
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

magick -background none -density 768 $SRC -resize 512x512 "$TMP/512.png"
for s in 256 64 48 40 32 24 20 16; do
    magick "$TMP/512.png" -resize ${s}x$s "$TMP/$s.png"
done
magick "$TMP/256.png" "$TMP/64.png" "$TMP/48.png" "$TMP/40.png" "$TMP/32.png" \
    "$TMP/24.png" "$TMP/20.png" "$TMP/16.png" src/main/icon/icon.ico

# 1280x640, the size GitHub recommends for social previews.
mkdir -p docs/images
magick -size 1280x640 gradient:'#1f2a36-#0b1016' \
    \( "$TMP/512.png" -resize 380x380 \) -gravity west -geometry +110+0 -composite \
    -fill '#e8eef4' -font Segoe-UI-Bold -pointsize 92 -gravity northwest -annotate +560+200 'WinBack' \
    -fill '#ff3b30' -annotate +560+300 'Recompiled' \
    -fill '#9fb0c2' -font Segoe-UI -pointsize 34 -annotate +564+430 'Native PC port of the N64 classic' \
    docs/images/social_preview.png
echo "wrote src/main/icon/icon.ico and docs/images/social_preview.png"
