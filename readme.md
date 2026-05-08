# WLED Sprite usermod

This repository packages the Sprite effect as a standalone WLED usermod, following the structure from the official `wled-usermod-example` template.

## What it does

- Registers a dynamic `Sprite UM` effect at runtime using `strip.addEffect(255, ...)`
- Resolves the active segment name to either a single PNG file or an ANIMDEF JSON file
- Caches decoded PNG frames in segment data for replay
- Supports sparse multi-instance placement, mask mode, palette gradients, and horizontal motion
- Remains ESP32-only, matching the current implementation scope

## Effect usage

1. Build WLED with this usermod enabled.
2. Upload your asset files to the WLED filesystem.
3. Set the segment name to your asset file name, for example `cloud.png` or `cloud.json`.
4. Select the `Sprite UM` effect for that segment.

### Controls

- `Speed`: horizontal motion speed
- `Intensity`: sprite instance count
- `Check 1` (`Mask`): use sprite alpha as a mask over the primary segment color
- `Check 2` (`Gradient`): in mask mode, use palette gradient fill instead of flat primary color
- `Check 3` (`Horizontal Gradient`): use a horizontal rather than vertical gradient axis in mask mode

### ANIMDEF format

Use a JSON array of frame objects:

```json
[
  {"filename": "cloud-0.png", "duration": 120},
  {"filename": "cloud-1.png", "duration": 120}
]
```

- `filename` may be absolute (`/cloud-0.png`) or relative to the JSON file location
- `duration` is in milliseconds and controls frame dwell time

## Local development

Clone this repo alongside WLED, then reference it from `platformio_override.ini`:

```ini
[env:esp32dev]
extends = env:esp32dev
custom_usermods =
  ${env:esp32dev.custom_usermods}
  file:///absolute/path/to/wled-sprite-usermod
```

In this dev container, `platformio_override.ini` hangs during PlatformIO project
processing. Use the included helper instead:

```sh
./build-local.sh /absolute/path/to/WLED esp32dev
```

On Windows, use the PowerShell helper for faster rebuilds:

```powershell
.\build-local.ps1 D:\path\to\WLED esp32dev
```

To build and deploy straight to the physical panel on the local network:

```powershell
.\build-local.ps1 D:\path\to\WLED esp32dev -DeployHost wled-32.lan
```

To deploy the already-built firmware without rebuilding first:

```powershell
.\build-local.ps1 D:\path\to\WLED esp32dev -DeployHost wled-32.lan -SkipBuild
```

Both helpers point `custom_usermods` directly at this repo via a local
`symlink://...` entry, so edits are visible to the WLED checkout immediately
without re-copying files. They generate a temporary `platformio.sprite.ini`
that bypasses `platformio_override.ini` and build a derived environment named
`esp32dev_sprite_um`. They also refresh the local `.pio-link` entry before each
build so the usermod path stays in sync if you switch helper strategies or move
the repo. The PowerShell helper can also POST the built firmware to WLED's
native `/update` endpoint and wait for the panel to reboot with `sprite_um`
available again.

## Publish by URL

This usermod is published from the `teetow/WLED` fork as a dedicated branch whose
root contains only the standalone usermod files. Others can consume it directly:

```ini
custom_usermods =
  ${env:esp32dev.custom_usermods}
  https://github.com/teetow/WLED.git#wled-sprite-usermod
```

## Notes

- This usermod is derived from the current Sprite effect work on the `sprite-effect-cache` branch.
- It intentionally does not ship demo images; usage examples live in this README.
- The code expects ESP32 builds and a 2D segment.
