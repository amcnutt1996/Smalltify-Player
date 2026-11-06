# Decisions

How each choice was made: **User** means you decided it in chat, **Orch** means the orchestrator decided, and **Agent** means a build agent decided it and the orchestrator accepted. Every decision after you stepped away is marked *(autonomous)*.

| # | Decision | By | Why |
|---|---|---|---|
| 1 | GUI: raylib 5.5 in event-waiting mode, with no raygui | User | One C library on every OS. With event waiting the window sleeps until there is input; background threads wake it with `glfwPostEmptyEvent()`. |
| 2 | ~~librespot ships next to the binary~~ → **superseded by #18**. The engine lives in `<app>/soloist/` inside the portable folder | User | You wanted no files written outside the app folder. |
| 3 | All runtime data in `<app>/data/`; nothing in the cache dir, AppData, $HOME or /tmp; album art kept in memory only | User | Portability; no leftovers on the machine. |
| 4 | ~~librespot 0.8.0 built with cargo~~ → **removed by #18** | User + Agent | It worked, but Spotify refuses its audio keys on your account (#9). |
| 5 | Portable C11 with POSIX + Win32 in one `platform.c`; verified only on macOS arm64 | User | Only a Mac is available here. The Win32 code has not been compiled. |
| 6 | Sign-in: Authorization Code + **PKCE**. `SPOTIFY_CLIENT_SECRET` is ignored and never read | Orch | A secret in a folder you hand out isn't secret. Spotify recommends PKCE for desktop apps. |
| 7 | `dist/.env` is seeded from `.env.example` plus **only** the `SPOTIFY_CLIENT_ID=` line of the repo `.env` | Orch | Keeps the secret out of build output. |
| 8 | ~~Two sign-ins (app + librespot)~~ → **superseded by #18**: one app sign-in, plus a Soloist key and a one-time pairing | Orch | Since Aug 2026 Spotify's login5 rejects third-party tokens for librespot. |
| 9 | ~~Hybrid librespot + other devices~~ → **superseded by #18**. The device picker stays as a feature | User | Your account got `error audio key 0 1`, a permanent Spotify refusal with no client-side fix (librespot#1649). |
| 10 | Polling: the UI ticks at 1 Hz from local interpolation; the Web API is fetched every 5 s while playing, at the predicted end of each track, and immediately after your own commands. Idle and background states poll every 5 s; errors back off 2→30 s; no redraw when nothing changed | Orch (+ PerfReviewer, *autonomous*) | Battery life. This cut requests from 60/min to about 12/min while playing. A pause made on another device shows up within 5 s. |
| 11 | One curl handle per thread, reused: keep-alive, reused TLS sessions, HTTP/2, gzip | Orch | No TLS handshake per poll. |
| 12 | GUI commands go through one worker thread with a single-slot mailbox (latest command wins) and optimistic UI | Agent | The UI never blocks; rapid clicks collapse into one command. |
| 13 | `transfer` keeps the current play/pause state (`play=false`); the GUI picker transfers with `play=true` | Orch *(autonomous)* | The CLI matches Spotify's default behaviour. In the GUI, choosing a device means you want to hear it. |
| 14 | Tests: plain `assert` + ctest, offline fixtures only; `-UNDEBUG` on test targets | Orch | asserts would otherwise compile out under MinSizeRel. |
| 15 | raylib built with `CUSTOMIZE_BUILD` + `SUPPORT_FILEFORMAT_JPG` | Agent | Spotify album art is JPEG, and raylib 5.5 leaves JPEG decoding off by default. |
| 16 | Your uncommitted `.env.example` edit (empty `SPOTIFY_CLIENT_SECRET=`) left untouched | Orch | It's your change. The app ignores the key. |
| 17 | `dist/.env` carries the client ID for dev builds | Orch *(autonomous)* | A PKCE client ID is public, not a secret. Before you distribute a folder, decide whether to ship your ID or leave it blank so users fill in their own. |
| 18 | **Spotify Soloist replaces librespot** as the local "this computer" Connect device | User (+ Orch for the details) | It is Spotify's official headless client (Aug 2026; macOS/Windows since Oct 2026) and uses the same engine as the Spotify apps, so it gets no audio-key refusals. You asked for it. Clean cutover: librespot code, the cargo build and the second sign-in are removed. |
| 19 | Soloist is **downloaded at runtime** from `soloist-builds.spotifycdn.com` into `<app>/soloist/`, not bundled | Orch *(autonomous)* | Spotify's docs forbid redistributing Soloist binaries. Builds expire 90 days after their build date (exit code 10), so the app re-downloads when a build is missing, expired, or expires within 7 days. Extraction uses the OS `tar` (built into macOS, Linux, and Windows 10 1803+). |
| 20 | Soloist key read from `.env` as `SOLOIST_API_KEY=`; data and cache go to `<app>/data/soloist{,-cache}` with a 100 MB cache cap (the minimum Soloist allows) | Orch *(autonomous)* | Same place as the client ID. The cap keeps the folder small. The key is passed to Soloist only as an argument (Spotify's documented limit) and is never logged. |
| 21 | Pairing is a one-time GUI step: "open Spotify on any device on this network and pick *Spotify C Player*"; the app waits until the device appears in the Web API device list | Orch *(autonomous)* | Soloist authenticates only through Spotify Connect discovery. There's no API for it. |
