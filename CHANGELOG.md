# Changelog

All notable changes are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.2.1] - 2026-10-11

### Changed
- Starting driftpaper while it is already running now replaces the running wallpaper instead of
  exiting, so `driftpaper other-picture.png` switches pictures. This also works between the
  Flatpak and a native install.

## [0.2.0] - 2026-10-11

### Added
- Settings window (`driftpaper-settings`): pick an image with a file browser, preview it, choose
  a preset and edit every setting while the wallpaper updates live.
- Turn the wallpaper on or off and start it on login from the settings window.
- Export a matching lock screen image from the settings window.
- `enabled` setting: a running wallpaper exits when it is switched off.
- App icon, launcher entry and AppStream metadata.

### Changed
- Only one wallpaper runs at a time.
- The Flatpak is built on the GNOME 50 runtime and opens the settings window.
- The release binary is built on Ubuntu 24.04 and includes the settings window.

## [0.1.0] - 2026-10-11

### Added
- Animated wallpaper for Wayland compositors with wlr-layer-shell (COSMIC, Sway, Hyprland, KDE Plasma, river).
- Automatic image analysis: finds the subject's centre and size, and builds a particle palette from its colours.
- Eight particle shapes (triangle, square, spark, circle, ring, diamond, star, plus) with weighted mixing.
- Four motions: orbit around the subject, rise, fall and drift.
- Presets: shards, stardust, geometric, embers, snow, fireflies, confetti.
- Animated colour fog and a breathing glow around the subject.
- Image adjustments: brightness, contrast, saturation and per-hue lightness.
- `renderer = auto | gpu | cpu`, with `render_scale` and `fps` defaults tuned per renderer.
- Config file that reloads live, plus `--set key=value` overrides.
- `--snapshot` to export a still PNG, for example for the lock screen.
- `--analyze` to show what was detected in an image.
- Monitor hotplug support and a per-monitor `outputs` filter.
- Flatpak bundle and Linux binary on every GitHub release.

[Unreleased]: https://github.com/Casper-RS/driftpaper/compare/v0.2.1...HEAD
[0.2.1]: https://github.com/Casper-RS/driftpaper/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/Casper-RS/driftpaper/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Casper-RS/driftpaper/releases/tag/v0.1.0
