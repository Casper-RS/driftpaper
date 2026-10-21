# Changelog

All notable changes are listed here. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/)
and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [1.0.0] - 2026-10-21

### Added
- Music sync (`music = true`, Music page in the settings window): the glow and particles pulse on
  the beat, with an equaliser ring in the middle of the screen (`music_style = ring`) or a glowing wave
  along the bottom (`wave`), and a small equaliser badge in the bottom-right corner
  (`music_badge`). It listens to the speakers through PulseAudio or PipeWire; libpulse is loaded
  only when music sync is on.
- A Look group on the Music page (`music_size`, `music_brightness`, `music_bars`, `music_shadow`,
  `music_colors`) to change the visualiser's size, brightness, number of bars, shadow and colours.

### Fixed
- The animation froze after login when COSMIC's own background program started on top of it.
- A monitor could stay frozen when the compositor dropped a frame callback.
- Animation became choppy after a day or more of uptime (float precision); the clock now restarts
  with a crossfade every four hours.
- Leaked frame callbacks when surfaces were recreated or monitors hidden.
- A monitor whose EGL surface could not be created was drawn to anyway; it now reports the error.
- Without cosmic-bg running, COSMIC sessions scanned all of /proc every second; now every 5 s.
- An empty `size`/`opacity` value read past the end of the string, and colours such as `1g0000`
  were accepted.
- `nan` and `inf` were accepted for numeric settings (comparisons with NaN always pass the range
  check), feeding NaN into the shaders; a huge `seed` overflowed.
- Settings window: a failed save (e.g. a full disk) could replace the config with a cut-off file.

## [0.3.0] - 2026-10-12

### Added
- Sharp on HiDPI screens: renders at the monitor's real pixel density, including fractional
  scaling (wp-fractional-scale-v1).
- Smooth crossfade when the picture or look changes.
- A different picture per monitor (`image.<monitor> = ...`), or one picture stretched across all
  monitors (`span = true`).
- Monitors section in the settings window: show or hide the wallpaper per monitor, pick a picture
  per monitor, or stretch one across all.
- COSMIC lock screen sync (`lock_screen_sync = true`): exports a still of each monitor a few
  seconds after the look changes and sets it as COSMIC's background, so the lock and login
  screens match.

### Changed
- Pictures are loaded and analysed once and shared, so changing settings no longer reloads them.
- Saving settings without changes no longer rebuilds the wallpaper.
- Settings window: shows a Start button when the wallpaper is on but not running, retries when it
  is switched back on while still shutting down, asks for a picture before switching on, and keeps
  at least one monitor visible.

### Fixed
- Crash when monitors were added or removed during a crossfade.

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

[Unreleased]: https://github.com/Casper-RS/driftpaper/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Casper-RS/driftpaper/compare/v0.3.0...v1.0.0
[0.3.0]: https://github.com/Casper-RS/driftpaper/compare/v0.2.1...v0.3.0
[0.2.1]: https://github.com/Casper-RS/driftpaper/compare/v0.2.0...v0.2.1
[0.2.0]: https://github.com/Casper-RS/driftpaper/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/Casper-RS/driftpaper/releases/tag/v0.1.0
