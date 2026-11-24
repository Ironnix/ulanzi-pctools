# PC Tools for Ulanzi Studio

A lightweight Windows plugin for Ulanzi Studio / Ulanzi Deck: media keys, mic mute, audio output switching, phone audio over Bluetooth, and an SSH/terminal launcher. It doesn't emulate any key presses.

## Why

Ulanzi's built-in *Multimedia* action works by simulating media keys. Games that read raw keyboard scan codes ignore the media flag and see an ordinary letter instead: in Apex Legends, **Play/Pause arrives as `G`** and pulls out your grenade, **Mute as `D`** and **Volume Down as `C`**. This plugin sends the same commands through Windows APIs instead, so no keyboard event is ever created.

## Actions

| Action | What it does | How |
|---|---|---|
| Play / Pause, Next, Previous, Stop | Controls whatever is playing (Spotify, Firefox, YouTube, …) | Windows media session API (SMTC); falls back to `WM_APPCOMMAND` to the taskbar if no session is active |
| Mute (System), Volume Up / Down | System volume | `WM_APPCOMMAND` to the taskbar |
| Mic Mute | Toggles mute on the default microphone; the key shows the live state | Core Audio `IAudioEndpointVolume` |
| Audio Output Switch | Switches the default output between two devices (e.g. headset ↔ speakers) | Core Audio + `IPolicyConfig` |
| Phone Audio | Streams your phone's audio to the PC over Bluetooth (A2DP sink); press again to disconnect | `Windows.Media.Audio.AudioPlaybackConnection` (Windows 10 2004+) |
| SSH / Terminal | Opens `cmd.exe /k <command>` in a new window, e.g. `ssh user@host` (set in the key settings) | — |

## Resource use

- **One Node.js process.** Ulanzi Studio starts one for every Node-based plugin.
- **One native helper, `pchelper.exe`.** About 55 KB, written in plain C, statically linked, and depends only on system DLLs.
- **No polling.** Mic mute, output device and phone connection changes come in as Windows notifications, so the plugin does nothing between key presses.

## Install

1. Download `PCTools-x.y.z.zip` from the [Releases](../../releases) page.
2. Unzip it into `%APPDATA%\Ulanzi\UlanziDeck\Plugins\`, so you end up with `...\Plugins\com.ulanzi.pctools.ulanziPlugin\`.
3. Restart Ulanzi Studio. The actions appear under **PC Tools**.

## Build

You need Node 20+, `x86_64-w64-mingw32-gcc` (Debian/Ubuntu: `gcc-mingw-w64-x86-64`) and `zip`. On Windows, use WSL.

```bash
git clone --recursive https://github.com/Ironnix/ulanzi-pctools.git
cd ulanzi-pctools
npm install
npm run build        # -> dist/PCTools-<version>.zip
```

Pushing a `v*` tag builds the plugin on GitHub Actions and attaches the zip to a release.

### Layout

```
plugin/com.ulanzi.pctools.ulanziPlugin/   manifest, icons, property inspectors (static files)
src/app.js                                plugin service (Node, bundled with esbuild)
src/helper/pchelper.c                     native helper (Win32 / COM / WinRT ABI, no SDK headers needed)
icons/                                    SVG sources of the key icons
sdk/                                      Ulanzi plugin SDK (git submodules, Apache-2.0)
```

### Helper protocol

`pchelper.exe` reads one command per line from stdin and replies on stdout. You can also run it with a single command as arguments for testing, e.g. `pchelper-121.exe media playpause`.

```
media playpause|next|previous|stop|mute|volup|voldown
mic toggle|mute|unmute
list | setout <device id> | status
phones | phone connect|disconnect|toggle|status [device id]
```

It also prints `EVENT mic=…`, `EVENT out=…` and `EVENT phone=…` lines whenever Windows reports a change.

## License

MIT for this plugin. The Ulanzi SDK in `sdk/` is © Ulanzi, Apache-2.0.
