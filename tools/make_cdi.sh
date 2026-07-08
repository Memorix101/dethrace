#!/usr/bin/env bash
#
# Build a Dreamcast-bootable .cdi from the dethrace SH4 build plus the
# Carmageddon demo assets.
#
# Usage:
#   tools/make_cdi.sh <path-to-extracted-demo> [output.cdi]
#
# <path-to-extracted-demo> is the folder that directly contains the game's
# DATA/ directory (the extracted https://rr2000.cwaboard.co.uk/R4/PC/carmdemo.zip).
# Its contents are placed at the CD root (/cd), so the game finds /cd/DATA and
# the shipped /cd/dethrace.ini.
#
# Run it in Flycast with, for example:
#   ~/flycast-x86_64.AppImage dethrace-dc.cdi

set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

elf="${ELF:-$root/build-dc/dethrace.elf}"
ini="$root/packaging/dreamcast/dethrace.ini"
mkdcdisc="${MKDCDISC:-$here/mkdcdisc}"

demo_dir="${1:-}"
out="${2:-dethrace-dc.cdi}"

if [ -z "$demo_dir" ]; then
    echo "usage: $0 <path-to-DATA-or-its-parent> [output.cdi]" >&2
    exit 1
fi
if [ ! -f "$elf" ]; then
    echo "error: $elf not found. Build the Dreamcast target first." >&2
    exit 1
fi

# Work out how to lay the assets out so they end up at /cd/DATA.
if [ -d "$demo_dir/DATA" ]; then
    # Parent folder that contains DATA/: drop its contents at the CD root.
    data_args=(-D "$demo_dir")
elif [ "$(basename "$demo_dir" | tr '[:lower:]' '[:upper:]')" = "DATA" ]; then
    # The DATA folder itself: include it by name so it becomes /cd/DATA.
    data_args=(-d "$demo_dir")
else
    echo "warning: neither $demo_dir/DATA nor a folder named DATA was found." >&2
    echo "         Pass the DATA folder or the folder that contains it." >&2
    data_args=(-D "$demo_dir")
fi

"$mkdcdisc" \
    -e "$elf" \
    "${data_args[@]}" \
    -f "$ini" \
    -o "$out" \
    -n "dethrace" \
    -N

echo "Wrote $out"
