# Spotify C Player

A small, low-power Spotify player and remote written in C11. It has a raylib GUI and a CLI. It plays on this computer through [Spotify Soloist](https://developer.spotify.com/documentation/soloist), Spotify's official headless Connect client, and can control any other Spotify Connect device too.

## Portable folder
Everything lives in one folder. The app writes nothing outside it: no cache directory, no AppData, no registry.

```
spotify-c-player[.exe]   the app (GUI with no args, CLI with a command)
.env                     SPOTIFY_CLIENT_ID=... and SOLOIST_API_KEY=...
soloist/                 Spotify Soloist, downloaded from Spotify on first use
data/token.json          your sign-in (owner-only permissions)
data/soloist/            Soloist's device id and paired session
data/soloist-cache/      Soloist's playback cache (capped at 100 MB)
```

Copy or move the folder anywhere. Delete it to uninstall.

## Setup
1. Create an app at https://developer.spotify.com/dashboard.
   - Add the redirect URI `http://127.0.0.1:8888/callback`.
   - If the app is in Development mode, add your account under User Management.
2. Put `SPOTIFY_CLIENT_ID=<your id>` in `.env` next to the binary. No client secret is needed or used: sign-in is PKCE.
3. Generate a Soloist API key at https://developer.spotify.com/dashboard/soloist and add `SOLOIST_API_KEY=<your key>` to `.env`. Without a key the app can still control your other devices. Keep the key secret: the app never logs it.
4. Launch. The first launch walks you through:
   1. **Sign in with Spotify** in the browser.
   2. **Download**: the app fetches Soloist from Spotify's servers into `soloist/`. Spotify doesn't allow redistributing it, so it isn't bundled.
   3. **Pair once**: open Spotify on your phone or computer (same network), open the device list and choose "Spotify C Player". The session is kept in `data/soloist/`.

   Later launches skip all three. Logging out also removes the pairing.

Playback control needs Spotify Premium.

### Soloist updates
Soloist builds expire 90 days after their build date. The app checks the build at every start and downloads a new one when it's missing or expires within 7 days. `spotify-c-player engine --update` forces a fresh download. Spotify publishes builds for macOS on Apple silicon, Windows x64, and Linux (x86_64, arm64, arm32), but not for Intel Macs or Windows on ARM. There the app controls other devices only.

## CLI
```
spotify-c-player login | logout | engine [--update]
spotify-c-player now
spotify-c-player play | pause | toggle | next | prev
spotify-c-player seek <seconds | m:ss>     vol <0-100 | +N | -N>
spotify-c-player shuffle on|off            repeat off|context|track
spotify-c-player devices                   transfer <name-or-id>
spotify-c-player playlists                 playlist <number | name>
spotify-c-player search [-p N] <query...>
```
`engine` runs Soloist in the foreground and installs or updates it first if needed. It prints the pairing hint on first use, and Ctrl-C or EOF stops it. The other commands need an active device: `engine`, the GUI, or any Spotify app. With nothing active, `play`, `playlist` and `search -p` start on "Spotify C Player".

## Build
You need CMake ≥ 3.20, a C11 compiler, and libcurl (development files).
```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```
The output goes to `build/dist/`. raylib 5.5 is fetched by CMake, and cJSON is vendored in `third_party/`. At runtime, extracting Soloist uses the OS's own `tar` (built into macOS, Linux, and Windows 10 1803+).

## Layout
| File | Role |
|---|---|
| `src/platform.c` | All OS-specific code (POSIX + Win32): paths, threads, processes, sockets, tar |
| `src/config.c` | `.env` and token files |
| `src/auth.c` | PKCE, loopback redirect listener, thread-safe token store |
| `src/http.c` | libcurl wrapper: keep-alive, bearer token, 401 refresh, 429 retry, download progress |
| `src/spotify.c` | Web API calls → `model.h` structs |
| `src/soloist.c` | Downloads, updates, and supervises Spotify Soloist |
| `src/poller.c` | Background playback-state polling: fetches every 5 s and at each track's end, UI ticks at 1 Hz locally |
| `src/art.c` | Album art download (memory only) |
| `src/ui.c` | raylib GUI, event-driven (≈0% CPU when idle) |
| `src/cli.c` | CLI commands |
