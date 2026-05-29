/*
 * voice_ui — voice→render bridge for on-screen bubbles (design/27).
 *
 * The voice worker tasks (voice_peer, voice_tools) and the imagine
 * worker run off the main task, but only the main loop owns the
 * e-paper. This is a tiny thread-safe mailbox: the workers POST a
 * bubble (the model's spoken transcript, or a busy/imagining notice)
 * and the main loop TAKEs the latest and renders it over the pet.
 *
 * Latest-post-wins; one bubble at a time. Not a queue — e-paper can't
 * keep up with a stream anyway, so we only ever show the most recent
 * spoken line / current status. Bounded copy; safe from any task.
 */

#pragma once

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    VOICE_UI_NONE = 0,   /* no bubble */
    VOICE_UI_SPOKEN,     /* talk bubble — what mochi just said / a tool result */
    VOICE_UI_THINKING,   /* thought bubble — busy: tool in flight / imagining */
} voice_ui_kind_t;

/* Reset to empty. Call at session start + stop so a stale bubble from
 * a previous session never lingers. */
void voice_ui_reset(void);

/* Post a bubble. `text` is copied into an internal bounded buffer.
 * Latest post wins. Thread-safe; callable from any task. */
void voice_ui_post(voice_ui_kind_t kind, const char *text);

/* Clear the current bubble (kind → NONE). */
void voice_ui_clear(void);

/* Main-loop consumer: if the bubble changed since the last take, copy
 * its text into `out` (bounded by `cap`), set *kind, and return true.
 * Returns false when nothing changed. */
bool voice_ui_take(char *out, size_t cap, voice_ui_kind_t *kind);

#ifdef __cplusplus
}
#endif
