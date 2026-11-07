# Smalltify Player

A small, low-power Spotify player and remote for the desktop, written in plain C11. It has a native GUI (raylib) and a command-line interface in one binary. It plays music on your computer through [Spotify Soloist](https://developer.spotify.com/documentation/soloist), Spotify's official headless Spotify Connect client, and it can also control any other Spotify Connect device on your account (phone, speaker, TV).

> **Platform status:** Smalltify Player has only been tested on **macOS** (13 Ventura or newer, Apple silicon). The code has Windows and Linux paths, but they are untested.

## What it's for
The official Spotify desktop app is a large Chromium-based app that uses a lot of memory and battery. Smalltify Player covers everyday listening with a fraction of the footprint:

- **Lightweight:** one native executable, event-driven UI (about 0% CPU when idle), no web runtime.
- **Plays locally:** streams on this computer through Spotify Soloist once it is paired.
- **Remote control:** play, pause, skip, seek, volume, shuffle, repeat and device transfer for any Spotify Connect device.
- **Library:** Liked Songs, playlists, albums, artists, podcasts, episodes, recently played, the queue and search.
- **Scriptable:** every action is also a CLI command (`smalltify-player now`, `next`, `vol +10`, ...).
- **Self-contained:** everything the app stores lives inside its own folder or app bundle. Delete it to uninstall.

Playback control needs a **Spotify Premium** account.

## First-time setup (macOS)

### 1. Prerequisites
- macOS 13 or newer. Local playback needs Apple silicon, because Spotify publishes no Soloist build for Intel Macs. On an Intel Mac the app works as a remote only.
- Xcode Command Line Tools (clang, `codesign`, libcurl): `xcode-select --install`
- CMake 3.20 or newer: `brew install cmake`
- An internet connection for the first build, because CMake downloads raylib 5.5. cJSON and the Inter font are vendored in `third_party/`.

### 2. Create a Spotify developer app
1. Go to https://developer.spotify.com/dashboard and create an app.
2. Add the redirect URI `http://127.0.0.1:8888/callback`.
3. If the app is in Development mode, add your Spotify account under **User Management**.
4. Copy the app's **Client ID**. You don't need a client secret: sign-in uses PKCE.

### 3. Get a Soloist API key (for playback on this Mac)
Generate a key at https://developer.spotify.com/dashboard/soloist. Keep it secret; the app never logs it. You can skip this step. Without a key the app still controls your other devices, and you can paste a key later (Devices → "Enable playback on this computer").

### 4. Configure `.env`
```sh
git clone https://github.com/amcnutt1996/Smalltify-Player.git
cd Smalltify-Player
cp .env.example .env
```
Edit `.env` and fill in `SPOTIFY_CLIENT_ID=` and, if you have one, `SOLOIST_API_KEY=`. `.env` is git-ignored. The build copies only these two lines into the app bundle and never copies a client secret.

### 5. Build and test
```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build
```
This produces `build/dist/Smalltify Player.app`, signed ad hoc.

### 6. Install
Drag `build/dist/Smalltify Player.app` to `/Applications` and open it from there, Launchpad or the Dock. Always open the `.app`, because double-clicking the bare `smalltify-player` file inside it opens Terminal.

### 7. First launch
1. Click **Sign in with Spotify** and approve the request in your browser. The player opens right away.
2. When macOS asks whether "Smalltify Player" may find devices on your local network, click **Allow**. Otherwise Spotify Connect can't see the player.
3. **Pair once.** Spotify lets Soloist sign in only after an official Spotify app on the same network picks it once. Open Spotify on your phone. The app downloads Soloist from Spotify into its folder (Soloist can't be bundled) and starts it. It then shows a tip naming this Mac's device, "Smalltify Player (<Mac name>)". Choose that device once in the phone's Spotify device list. If no other device of your account shows up for 10 minutes, the unpaired Soloist stops again to save battery.
4. Done. The pairing is saved inside the app. From now on, Play streams on this Mac after every restart, with no phone needed. Restarts, rebuilds and Soloist updates keep the pairing; only **Log out** or deleting the app clears it.

## Where the app keeps its files
Everything lives in one folder, and the app writes nothing outside it (no cache directory, AppData or registry). On macOS that folder is `Smalltify Player.app/Contents/Resources`:

```
smalltify-player[.exe]   the app (GUI with no args, CLI with a command)
.env                     SPOTIFY_CLIENT_ID=..., SOLOIST_API_KEY=..., UI_FONT_SIZE=... (Settings)
Inter-LICENSE.txt        license of the embedded UI font (Inter, SIL OFL 1.1)
soloist/                 Spotify Soloist, downloaded from Spotify on first use
data/token.json          your sign-in and the permissions it was granted (owner-only permissions)
data/soloist/            Soloist's device id and paired session
data/soloist-paired      marker: this computer was paired (with data/device-name, the name it was paired under)
data/soloist-cache/      Soloist's playback cache (capped at 100 MB)
data/soloist-home/       Soloist's HOME/TMPDIR, so its OS caches stay here too
```

Drag the app to the Trash to uninstall everything. If the bundle's `.env` has no Soloist key, paste one on the key screen and press **Save**, which writes it to the bundle's `.env`.

- The app writes into its own bundle, so keep it in a folder you can write to (`/Applications` works for admin accounts). If it runs from a read-only place (a disk image, or a downloaded copy that macOS runs from a temporary location), it asks you to move it first.
- It is ad-hoc signed, not notarized. After the first run, `codesign --verify` reports the bundle as modified because of the files in `Contents/Resources`. That's expected.
- CLI: `"/Applications/Smalltify Player.app/Contents/MacOS/smalltify-player" now`, etc.

## Using the player
The bottom bar has **Devices**, **Library**, **Settings** and **Log out**.

- **Settings:** text size (Small, Default, Large, Extra large). A pick applies at once and is saved as `UI_FONT_SIZE` in the app's `.env`. Text uses the embedded Inter font (Latin, Greek, Cyrillic, Vietnamese; Chinese, Japanese, Korean and emoji show as `?`).
- **Library:** a stack of screens. **Back** or Esc goes up one level; Esc at the top closes the panel, and reopening it returns to where you were.
  - Top level: a search field, then **Liked Songs** (with your total), **Playlists**, **Albums**, **Artists**, **Podcasts**, **Your Episodes**, **Recently played** and **Queue**.
  - Search (Enter) shows up to 10 songs, artists, albums, playlists, podcasts and episodes under headings. **See all** lists more of one kind.
  - Playlists, albums, artists (their albums) and podcasts (their episodes) open in their own screen, with **Play** in the header. Clicking a song plays it inside its playlist, album, podcast or Liked Songs, so the next songs follow. Songs in search, Recently played, Queue and Your Episodes play on their own.
  - **+Queue** on a song or episode adds it to the queue. Lists load more as you scroll, up to 500 entries. Greyed-out rows are unavailable (local files, or not available in your country).
  - Spotify only shows the songs of playlists you own or collaborate on. Playlists you follow still play with **Play**.
- **Heart** at the end of the title line: shows whether the current song is in Liked Songs; click it to like or unlike.

Not available, because Spotify's Web API doesn't offer them: Home, Browse, Made For You, recommendations, an artist's top songs, lyrics, Jams and friend activity.

Sign-ins from before this version are missing four permissions (liking and saving, following artists, followed artists, recently played). The app asks once per launch to sign in again; **Later** keeps the player running, and Artists and Recently played show a **Sign in again** button until you do.

Pressing Play plays on the active device, else this computer (once paired), else any other available device of your account ("Playing on iPhone"). With nothing available the status line says how to get a device.

## Soloist updates
Soloist builds expire 90 days after their build date. The app checks the build at every start and downloads a new one when it's missing or expired, and when it expires within 7 days at most once a day (Spotify may still serve the same build; the check time is kept in `data/soloist-check`). A download is unpacked into `soloist.new/`, run once to check it, and only then swapped in, so a bad download never breaks a working build. A build expiring while the app runs is replaced once and restarted; if Soloist stops otherwise, the player shows **Restart Soloist** (which re-reads the key from `.env`) and **Change key**. `smalltify-player engine --update` forces a fresh download. Spotify publishes builds for macOS on Apple silicon, Windows x64, and Linux (x86_64, arm64, arm32), but not for Intel Macs or Windows on ARM. There the app controls other devices only.

## CLI
```
smalltify-player login | logout | engine [--update]
smalltify-player now
smalltify-player play | pause | toggle | next | prev
smalltify-player seek <seconds | m:ss>     vol <0-100 | +N | -N>
smalltify-player shuffle on|off            repeat off|context|track
smalltify-player devices                   transfer <name-or-id>
smalltify-player playlists                 playlist <number | name>
smalltify-player search [-p N] [--type track,album,artist,playlist,show,episode | all] <query...>
smalltify-player list [--all] liked|playlists|albums|artists|shows|episodes|recent|queue
smalltify-player list [--all] playlist|album|artist|show <id or uri>
smalltify-player like | unlike | saved <uri...>
smalltify-player queue <uri>               me
```
`engine` runs Soloist in the foreground and installs or updates it first if needed. It prints the pairing hint on first use, and Ctrl-C stops it (EOF too, when stdin is a terminal, so `nohup`/launchd work). The other commands need a device: `engine`, the GUI, or any Spotify app. With nothing active, `play`, `playlist` and `search -p` start on this computer's engine if it is online, else on any available device.

`list` prints one page (`--all` follows every page) as `N. name - sub (length)  uri`; `search` prints the same format. `like`/`unlike` save to or remove from your library (songs, albums, shows, episodes, artists), `saved` checks them, `queue` adds a song or episode to the queue, and `me` prints your account name and the Liked Songs URI.

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
| `src/ui.c` | raylib GUI, event-driven (≈0% CPU when idle); Library panel, Settings, embedded Inter font |
| `src/browse.c` | Pure Library-panel helpers: what a click plays, row texts, search grouping, list cap |
| `src/cli.c` | CLI commands |

## License
Smalltify Player is released under the [MIT License](LICENSE).

Third-party components keep their own licenses: cJSON (MIT, `third_party/cjson/LICENSE`), the Inter font (SIL OFL 1.1, `third_party/inter/LICENSE.txt`) and raylib (zlib, fetched at build time). Spotify Soloist is not included; the app downloads it from Spotify, and it is covered by Spotify's own terms.

Not affiliated with or endorsed by Spotify. Spotify is a trademark of Spotify AB.
