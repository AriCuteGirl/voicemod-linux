<p align="center"><img src="ui/icon.png" width="96" alt=""></p>

# Voicemod Linux

Run the **Voicemod** voice changer on Linux. The official Windows engine runs under Wine,
the app window is native (Electron), and your changed voice comes out of a PipeWire
virtual microphone that Discord, games, OBS and anything else can use.

> Unofficial community project. Not affiliated with, endorsed by or supported by Voicemod S.L.
> This repository contains **no Voicemod code or assets**: on first start the launcher downloads
> the official Windows build from Voicemod's own servers into your home folder, and you sign in
> with your own Voicemod account. Free and Pro accounts work exactly as they do on Windows.

## Install

Grab a build from [Releases](../../releases):

| Distro | File |
|---|---|
| Any (recommended) | `voicemod-linux-*.AppImage` |
| Arch, EndeavourOS, Manjaro | `voicemod-linux-*.pacman` (`sudo pacman -U …`) |
| Debian, Ubuntu, Mint, Pop!_OS | `voicemod-linux-*.deb` (`sudo apt install ./…deb`) |
| Fedora, openSUSE | `voicemod-linux-*.rpm` |
| Anything else | `voicemod-linux-*.tar.gz` |

You also need Wine and a few tools. The `.deb`/`.pacman`/`.rpm` packages pull them in;
for the AppImage install them yourself:

```bash
# Arch / EndeavourOS / Manjaro
sudo pacman -S --needed wine winetricks cabextract unzip curl python libpulse fuse2
# Debian / Ubuntu (winetricks is in "contrib" on Debian)
sudo apt install wine winetricks cabextract unzip curl python3 pulseaudio-utils libfuse2
# Fedora
sudo dnf install wine winetricks cabextract unzip curl python3 pulseaudio-utils fuse-libs
```

PipeWire (with `pipewire-pulse`) or PulseAudio is required.

## First start

1. Run the app. The first start sets everything up (about 5 minutes, 240 MB download):
   a private Wine prefix, the Visual C++ runtime, the official Voicemod build, and the
   small compatibility pieces from this repo. Everything lives in
   `~/.local/share/voicemod-linux`; nothing is installed system-wide and no root is needed.
2. Click **Get started**, sign in in your browser, then **Open Voicemod app**.
3. In Voicemod's settings, pick your real microphone as the **input** (not "Default").
4. In Discord/games, choose **"Voicemod Virtual Audio Device (WDM)"** as the microphone.

Voicemod only processes your mic while an app is listening to the virtual mic, just like on
Windows. Closing the window stops the engine and removes the virtual devices.

### Tips

- **Discord:** turn off Krisp noise suppression, echo cancellation and automatic gain, and set
  input sensitivity manually (around −50 dB). Use Voicemod's own *Background Noise Reduction*
  instead. It runs before the voice effect.
- Don't set Voicemod's input to "Default" if the virtual mic is your system default; it would
  record itself.

## How it works

Voicemod for Windows is a Qt app with a web frontend, a PortAudio/WASAPI audio engine and a
kernel-mode virtual audio driver. On Linux:

| Windows piece | Replacement |
|---|---|
| Virtual audio driver | PipeWire null sink + remapped source named like the Windows device |
| Driver control channel (`\\.\VMDriver`) | `driver/vmstub.sys`, a tiny Wine kernel driver that accepts its IOCTLs |
| Driver install/version check | SetupAPI registry entries (`driver/vmstub-device.reg`) |
| "An app opened the virtual mic" detection | `tools/vad-watch.py` watches PipeWire; `shim/` turns that into the Windows audio session + driver event Voicemod expects |
| Qt/WebEngine window (renders blank under Wine) | Voicemod's own frontend, extracted from your `Voicemod.exe` and hosted in Electron |
| Wine gaps (METHOD_NEITHER buffer, stubbed session notifications) | `shim/version.dll`, loaded next to `Voicemod.exe`, patches in memory only |

`Voicemod.exe` is never modified on disk (it verifies its own signature).

## Build

```bash
make                     # shim/version.dll + driver/vmstub.sys (needs mingw-w64)
cd ui && npm ci && npm run dist   # packages into ../dist
```

Run from a checkout without packaging: `./voicemod-linux.sh setup` once, then `./voicemod-linux.sh`.

## Troubleshooting

- Engine log: `~/.local/share/voicemod-linux/prefix/drive_c/users/$USER/AppData/Local/VoicemodV3/logs/voicemod-desktop.log`
- Shim traces: `WINEDEBUG=+debugstr ./voicemod-linux.sh engine --boot` (lines start with `vmshim:`)
- Start over: delete `~/.local/share/voicemod-linux`.

## Known issues

- One voice fails to load (`Error during string conversion NoReverb to float`).
- Voicemod's Control API (port 59129) listens on all network interfaces, like on Windows.
- Tested on Arch-based distros with Hyprland, PipeWire and Wine 11.

## License

MIT for the code in this repository. Voicemod is a trademark of Voicemod S.L.; its software
is downloaded from Voicemod and used under Voicemod's own terms.
