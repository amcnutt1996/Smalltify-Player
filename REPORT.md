# Spotify C Player: build report (2026-11-06)

## Status
**Built, reviewed, optimized and committed.** Two steps need you in person:
1. Create a **Soloist API key** at https://developer.spotify.com/dashboard/soloist. Paste it after `SOLOIST_API_KEY=` in `build/live/dist/.env`. The line is already there and empty; there's a matching one in the repo `.env`.
2. Run `build/live/dist/spotify-c-player`. It downloads Soloist from Spotify (~11 MB, once). When it shows **"Pair once"**, open Spotify on your phone or computer on the same Wi-Fi, open the device list, and pick **"Spotify C Player"**. From then on, the app plays on this desktop as its own device and opens straight to the player.

`build/live/dist/` is your ready-to-run folder, and you're already signed in there. `build/dist/` is a fresh build with no sign-in. Any folder can be moved anywhere; delete it to uninstall.

## What was built
| # | Feature (one subagent each) | Commits |
|---|---|---|
| — | Scaffold: module headers as the contract, CMake, cJSON vendored | `735e25b`, `cc7f7ce` |
| 1 | Auth CLI wrapper: PKCE sign-in, loopback redirect, thread-safe token store, platform layer (POSIX + Win32), `login`/`logout`/`engine` | `1cc4f29`, `fa96ce6`, `1a22e9b` |
| 2 | HTTP wrapper: libcurl, keep-alive, 401 refresh, 429 retry | `ca486a8` |
| 3 | `now` CLI + Web API state parser | `32b30c6`, `68a06ed` |
| 4 | Playback commands: play/pause/toggle/next/prev/seek/vol/shuffle/repeat/devices/transfer | `54059b3`, `c90a11b` |
| 5 | GUI shell (raylib, event-driven), device picker | `c04ac05`, `7140b77` |
| 6 | Poller thread | `ec0c176` |
| 7 | Album art + extras: playlists and search (CLI + GUI Library panel) | `f8cf3fd`, `31b912b`, `7367cf7` |
| — | Council fixes (13 review findings, 5 test findings) | `2b751f7`, `a9ca7b0`, `632fae6`, `43490a2`, `c0e5be6` |
| — | Post-build optimization | `1fc5200`, `84fccf1`, `0b7e812` |
| — | **Spotify Soloist cutover** (replaces librespot) | `8c1d561`, `df97069`, `ed30188` |
| — | Docs: README, DECISIONS | `9fdceeb`, `6b0b308`, plus this commit |

The code is C11, about 6.5k lines in `src/`; the largest files are `ui.c` and `platform.c`. Every OS-specific call is in `platform.c`.

## Why Soloist instead of librespot
- **Clean build:** no warnings from `src/` or `tests/`; **ctest 5/5** on the final tree. The ASan + UBSan builds of all tests were clean at the council stage, before the Soloist cutover; they weren't re-run afterwards.

## Verification (observed)
- **Clean build:** no warnings from `src/` or `tests/`; **ctest 5/5**; ASan + UBSan builds of all tests are clean.
- **Real account, read-only:** `login` (PKCE in your browser) → `now` / `devices` / `playlists` / `search` all worked. The user-approved librespot run reached playback before Spotify refused the key.
- **Soloist:** downloaded from Spotify's CDN into a scratch app folder; `--version` ran; the archive was deleted; `engine --update` re-downloaded. With a dummy key it starts and asks for pairing; EOF or Ctrl-C leaves no processes.
- **GUI flow,** captured in screenshots inside the app: key screen → "Downloading Spotify Soloist… N%" → "Pair once" → player with art, controls, device picker and Library panel.
- **Nothing outside the app folder:** with HOME and TMPDIR pointed at empty dirs, both stayed empty. `~/Library/Caches` and `~/Library/Application Support` were unchanged.
- **Shutdown:** SIGTERM, SIGHUP, SIGINT, Cmd-Q and SIGTERM while minimized each exit cleanly, leaving no engine. If the app is killed (`kill -9`), the watchdog removes the engine.

## Resource use (macOS arm64, measured)
| Metric | Value |
|---|---|
| App binary | 600 KB (down from 926 KB) |
| Idle CPU, sign-in or pairing screen | ~0% (CPU time flat over 20 s) |
| Player CPU, playing, foreground | ~0.4% |
| Web API requests while playing | ~12/min, down from 60/min |
| Requests when minimized or paused | 12/min |
| App RSS | ~95 MB, mostly macOS's OpenGL/window buffers |

## Known limits and risks
- **Linux and Windows code was never compiled or run here** (Mac only): the Linux `fork`/`PR_SET_PDEATHSIG` spawn and the whole Win32 path. A Linux build and a Windows build are the next checks.
- Soloist has no Intel Mac or Windows-on-ARM builds; on those platforms the app works only as a remote control.
- Soloist receives its key as a command-line argument (Spotify's limitation), so other local processes can see it. Soloist also runs Spotify's own crash reporter.
- The GUI crashes if launched while the display is asleep. This is a raylib limitation.
- Pairing needs local-network discovery. A VPN, guest Wi-Fi or client isolation can hide the device.

## Housekeeping
- **Cleanup:** all dev services, agents, scratch folders and worktrees are stopped or deleted. No `spotify-c-player`, `soloist` or `librespot` processes are running.
- **Secrets:** the client secret in your `.env` is never read and never copied into build output. The repo `.env` is gitignored.
