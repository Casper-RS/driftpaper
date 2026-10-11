# driftpaper

Animated particle wallpapers for Wayland. Give it any image: driftpaper finds the subject,
takes particle colours from it, and animates particles, a drifting colour fog and a soft
breathing glow around it. It uses well under 1% of one CPU core on a GPU.

![shards preset](docs/preset-shards.jpg)

## Features

- **Works with any image.** The subject's position and size are detected automatically, and the
  particles keep a clear space around it. Images without a clear subject get particles everywhere.
- **Colours from the image.** The particle palette and glow are taken from the picture itself.
- **8 shapes:** triangle, square, spark, circle, ring, diamond, star and plus, mixed by weight.
- **4 motions:** orbit around the subject, rise, fall and drift.
- **7 presets:** shards, stardust, geometric, embers, snow, fireflies and confetti.
- **Live config:** edit the config file while it runs and changes apply within a second.
- **GPU or CPU rendering**, with lower resolution and frame rate on the CPU to keep it usable.
- **Lock screen snapshots:** export a still PNG of the current look.
- **Multi-monitor** support with hotplugging and a per-monitor filter.

| shards | stardust | embers |
|---|---|---|
| ![](docs/preset-shards.jpg) | ![](docs/preset-stardust.jpg) | ![](docs/preset-embers.jpg) |
| **snow** | **fireflies** | **confetti** |
| ![](docs/preset-snow.jpg) | ![](docs/preset-fireflies.jpg) | ![](docs/preset-confetti.jpg) |

## Requirements

A Wayland compositor with `wlr-layer-shell`: COSMIC, Sway, Hyprland, KDE Plasma, river,
Wayfire, labwc and others. GNOME is not supported because Mutter does not offer layer-shell.

## Install

### Flatpak (from Releases)

Download `driftpaper-<version>.flatpak` from the [latest release](https://github.com/Casper-RS/driftpaper/releases/latest), then:

```sh
flatpak install --user driftpaper-*.flatpak
flatpak run io.github.casper_rs.driftpaper ~/Pictures/my-wallpaper.png
```

The Flatpak reads its config from `~/.var/app/io.github.casper_rs.driftpaper/config/driftpaper/config.ini`.

> **Why the Flatpak talks to the compositor directly:** wallpapers use the privileged
> `wlr-layer-shell` protocol, which compositors hide from Flatpak's filtered Wayland socket.
> The Flatpak therefore uses the compositor's own socket (`wayland-0` or `wayland-1`), and has
> read-only access to your home folder so it can open any image.

### Binary (from Releases)

Download `driftpaper-<version>-linux-x86_64.tar.gz`, extract it and copy `driftpaper` into
your `PATH`, for example `~/.local/bin`.

### From source

```sh
# Debian/Ubuntu/Pop!_OS
sudo apt install build-essential libwayland-dev libegl-dev libgles-dev
# Fedora
sudo dnf install gcc make wayland-devel mesa-libEGL-devel mesa-libGLES-devel
# Arch
sudo pacman -S base-devel wayland mesa libglvnd

make
sudo make install            # or: make install PREFIX=~/.local
```

## Usage

```sh
driftpaper ~/Pictures/wall.png                # run with an image
driftpaper -p embers ~/Pictures/wall.png      # use a preset
driftpaper -s count=300 -s motion=drift       # override single settings
driftpaper --list                             # presets, shapes and motions
driftpaper --analyze ~/Pictures/wall.png      # what was detected in the image
driftpaper --print-config > ~/.config/driftpaper/config.ini   # write a config to edit
```

### Start on login

Pick one:

```sh
# systemd user service (installed by make install)
systemctl --user enable --now driftpaper.service

# or XDG autostart, which works on every desktop
cp dist/driftpaper.desktop ~/.config/autostart/
```

For the Flatpak, use `Exec=flatpak run io.github.casper_rs.driftpaper` in the autostart file.

### Matching lock screen

Most lock screens show a still image. Export one that matches your animated wallpaper:

```sh
driftpaper --snapshot ~/Pictures/driftpaper-lock.png --size 2560x1440
```

## Configuration

Settings live in `~/.config/driftpaper/config.ini`. [`examples/config.ini`](examples/config.ini)
lists every setting with an explanation. The most useful ones:

| Setting | Values | Meaning |
|---|---|---|
| `image` | path | The picture to animate |
| `preset` | `shards`, `stardust`, `geometric`, `embers`, `snow`, `fireflies`, `confetti` | Starting point, other keys override it |
| `renderer` | `auto`, `gpu`, `cpu` | `gpu` prefers the dedicated GPU, `cpu` uses software rendering |
| `fps` | `auto` or 1-240 | 30 on the GPU, 12 on the CPU |
| `render_scale` | `auto` or 0.25-1 | Render at a fraction of the resolution (compositor scales it up) |
| `count` | number | How many particles |
| `motion` | `orbit`, `rise`, `fall`, `drift` | How particles move |
| `shapes` | e.g. `triangle:2, spark` | Which shapes, with optional weights |
| `size` | e.g. `9-20` | Size range in pixels at 1440p |
| `speed`, `speed_falloff` | numbers | Speed, and how much faster the outer orbit moves |
| `inner_radius`, `outer_radius` | `auto` or a fraction of the image height | Clear zone around the subject, and the edge of the particle field |
| `colors` | `auto` or `#ff3366, #5577ee` | Particle colours |
| `fog`, `glow` | 0 disables | Background effects |
| `hue_adjust` | e.g. `220, 40, -0.2` | Darken or lighten one colour range of the image |
| `outputs` | `all` or `DP-1, HDMI-A-1` | Which monitors to draw on |

### CPU usage

Measured on two 2560×1440 monitors:

| Renderer | Settings | CPU |
|---|---|---|
| GPU (NVIDIA RTX 4070 Super) | defaults (30 fps) | ~0.8% of one core |
| CPU (Mesa llvmpipe) | defaults (12 fps, render_scale 0.35) | ~75% of one core |

The CPU renderer is meant for machines without working graphics drivers. Lower `fps`,
`render_scale` or set `fog = 0` to make it cheaper.

## Development

```
src/main.c        command line
src/settings.c    combines defaults, preset, config file and --set overrides
src/config.c      settings, presets and config file parsing
src/analyze.c     subject detection and palette extraction
src/render.c      OpenGL ES 2 shaders and particle generation
src/wallpaper.c   Wayland outputs, frame loop and live reload
src/snapshot.c    offscreen rendering to PNG
src/egl.c         EGL setup and renderer selection
```

Releases are built by GitHub Actions when a `v*` tag is pushed. Bump `VERSION` in the
`Makefile`, add a section to `CHANGELOG.md`, then tag:

```sh
git tag v0.2.0 && git push origin v0.2.0
```

The workflow checks that the tag, `Makefile` and changelog agree, builds the binary and the
Flatpak bundle, and publishes them with that version's changelog notes.

## License

MIT, see [LICENSE](LICENSE). Bundles [stb_image and stb_image_write](https://github.com/nothings/stb)
(public domain / MIT) and the Wayland protocol definitions in `protocol/` (MIT).
