# Spotify C Player

A small, low-power Spotify player and remote written in C11. It has a raylib GUI and a CLI, and it bundles [librespot](https://github.com/librespot-org/librespot) 0.8.0 as its playback engine.

## Portable folder
Everything lives in one folder. The app writes nothing outside it: no cache directory, no AppData, no registry.

```
spotify-c-player[.exe]   the app (GUI with no args, CLI with a command)
librespot[.exe]          bundled playback engine (Spotify Connect device "Spotify C Player")
.env                     SPOTIFY_CLIENT_ID=...
data/token.json          your sign-in (owner-only permissions)
data/librespot/          the engine's one-time sign-in
```

Copy or move the folder anywhere. Delete it to uninstall.

## Setup
1. Create an app at https://developer.spotify.com/dashboard.
   - Add the redirect URI `http://127.0.0.1:8888/callback`.
   - If the app is in Development mode, add your account under User Management.
2. Put `SPOTIFY_CLIENT_ID=<your id>` in `.env` next to the binary. No client secret is needed or used: sign-in is PKCE.
3. Launch. The first launch asks for two sign-ins, and later launches skip both:
   1. **Sign in with Spotify**: your app. Now playing and the controls use it.
   2. **Connect playback engine**: librespot's own sign-in. Spotify won't accept third-party tokens for it.

Playback control needs Spotify Premium.

### Local playback vs. other devices
Spotify refuses librespot's audio keys on some accounts (`error audio key 0 1`), and there is no client-side fix. When that happens, the app says so and opens the device picker. You can then control the Spotify app on your phone, desktop or speaker instead.

## CLI
```
spotify-c-player login | logout | engine
spotify-c-player now
spotify-c-player play | pause | toggle | next | prev
spotify-c-player seek <seconds | m:ss>     vol <0-100 | +N | -N>
spotify-c-player shuffle on|off            repeat off|context|track
spotify-c-player devices                   transfer <name-or-id>
spotify-c-player playlists                 playlist <number | name>
spotify-c-player search [-p N] <query...>
```
The CLI needs an active device. Use `engine`, which runs librespot in the foreground, or any Spotify app.

## Build
You need CMake ≥ 3.20, a C11 compiler, libcurl (development files), and Rust/cargo (to build librespot).
```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```
The output goes to `build/dist/`. Use `-DSCP_BUILD_LIBRESPOT=OFF` to skip the cargo build. raylib 5.5 is fetched by CMake, and cJSON is vendored in `third_party/`.

## Layout
| File | Role |
|---|---|
| `src/platform.c` | All OS-specific code (POSIX + Win32): paths, threads, processes, sockets |
| `src/config.c` | `.env` and token files |
| `src/auth.c` | PKCE, loopback redirect listener, thread-safe token store |
| `src/http.c` | libcurl wrapper: keep-alive, bearer token, 401 refresh, 429 retry |
| `src/spotify.c` | Web API calls → `model.h` structs |
| `src/librespot.c` | Spawns and supervises the bundled engine |
| `src/poller.c` | Background playback-state polling (1 s while playing, 5 s idle) |
| `src/art.c` | Album art download (memory only) |
| `src/ui.c` | raylib GUI, event-driven (≈0% CPU when idle) |
| `src/cli.c` | CLI commands |
