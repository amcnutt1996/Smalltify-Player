/* Album art: download on a worker thread when the track changes; keep only
 * the current image in memory (no disk cache -- nothing outside app dir,
 * and nothing persistent). Decoding to a texture happens in ui.c. */
#ifndef ART_H
#define ART_H

#include <stddef.h>
#include <stdint.h>

typedef struct ArtLoader ArtLoader;

ArtLoader *art_start(void (*on_ready)(void *ud), void *ud);
/* Request url (no-op if same as current/pending). "" clears. */
void       art_request(ArtLoader *a, const char *url);
/* If new bytes arrived since *seq: copies pointer to an internal buffer that
 * stays valid until the next art_take call, returns 1. */
int        art_take(ArtLoader *a, const unsigned char **data, size_t *len, uint64_t *seq);
/* Ask the thread to stop without waiting (art_stop still joins). NULL-safe. */
void       art_signal_stop(ArtLoader *a);
void       art_stop(ArtLoader *a);

#endif
