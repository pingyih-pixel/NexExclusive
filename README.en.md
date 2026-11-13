# NexExclusive

[![build](https://github.com/pingyih-pixel/NexExclusive/actions/workflows/build.yml/badge.svg)](https://github.com/pingyih-pixel/NexExclusive/actions/workflows/build.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

[简体中文](README.md) · **English**

WASAPI exclusive output for the NetEase Cloud Music desktop client. Audio bypasses the Windows mixer and reaches the sound card at the source's own sample rate. Windows 10/11 x64.

<!-- TODO: add a screenshot of the settings panel as docs/panel.png and reference it here -->

Aimed at listeners with an external DAC or headphone amp who want a 44.1 kHz track to play out at 44.1 kHz.

## Features

- **Exclusive output** — audio skips the system mixer, so there is no forced resample to 48/96 kHz.
- **Follows the source** — 44.1 plays at 44.1, 96 plays at 96. Conversion happens only when the device rejects the format, and the panel says which case applied: "passthrough" or "converted".
- **No interruption on change** — track changes, seeks and output-device switches hand over smoothly from the old output to the new one.
- **Install once** — one-click install, tray resident, autostart; it re-attaches after a client restart or a page reload.

## Before you install

Please read these three points first.

**Defender will quarantine the program.** The host program loads a DLL into the client process with `CreateRemoteThread`, and that behavior alone triggers Defender's behavioral detection (`Behavior:Win32/DefenseEvasion.A!ml`). The exe is quarantined within 30–60 seconds, and the shortcut, autostart entry and uninstall entry are removed with it. The project considers this a false positive; the submission material is in [docs/defender-false-positive.md](docs/defender-false-positive.md) (Chinese and English), and the full source is public for you to check.

Before installing, open PowerShell as administrator and exclude the two files:

```powershell
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive\NexExclusive.exe"
Add-MpPreference -ExclusionPath "$env:LOCALAPPDATA\NexExclusive\nexsink.dll"
```

If you run it straight from the extracted folder, exclude the same two files there as well. Use the program only if you are willing to add these exclusions, and keep real-time protection on.

**Not affiliated with NetEase.** This is an unofficial tool. Loading a DLL into a third-party client may violate its terms of service; use at your own risk.

**System and device.** Windows 10/11 x64. In the Windows sound properties ("Advanced" tab), the playback device must allow applications to take exclusive control. Only client version 3.1.42.205594 has been verified; see [Compatibility](#compatibility).

## Download and first run

**There is no official Release yet.** The Defender false positive is unresolved, and publishing a build that is deleted thirty seconds after install would spend the first impression for nothing, so the Release is on hold; the reasoning is in [docs/ROADMAP.md](docs/ROADMAP.md) (Chinese). Two ways to get the program:

1. Open [Actions](https://github.com/pingyih-pixel/NexExclusive/actions/workflows/build.yml), go to the latest successful run and download the artifact `NexExclusive-package` (downloading artifacts requires a GitHub login). It contains `NexExclusive-x.y.z.zip` and `SHA256SUMS.txt`.
2. Build it yourself; see [docs/BUILDING.md](docs/BUILDING.md).

Once a Release is published, this section will link to it.

After downloading, check the hash; the output should match the corresponding line in `SHA256SUMS.txt`:

```powershell
Get-FileHash .\NexExclusive-x.y.z.zip -Algorithm SHA256
```

First run:

1. Unzip and run `NexExclusive.exe`. It offers to install into `%LOCALAPPDATA%\NexExclusive`, add an autostart entry and create a Start Menu shortcut; installation itself does not need admin rights.
2. After installation it launches the client and a tray icon appears. From then on, just use the client as usual.
3. Tray menu: open the client / view logs / open program folder / quit (stops injection).

Command line:

```
NexExclusive.exe --install [--client "D:\...\cloudmusic.exe"]
NexExclusive.exe --show
NexExclusive.exe --uninstall
NexExclusive.exe --help
```

Use `--client` to point at `cloudmusic.exe` when the client lives in a non-default location and auto-detection fails.
`--show` reuses the existing main process and tries to bring its window forward; it starts a new instance only when no client process exists. If the log reports `OpenProcess(<pid>) failed 5`, close the normal host and start it as administrator.

## Settings panel

The Settings page gets a separate "NexExclusive" entry with the exclusive status, output mode, format, and compatibility controls. The Playback page keeps an "Exclusive output" panel as a quick access point.

| Setting | What it does | When to change it |
| --- | --- | --- |
| Enable exclusive | Master switch; off returns to the client's default output | To compare against shared mode briefly, or when troubleshooting |
| Bit-depth negotiation | Source-rate first follows an exclusive format the device accepts; strict native follows the client format | There is no fixed UI selector, so the source representation is not silently rewritten |
| Sample rate | Follow source (default), or force 44.1 / 48 / 88.2 / 96 kHz | When you want every track to leave at the same rate |
| Buffer frames | Auto, or 512–4096 | Raise it if you hear crackling; lower it if latency matters |
| Output policy | `Source-rate first (recommended)` tries the source format first, then exclusive conversion; `Strict native` accepts only the source format | Use source-rate first for normal listening and strict native when verifying a compatible endpoint |
| Compatibility conversion | Exclusive rate or depth conversion after the source format is rejected | On in source-rate-first mode; strict native disables it |
| Dither | Off / on / auto | Auto by default; it applies when bit depth is reduced or audio was resampled, and rarely needs a change |
| Fade-in | 0–20 ms | Raise it if playback starts with a pop |
| Shared fallback | Opens a shared stream after exclusive output fails | Always off in the product path |

The same settings live in `hook.cfg` as plain `key=value`; restart the client to apply. The panel is a front-end for that file. Strict native verification targets client PCM to exclusive endpoint PCM; when the endpoint accepts the source format, this can produce bit-perfect output. If it rejects the source format, the UI asks you to switch to source-rate first instead of opening shared output.

## How it works

When the client is not running, the host `NexExclusive.exe` starts it with `--remote-debugging-port=9333`, then loads `nexsink.dll` into the client process using `CreateRemoteThread` and `LoadLibraryA`. Inside the process the DLL hooks the `IMMDevice` and `IAudioClient` vtables: when the client negotiates its output format, the DLL answers with the device's real exclusive-mode capabilities, and buffering, resampling and the hand-over on changes also happen there.

The host injects the settings panel into the client page through the local CDP port (9333). The panel talks to the host through a config endpoint on `127.0.0.1:9334`, which reads and writes `hook.cfg` and `ui.json`.

While the program runs, ports 9333 and 9334 are open on the loopback address, and other processes on the same machine can reach them. All network traffic of the host stays on loopback, and the DLL itself contains no network code. The client's files are not modified.

## Compatibility

| Item | Verified | Not yet verified |
| --- | --- | --- |
| System | Windows 11 26200 Home | Windows 10 22H2 |
| Client | 3.1.42.205594 | Other versions. The log marks them "unverified"; if a client update changes its internal layout, the result is silence without an error |
| Device | HyperX Cloud III (44.1 / 48 / 96); AudioQuest DragonFly MM3A (88.2 as well) | Realtek onboard audio, HDMI output, 352.8 / 384 kHz, 5.1 / 7.1 multichannel |

## Known issues

- **Defender false positive** — see [Before you install](#before-you-install).
- **Software volume does not work in exclusive mode.** A WASAPI limitation. The system slider still works when the endpoint has a hardware volume node. The project deliberately does no digital attenuation in the module — no bits traded for volume.
- **Uninstalling may leave the exe behind.** The program is holding itself open, and without admin rights it cannot register a delete-on-reboot. Close it and delete the folder.
- **The settings panel still needs cross-version regression.** The current client has been checked for the separate module, aligned controls, editor save/cancel actions and the lossless specification badge; selectors still need confirmation on other client versions.
- **88.2 kHz sources** convert to 96 kHz on devices that only support 44.1 / 48 / 96. This is a bandwidth-preserving trade-off, at the cost of one non-integer-ratio resample.

A fuller list of limitations and plans is in [docs/ROADMAP.md](docs/ROADMAP.md) (Chinese).

## Documentation

- [docs/BUILDING.md](docs/BUILDING.md) — build from source, run the regression tests, package a release
- [docs/OPENHOME_NETEASE_RESEARCH.md](docs/OPENHOME_NETEASE_RESEARCH.md) — OpenHome architecture and the NetEase local-database conclusion
- [docs/ROADMAP.md](docs/ROADMAP.md) — limitations and plans
- [docs/defender-false-positive.md](docs/defender-false-positive.md) — material for the Microsoft false-positive submission

## License

[MIT](LICENSE).
