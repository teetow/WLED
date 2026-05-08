#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "Usage: $0 /path/to/WLED [pio-env]" >&2
  exit 1
fi

USERMOD_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
WLED_DIR=$(cd -- "$1" && pwd)
PIO_ENV=${2:-esp32dev}
USERMOD_NAME=wled-sprite-usermod
ALT_CONF=platformio.sprite.ini
ALT_ENV=${PIO_ENV}_sprite_um
USERMOD_PATH=$(python3 -c 'import pathlib, sys; print(pathlib.Path(sys.argv[1]).resolve())' "$USERMOD_DIR")
USERMOD_SPEC="$USERMOD_NAME = symlink://$USERMOD_PATH"
USERMOD_LINK="$WLED_DIR/.pio/libdeps/$ALT_ENV/$USERMOD_NAME.pio-link"

if [[ ! -f "$WLED_DIR/platformio.ini" ]]; then
  echo "WLED checkout not found at: $WLED_DIR" >&2
  exit 1
fi

awk '
BEGIN{skip=0}
/^extra_configs =/ {print; skip=1; next}
skip==1 && /platformio_override\.ini/ {next}
skip==1 && /platformio_release\.ini/ {print; skip=0; next}
{print}
' "$WLED_DIR/platformio.ini" > "$WLED_DIR/$ALT_CONF"

{
  printf '\n[env:%s]\n' "$ALT_ENV"
  printf 'extends = env:%s\n' "$PIO_ENV"
  printf 'custom_usermods =\n'
  printf '  ${env:%s.custom_usermods}\n' "$PIO_ENV"
  printf '  %s\n' "$USERMOD_SPEC"
} >> "$WLED_DIR/$ALT_CONF"

rm -f "$USERMOD_LINK"

cd "$WLED_DIR"
python3 -m platformio run -c "$ALT_CONF" -e "$ALT_ENV"
