#!/usr/bin/env bash
# Builds the UI's fonts from your own TrueType files, in place of the bundled
# Liberation Sans. Usage:
#
#   firmware/tools/make_fonts.sh path/to/Regular.ttf path/to/Bold.ttf
#
# The output goes to firmware/components/ui/fonts/custom/, which git ignores, so
# a font you may use but not share (for example one licensed only for your own
# devices) never ends up in the repository. The build uses those files whenever
# they exist; delete the folder to go back to Liberation Sans.
#
# Needs Node.js (for npx lv_font_conv) and the LVGL sources, which the first
# idf.py build downloads into firmware/managed_components.
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 REGULAR.ttf BOLD.ttf" >&2
    exit 1
fi
regular=$1
bold=$2
here=$(cd "$(dirname "$0")/.." && pwd)
out=$here/components/ui/fonts/custom
symbols=$here/managed_components/lvgl__lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff
if [ ! -f "$symbols" ]; then
    echo "LVGL's symbol font isn't there yet: run idf.py build once first" >&2
    exit 1
fi
mkdir -p "$out"

# The same names and sizes as the bundled fonts, so ui.cpp needs no change.
# Characters: printable ASCII, the degree sign and the bullet, plus LVGL's
# check and arrow symbols.
make_font() {
    local name=$1 file=$2 size=$3
    npx --yes lv_font_conv@1.5.2 --font "$file" -r 0x20-0x7E,0xB0,0x2022 \
        --font "$symbols" -r 0xF00C,0xF053,0xF054,0xF077,0xF078 \
        --size "$size" --bpp 1 --format lvgl --no-compress --lv-include lvgl.h \
        -o "$out/$name.c"
    echo "$name"
}
make_font sticky_sans_16 "$regular" 16
make_font sticky_sans_18 "$regular" 18
make_font sticky_sans_24 "$regular" 24
make_font sticky_bold_18 "$bold" 18
make_font sticky_bold_24 "$bold" 24
make_font sticky_bold_34 "$bold" 34
make_font sticky_bold_52 "$bold" 52
echo "Fonts written to $out. Run idf.py fullclean, then build, to use them."
