# Smalltify Player: build report (2026-11-06)

## Status
**Renamed to Smalltify Player and rebuilt from a clean build folder.** The installable app is **`build/dist/Smalltify Player.app`** (836 KB, macOS 13+, ad-hoc signed, `codesign --verify` passes, empty `data/`, bundle id `com.smalltifyplayer.app`, binary `smalltify-player`). Your client ID and Soloist key are already in it. The old `Spotify C Player.app` was removed from `/Applications` together with its system leftovers (see Housekeeping), so this is a fresh install: drag the app to /Applications → sign in with your browser → allow Local Network (asked again because the bundle id changed) → pick "Smalltify Player (<Mac name>)" once in Spotify on your phone.

Not verified on real hardware: the minimized pairing poll switching to 15 s, a clean long idle-CPU measurement on the player screen, and the Linux/Windows code paths (never compiled here).

## Update: readable text, Settings, full library (2026-11-06, while you were away)
- **Text:** the embedded Inter font replaces the old raylib build's 10 px bitmap font; default body text is 14 px, and all layout follows the text size. **Settings** (next to Log out) offers Small / Default / Large / Extra large, applies at once, and saves `UI_FONT_SIZE` in `.env`.
- **Library:** Liked Songs (with your total), Playlists, Albums, Artists, Podcasts, Your Episodes, Recently played and Queue; search across songs, artists, albums, playlists, podcasts and episodes; drill into playlists, albums, artists and shows; play a song inside its playlist, album or Liked Songs; +Queue; a heart for liking the current song. README → *Using the player* has the details.
- **Spotify's Feb 2026 API limits** shape what's possible: search returns at most 10 per kind (the old GUI search asked for 20, which Spotify now refuses), followed playlists list no songs (they still play), and Home, Browse, recommendations, artist top songs, lyrics and Jams have no API.
- **CLI:** `list`, `search --type`, `like`/`unlike`/`saved`, `queue`, `me`.
- **Verified:** clean build, ctest 6/6; read-only calls on your account (Liked 2,434 songs, 19 playlists, 3 albums, 12 shows, own playlist contents, search); every Library screen rendered with your real data at Default and Extra large (in an instrumented copy, because the locked screen blocked real clicks); the release build launching to the new-permissions screen; idle CPU ~0.02% with the Library open. A code review found no blockers; its two small fixes (Settings redraw, queue adds under a full command queue) are in.
- **Not verified, by design (they would have changed your account):** pressing play from a Library row, Play on a playlist/album/Liked Songs, +Queue, liking/unliking, and signing in again. Playing a podcast episode inside its show is the least certain of these.
- Commits: `f6f077a`…`36fb51e` (API, auth, CLI), `d6ff8bb`…`2eb0ddd` (font, Settings), `e342137`, `7ad8dca` (Library UI, cleanup), plus the review fixes and docs. Decisions #12 and #40–#46 in DECISIONS.md.

## What was built
| # | Feature (one subagent each) | Commits |
|---|---|---|
| — | Scaffold: module headers as the contract, CMake, cJSON vendored | `735e25b`, `cc7f7ce` |
| 1 | Auth CLI wrapper: PKCE sign-in, loopback redirect, thread-safe token store, platform layer (POSIX + Win32), `login`/`logout`/`engine` | `1cc4f29`, `fa96ce6`, `1a22e9b` |
| 2 | HTTP wrapper: libcurl, keep-alive, 401 refresh, 429 retry | `ca486a8` |
| 3 | `now` CLI + Web API state parser | `32b30c6`, `68a06ed` |
| 4 | Playback commands: play/pause/toggle/next/prev/seek/vol/shuffle/repeat/devices/transfer | `54059b3`, `c90a11b` |
| 5 | GUI shell (Raven, event-driven), device picker | `c04ac05`, `7140b77` |
| 6 | Poller thread | `ec0c176` |
| 7 | Album art + extras: playlists and search (CLI + GUI Library panel) | `f8cf3fd`, `31b912b`, `7367cf7` |
| — | Council fixes (13 review findings, 5 test findings) | `2b751f7`, `a9ca7b0`, `632fae6`, `43490a2`, `c0e5be6` |
| — | Post-build optimization | `1fc5200`, `84fccf1`, `0b7e812` |
| — | **Spotify Soloist cutover** (replaces librespot) | `8c1d561`, `df97069`, `ed30188` |
| — | Docs: README, DECISIONS | `9fdceeb`, `6b0b308`, plus this commit |

The code is C11, about 6.5k lines in `src/`; the largest files are `ui.c` and `platform.c`. Every OS-specific call is in `platform.c`.

## Why Soloist instead of librespot
You approved both sign-ins with your real account, and everything worked: now playing, devices, search, playlists, volume and pause. librespot also authenticated. But Spotify refuses librespot's track-decryption keys for your account (`error audio key 0 1`), so nothing plays. This is a permanent server-side refusal ([librespot#1649](https://github.com/librespot-org/librespot/issues/1649)), and no client-side fix exists. You asked for Soloist: Spotify's official headless Connect player (Aug 2026, desktop since Oct 2026). It uses the same engine as the Spotify apps. Spotify forbids redistributing it, so the app downloads it into its own folder and renews it before the 90-day expiry. Details are in DECISIONS.md #18–#21.

## Verification (observed)
- **Clean build:** no warnings from `src/` or `tests/`; **ctest 5/5** on the final tree. The ASan + UBSan builds of all tests were clean at the council stage, before the Soloist cutover; they weren't re-run afterwards.
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
- The GUI crashed if launched while the display was asleep in the old raylib build. Not re-tested on Raven.
- Pairing needs local-network discovery. A VPN, guest Wi-Fi or client isolation can hide the device.

## Housekeeping
- **Cleanup:** all dev services, agents, scratch folders and worktrees are stopped or deleted. No `smalltify-player`, `soloist` or `librespot` processes are running.
- **Secrets:** the client secret in your `.env` is never read and never copied into build output. The repo `.env` is gitignored.
- **Old name removed:** the `com.spotifycplayer.app` shader cache and the `soloist` CFNetwork cache in `DARWIN_USER_CACHE_DIR`, old crash reports, `/tmp` build scratch and 11 stale Launch Services registrations were deleted. One entry remains: macOS's Local Network record for `com.spotifycplayer.app` in the root-owned `/Library/Preferences/com.apple.networkextension.plist`. It is inert without the app; clear it in System Settings → Privacy & Security → Local Network if listed.
