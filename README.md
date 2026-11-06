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
data/soloist-paired      marker: this computer was paired (with data/device-name, the name it was paired under)
data/soloist-cache/      Soloist's playback cache (capped at 100 MB)
data/soloist-home/       Soloist's HOME/TMPDIR, so its OS caches stay here too
```

Copy or move the folder anywhere. Delete it to uninstall.

## Install on macOS
**Ready to install:** `build/dist/Spotify C Player.app` (built 2026-11-06 with your client ID and Soloist key; signed ad hoc, `codesign --verify` passes).

First run, once:
1. Drag `build/dist/Spotify C Player.app` to `/Applications` and open it.
2. Click **Sign in with Spotify** and approve the request in your browser.
3. When macOS asks whether the app may find devices on your local network, click **Allow**.
4. With Spotify open on your phone, the app shows a tip naming this Mac's device ("Spotify C Player (<Mac name>)"). Choose that device once in the phone's Spotify device list.
5. Done. The pairing is saved inside the app. From now on Play streams on this Mac after every restart, with no phone needed. Only **Log out** or deleting the app clears it.

The macOS build is an app bundle: `build/dist/Spotify C Player.app` (macOS 13 or newer, Apple silicon for local playback). Drag it to `/Applications` and open it from there, Launchpad or the Dock. Always open the `.app`: double-clicking the bare `spotify-c-player` file inside it opens Terminal.

Inside the bundle, the app folder above is `Spotify C Player.app/Contents/Resources` (`.env`, `soloist/`, `data/`). Drag the app to the Trash to uninstall everything. The build seeds that `.env` with the `SPOTIFY_CLIENT_ID` and `SOLOIST_API_KEY` lines of the repo `.env` (never the client secret). Without a key, paste it on the key screen and press **Save**, which writes it to the bundle's `.env`.

- The app writes into its own bundle, so keep it in a folder you can write to (`/Applications` works for admin accounts). From a read-only place (a disk image, or a downloaded copy macOS runs from a temporary location), it asks you to move it first.
- It is ad-hoc signed, not notarized. After the first run, `codesign --verify` reports the bundle as modified because of the files in `Contents/Resources`. That's expected.
- On first pairing, macOS asks to let "Spotify C Player" find devices on your local network. Allow it, or Spotify Connect can't see the player.
- CLI: `"/Applications/Spotify C Player.app/Contents/MacOS/spotify-c-player" now`, etc.

## Setup
1. Create an app at https://developer.spotify.com/dashboard.
   - Add the redirect URI `http://127.0.0.1:8888/callback`.
   - If the app is in Development mode, add your account under User Management.
2. Put `SPOTIFY_CLIENT_ID=<your id>` in `.env` next to the binary. No client secret is needed or used: sign-in is PKCE.
3. Generate a Soloist API key at https://developer.spotify.com/dashboard/soloist and add `SOLOIST_API_KEY=<your key>` to `.env`. Without a key the app can still control your other devices (Devices → "Enable playback on this computer" opens the key screen). Keep the key secret: the app never logs it.
4. Launch and **sign in with Spotify** in the browser. The player opens right away.
5. **Pair once.** This computer's device is "Spotify C Player (<hostname>)". Spotify lets Soloist sign in only when an official Spotify app on the same network picks it, once. When another device of your account is online (say your iPhone), the app quietly fetches Soloist from Spotify into `soloist/` (it can't be bundled), starts it so it appears in that device's Spotify device list, and shows one dismissible tip: "in Spotify on iPhone, choose 'Spotify C Player (…)'". Pick it there once. With no other device seen for 10 minutes, the unpaired Soloist stops again to save battery.

   After pairing, Soloist starts with the app every time, under the same name and `data/soloist/`, and library picks and Play play on this computer. Restarts, rebuilds and Soloist updates keep the pairing; only **Log out** removes it.

Pressing Play plays on the active device, else this computer (once paired), else any other available device of your account ("Playing on iPhone"). With nothing available the status line says how to get a device.

Playback control needs Spotify Premium.

### Soloist updates
Soloist builds expire 90 days after their build date. The app checks the build at every start and downloads a new one when it's missing or expired, and when it expires within 7 days at most once a day (Spotify may still serve the same build; the check time is kept in `data/soloist-check`). A download is unpacked into `soloist.new/`, run once to check it, and only then swapped in, so a bad download never breaks a working build. A build expiring while the app runs is replaced once and restarted; if Soloist stops otherwise, the player shows **Restart Soloist** (which re-reads the key from `.env`) and **Change key**. `spotify-c-player engine --update` forces a fresh download. Spotify publishes builds for macOS on Apple silicon, Windows x64, and Linux (x86_64, arm64, arm32), but not for Intel Macs or Windows on ARM. There the app controls other devices only.

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
`engine` runs Soloist in the foreground and installs or updates it first if needed. It prints the pairing hint on first use, and Ctrl-C stops it (EOF too, when stdin is a terminal, so `nohup`/launchd work). The other commands need a device: `engine`, the GUI, or any Spotify app. With nothing active, `play`, `playlist` and `search -p` start on this computer's engine if it is online, else on any available device.

## Build
You need CMake ≥ 3.20, a C11 compiler, and libcurl (development files).
```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```
The output goes to `build/dist/`. raylib 5.5 is fetched by CMake, and cJSON is vendored in `third_party/`. At runtime, extracting Soloist uses the system `tar` (part of macOS and Windows 10 1803+; on Linux `/usr/bin/tar` or `/bin/tar`).

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
