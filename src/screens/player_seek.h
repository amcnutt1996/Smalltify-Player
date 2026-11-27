/* player_seek.h: the player's seek bar: where the track is, dragged to seek.
 *
 * A Raven Slider (comp_slider) takes the pointer, but it is only painted
 * while the user holds it. The rest of the time a custom widget under it
 * paints the same track, fill and knob at the position given, without the
 * slider's spring: moving once per second of playback then costs one frame,
 * where the spring of a slider moved by Rslider_set_value costs about 15.
 *
 * Owner: the Player module. UI thread only; one seek bar per process. */
#ifndef SMP_SCREENS_PLAYER_SEEK_H
#define SMP_SCREENS_PLAYER_SEEK_H

#include "gui/components.h"

/* Builds it into parent and returns it. on_change follows a drag with the
 * value (0..1); on_commit runs once it is let go, or for each step a
 * screen reader asks for: that is when the seek is sent. */
Rid seek_build(Rid parent, CompSliderFn on_change, CompSliderFn on_commit, void *ud);

/* Where the knob is (0..1). Ignored while the user holds it; paints only
 * when it changes. */
void seek_set(float value);
void seek_set_enabled(int enabled);

/* 1 from the press until the release. */
int seek_held(void);

/* How far the track starts in from each end of the bar: half the knob,
 * which grows with the text size. For lining the times up with the
 * track's ends. */
float seek_inset(void);

/* Once per update pass: a slider that was painted while held is replaced
 * by a fresh one, so the next press starts its knob where it is put
 * rather than where the last drag left it. */
void seek_update(void);

#endif /* SMP_SCREENS_PLAYER_SEEK_H */
