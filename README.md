<div align="center">

# driftpaper

**Animated live wallpapers for Linux, built natively for Wayland.**

A lightweight alternative to Wallpaper Engine: turn any picture into a living wallpaper with
drifting particles, soft glow and visualisers that move with your music, at well under 1% CPU.

[**Download the latest release**](https://github.com/Casper-RS/driftpaper/releases/latest)

![driftpaper with the shards preset](docs/preset-shards.jpg)

<!-- Room for a short demo video or GIF of the animated wallpaper -->

</div>

## About

driftpaper takes any image and brings it to life. It finds the subject of the picture, takes its
colours and animates particles, a drifting colour fog and a breathing glow around it. With music
sync switched on, the wallpaper pulses to whatever your speakers play.

It is written in C with OpenGL ES and runs as a native Wayland background layer. There is no browser engine,
Wine or Proton involved, so it starts instantly and stays out of the way of games and other
applications. A settings window lets you change everything while the wallpaper updates live.

| shards | stardust | embers |
|---|---|---|
| ![](docs/preset-shards.jpg) | ![](docs/preset-stardust.jpg) | ![](docs/preset-embers.jpg) |
| **snow** | **fireflies** | **confetti** |
| ![](docs/preset-snow.jpg) | ![](docs/preset-fireflies.jpg) | ![](docs/preset-confetti.jpg) |

<!-- Room for screenshots of the music visualisers: equaliser ring, liquid halo, beat ripples -->

<img src="docs/settings-window.png" width="420" alt="driftpaper settings window">

## Installation

driftpaper needs a Wayland compositor with `wlr-layer-shell`, such as COSMIC, KDE Plasma,
Hyprland, Sway, river, Wayfire or labwc. GNOME is not supported.

### Flatpak

Download `driftpaper-<version>.flatpak` from the
[latest release](https://github.com/Casper-RS/driftpaper/releases/latest) and install it:

```sh
flatpak install --user driftpaper-*.flatpak
```

### Binary

The release tarball needs GTK 4.14 and libadwaita 1.5 or newer for the settings window.

```sh
tar xf driftpaper-*-linux-x86_64.tar.gz && cd driftpaper-*-linux-x86_64
install -Dm755 driftpaper driftpaper-settings -t ~/.local/bin
install -Dm644 dist/io.github.casper_rs.driftpaper.desktop -t ~/.local/share/applications
install -Dm644 dist/io.github.casper_rs.driftpaper.svg -t ~/.local/share/icons/hicolor/scalable/apps
```

### From source

```sh
# Debian, Ubuntu, Pop!_OS
sudo apt install build-essential libwayland-dev libegl-dev libgles-dev libgtk-4-dev libadwaita-1-dev
# Fedora
sudo dnf install gcc make wayland-devel mesa-libEGL-devel mesa-libGLES-devel gtk4-devel libadwaita-devel
# Arch
sudo pacman -S base-devel wayland mesa libglvnd gtk4 libadwaita

make && sudo make install        # or: make install PREFIX=~/.local
```

### Getting started

Open **driftpaper** from your app menu, choose a picture with **Browse…** and switch on
**Animated wallpaper**. Switch on **Start on login** to keep it running after a reboot.

## Features

- **Any picture.** The subject and its size are detected automatically; colours come from the image.
- **Particles.** 8 shapes, 4 motions (orbit, rise, fall, drift) and 7 ready-made presets.
- **Background effects.** Drifting colour fog, breathing glow, brightness, contrast, saturation
  and colour range adjustments.
- **Music sync.** Reacts to any audio through PipeWire or PulseAudio, with an equaliser ring,
  liquid halo or beat ripples, and a corner badge (equaliser, spectrum dial or vinyl record).
- **Multi-monitor.** A picture per monitor or one stretched across all of them, with hotplug
  support and per-monitor on/off.
- **Sharp on HiDPI** screens, including fractional scaling, with smooth crossfades between looks.
- **Lock screen.** Keeps the COSMIC lock and login screen in sync, or exports a matching still.
- **Light on resources.** About 1% of one CPU core on a GPU; a software renderer is available for
  machines without working graphics drivers.
- **Live configuration.** Use the settings window, the command line (`driftpaper --help`) or
  `~/.config/driftpaper/config.ini`. Every setting is explained in
  [`examples/config.ini`](examples/config.ini).

## License

MIT, see [LICENSE](LICENSE). Bundles [stb_image and stb_image_write](https://github.com/nothings/stb)
and the Wayland protocol definitions in `protocol/`.
