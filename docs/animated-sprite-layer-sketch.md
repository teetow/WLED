# Animated Sprite Effect Proposal

## Scope

- This is a new Sprite effect.
- The existing GIF effect is unchanged.
- ESP8266 is out of scope for this iteration.

## Proposal

- No bespoke animated file format.
- PNG files live directly on the device.
- ANIMDEF is a basic animation JSON format.
- ANIMDEF defines an array of objects with `filename` and `duration`.
- Use caching strategies on the device for performance only if needed, and only after measuring.

Example ANIMDEF:

```json
[
	{"filename":"cloud-0.png","duration":120},
	{"filename":"cloud-1.png","duration":120}
]
```

## Direction

Most of the custom FX ideas can collapse under the Sprite effect.

## Features

- Segment name references a single image or an ANIMDEF.
- If segment bounds and sprite size disagree, center the sprite instance.
- Never stretch or tile the sprite.
- `mask` is a boolean option.
- When `mask` is enabled, sprite transparency is applied to either:
  - a solid color
  - if Gradient is also checked, a gradient (follow WLED idioms here)
- `instances` is a slider that spawns additional sprites in a Poisson-disc random pattern.
- `instances` is intended for sparse repeated assets such as stars.
- `speed` moves sprite instances horizontally across the screen and wraps at the edges.
- `speed` must play nicely with `instances`.

## Goal

This should satisfy the current sprite and animation needs under one effect.

## Interpreted Contract

This is how I am reading the proposal for implementation purposes.

- Segment name is the asset reference.
- If segment name ends in `.png`, Sprite renders it as a single still sprite.
- If segment name ends in `.json`, Sprite treats it as ANIMDEF.
- ANIMDEF `duration` is authoritative for frame timing.
- `speed` controls horizontal motion only, not animation playback speed.
- PNG alpha is always respected.
- In normal mode, PNG RGB is rendered directly and alpha blends it into the effect output.
- In `mask` mode, PNG RGB is ignored and sprite alpha becomes the coverage mask for either a solid color or a gradient-style fill.
- `instances` creates a stable sparse layout for the active segment and asset. The layout should only be regenerated when a relevant input changes, such as segment size, asset, or instance count.
- `speed` applies a shared horizontal motion field to all instances, with wraparound at the segment edges.
- Sprite assets are never stretched or tiled.
- If sprite dimensions and segment dimensions disagree, the sprite is centered within the segment for the single-instance case.
- For multi-instance mode, each instance uses its own placement position, but the underlying sprite image is still never stretched or tiled.

## Implementation Entry Points

This proposal maps cleanly onto the existing WLED seams.

- Add a new core `Sprite` effect alongside the existing `Image` effect.
- Keep the existing GIF effect unchanged.
- Add a dedicated sprite loader/runtime rather than overloading the current GIF-only loader.
- Add ESP32-only PNG decode support.
- Keep ESP8266 compile-safe, but out of scope for actual Sprite functionality in this iteration.
- Use the existing filesystem upload path for asset delivery.
- Reuse the existing segment-name asset lookup pattern.

## First Implementation Slice

The first code slice should be narrow and testable.

- Add the new Sprite effect registration and metadata.
- Add an ESP32-only sprite loader that resolves segment name to either a `.png` file or an ANIMDEF `.json` file.
- Support centered rendering with no stretch and no tiling.
- Support ANIMDEF frame stepping using per-frame `duration`.
- Support the default single-instance path first, but keep the runtime structure compatible with `instances` and `mask`.
- Defer caching until after we have measured decode cost on actual ESP32 builds.

## Non-Goals For The First Slice

These are explicitly not part of the minimum first milestone.

- Replacing the existing GIF effect
- ESP8266 Sprite playback
- A bespoke packed animation file format
- Premature caching or asset atlasing without measurements
