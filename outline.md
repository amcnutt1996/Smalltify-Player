# Library
`Spotify Soloist` - Spotify's official headless Spotify Connect player (replaced `librespot`, see DECISIONS.md #18). It's downloaded at runtime into the app folder, because Spotify forbids redistributing it. The app spawns it, reads its output, and kills it when done.

# Build Order
1. Auth CLI Wrapper
2. HTTP Wrapper
3. CLI that prints 'now playing'
4. CLI playback commands
5. GUI shell
6. poller thread
7. art+Extras

# Files
`main.c` - Startup, wires everything together
`config.c + config.h` - client ID, file paths, load/save token file
`auth.c + auth.h` - PKCE generation, authorize URL, loopback listener, code-> token exchange, refresh logic
`http.c + http.h` - a thin libcurl wrapper. get/put/post with bearer token, response buffer + status code, 401-> refresh-> retry, 429 handling.
`spotify.c + spotify.h` - API functions like get_playback_state, play, pauyse, next, prev, seek, set_volume, get_devices, transfer, search, get_playlists, etc. Each parses JSON into plain C structures.
`model.h` - Structs: Track, PlaybackState, Device, Playlist
`poller.c + poller.h` - background thread: every ~1/2 seconds fetch playback state, hand results to UI safely.
`art.c + art.h` - (optional) download + decode + cache album art
`ui.c + ui.h` - window, widgets, button callbacks -> spotify.c calls
`soloist.c + soloist.h` - downloads and updates Soloist, spawns and supervises it, and tracks its pairing state
`platform.c + platform.h` - all OS-specific code (paths, threads, processes, sockets)
`cli.c + cli.h` - command-line front end


