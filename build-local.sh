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

if [[ ! -f "$WLED_DIR/platformio.ini" ]]; then
  echo "WLED checkout not found at: $WLED_DIR" >&2
  exit 1
fi

mkdir -p "$WLED_DIR/usermods/$USERMOD_NAME"
rsync -a --delete \
  --exclude .git \
  --exclude .pio \
  --exclude build_output \
  "$USERMOD_DIR/" "$WLED_DIR/usermods/$USERMOD_NAME/"

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
  printf '  %s\n' "$USERMOD_NAME"
} >> "$WLED_DIR/$ALT_CONF"

cd "$WLED_DIR"
python3 -m platformio run -c "$ALT_CONF" -e "$ALT_ENV"
