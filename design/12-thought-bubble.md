# 12 — Thought-bubble subsystem (general-purpose progressive disclosure)

**Status:** M11.5a — first milestone (M1) landing 2026-05-20.
SLEEPY need only; framework extends to other needs and non-care
actions in later milestones.
**Predecessor:** [11-m11-pet-state-in-c.md](./11-m11-pet-state-in-c.md)
(substrate + mood projection in C).
**Sibling:** [06-scene-contracts.md](./06-scene-contracts.md) (server-
supplied scene-rendering contracts; thought bubbles are *not* server-
authored, see §generation below).
**Successor:** M2 — hungry + lonely; M3 — non-care affordances
(talk-seed, navigate, future custom dispatches).

## What this is

A progressive-disclosure UI primitive: when mochi's state warrants an
action the kid wouldn't otherwise reach, a small bubble appears above
the pet with a short label and a tap target. Tapping the bubble fires
the action; ignoring it leaves the bubble visible until the need
clears (e.g. mochi falls asleep on her own, gets fed via another
surface, the need-window passes).

The first concrete case: **the pet has no Sleep affordance.** Sleep is
only reachable today via the PWR long-press gesture, which is
invisible. When `energy < SLEEPY_THRESHOLD (28)` and the pet is awake,
the bubble surfaces a `tap to sleep` target.

The web mochi already has this — `c15r/mochi:shared/thoughts.ts` +
`frontend/components/ThoughtBubble.tsx`. M11 explicitly *dropped* the
`Pet.thoughts` field when porting to C, so the device went without.
This subsystem ports the *model*, not the implementation: thought
generation happens device-side over the C substrate, and rendering
fits the 200×200 1bpp e-paper budget.

## Why not "just add a Sleep button"

That was the obvious first instinct and Christopher specifically asked
us not to. Reasons:

1. The corner-icon layout is full (4 care actions: comfort, cheer,
   feed, play). Adding a 5th would either crowd or displace.
2. Sleep is not always a care need — it's the *consequence* of
   low energy. The right idiom is "mochi feels sleepy → invites the
   kid to settle her down," not "Sleep is always available."
3. The same problem will recur for any future situational action
   (talk-seed prompts, navigation invites, etc.). A general-purpose
   "thought bubble" surface absorbs them all.

The thought bubble is **deliberately tied to a need or invitation, not
a permanent control.** It comes and goes with mochi's interior life.

## Subsystem shape

Three pieces, mirrored loosely on the web side:

```
firmware/main/thought.h         types + API
firmware/main/thought.cpp       pure generation (predicate chain)
                                + bubble rendering (1bpp composite)
firmware/main/main.cpp          wiring: per-render compute + dispatch
```

### Types

```c
typedef enum {
    THOUGHT_ACTION_NONE = 0,
    THOUGHT_ACTION_CARE_EVENT,   /* event_kind_t flows to substrate    */
    THOUGHT_ACTION_TALK_SEED,    /* future: open voice with text seed  */
    THOUGHT_ACTION_NAVIGATE,     /* future: route to non-care surface  */
} thought_action_kind_t;

typedef struct {
    thought_action_kind_t action_kind;
    event_kind_t          action_event;   /* CARE_EVENT only           */
    const char           *line1;          /* top text, ≤ 11 chars      */
    const char           *line2;          /* sub-line hint, ≤ 11 chars */
    int64_t               expires_at_ms;  /* 0 = lifetime tied to need */
} pet_thought_t;
```

Future variants (talk-seed payload text, navigate target id) attach
through a union or a tagged-payload extension when M2/M3 lands.
M1 keeps it a flat struct.

### Generation

`thought_generate(pet, now_ms, out_thought)` runs a priority chain
over the decayed pet snapshot. M1 chain:

1. `pet.asleep` → no thought.
2. `pet.stats.energy < 28` → SLEEPY thought (`care_event = SLEPT`).

M2 chain extends:

3. `pet.stats.fullness < 35` → HUNGRY thought (`care_event = FED` with
   default `{ item: "kibble" }`).
4. `now - pet.last_interaction_at > LONELY_MS / age_mult` → LONELY
   thought (`talk_seed`).

Constants come from `shared/thoughts.ts` directly; keep them in
lockstep. The TS-side critical-need bypass-suppression change (the
reason this work started — see the val-side proposal in the same
review) lands as a server-side refinement; the device runs the
predicate locally either way so a sluggish round-trip never hides a
real need from the kid.

Ordering matters: the first match wins. Hungry beats sleepy beats
lonely matches the web ordering and matches how the kid's attention
should triage.

### Rendering

The bubble is rendered into the same 200×200 1bpp MSB-first composite
framebuffer the pet + chrome are blitted onto. One bubble per frame
(M1 cap; M2+ may promote to a stack of up to ~3, mirroring the web
side's stackPos opacity ladder).

**Layout — see `firmware/main/thought.cpp` constants:**

```
y=0   ┌──────────────────────────┐
      │ status bar (19 px)        │
y=19  ├──────────────────────────┤
y=22  │ [icon TL]    ╭────────╮  [icon TR]   ┐
      │              │ sleepy │              │
      │              │ tap sl │              │  bubble
y=58  │              ╰────╥───╯              │  body
                          ▼  ← tail              ┘
y=64
      │               <pet>                  │
      │                                      │
      │ [icon BL]                  [icon BR] │
y=200 └──────────────────────────────────────┘
```

- Bubble x=54..146 (92 wide) — exactly between TL (ends x=52) and TR
  (starts x=148) care icons so the bubble *never* overlaps them.
- Bubble y=22..58 (36 tall) — 3 px below the status bar's bottom
  border, 34 px above the pet head.
- Tail: 6×6 triangle pointing down to the pet's head, centered on
  x=100 (pet midline).
- Two centered scale-1 text lines (8×8 font, "sleepy..." + "tap
  sleep") — 72 px wide each, 10 px margin either side.
- Border: 1-px black outline with chamfered corners (top/bottom rows
  shortened by 1 px on each side, no per-corner pixels) so it reads
  as rounded rather than boxy.

**No new sprite fetches.** First milestone is text-only; the web side
embeds a small ui-v1 cell (moon, bowl, heart). We can add that in M2
by reusing the same icon-fetch pipeline that already loads the four
corner icons; for now the text is enough to read at arm's length.

### Touch dispatch

`classify(x, y)` in main.cpp checks the thought hit rectangle *first*
when `s_thought_active`. The hit rect is enlarged 6 px on every side
from the visible bubble + tail so off-by-a-finger taps still register
— same forgiveness margin as the corner icons.

On `Zone::Thought` tap:

1. Resolve `event_kind_t` from `s_active_thought.action_event`.
2. Append to `event_log`, enqueue via `pet_sync_enqueue`, bump
   `pet_sync_touch`.
3. Render the action's transient expression immediately (`sleeping`
   for SLEPT, `eating` for FED, …) — the same 5 s hold the corner
   icons use.
4. Set `s_thought_suppress_until_ms = now + 30 s` so the same need
   can't re-surface locally before the substrate confirms.
5. Clear `s_thought_active` so the next render skips the bubble.

The 30 s suppression mirrors `shared/thoughts.ts`'s POST_ACTION_QUIET
(90 s in the web because suppression there serves a different
purpose; on-device the sync round-trip is fast and we want the bubble
back if the action failed silently).

### Lifecycle

- Per render: `thought_generate` is consulted; bubble re-renders if a
  thought applies *and* suppression has elapsed.
- 60 s idle tick: bubble state changes (appeared, disappeared, or
  swapped target) trigger a re-render even if the resting sprite
  hasn't changed — same gate as the existing sprite-change check.
- After a thought-bubble tap: suppression window starts; bubble
  cleared from local state until the next substrate refresh confirms
  or denies the action.

## Open / deferred

- **Bubble dismiss (no-action close).** Web has a separate dismiss
  affordance (tap the bubble = act; long-press / outside tap =
  dismiss). The device's tap model is single-shape; no long-press
  policy yet outside the voice/PWR gestures. For M1, the kid either
  acts on the bubble or waits for the need to clear naturally.
- **Bubble icon inside the box.** Web embeds a ui-v1 cell (moon, bowl,
  heart) on the left of the text. M2 work; needs a moon icon load.
- **Stacked thoughts.** Web shows up to 4 at once with opacity
  laddering. M3+ work; for now one bubble at a time suffices.
- **Server-authored thoughts.** Web's `Pet.thoughts` field carries
  server-injected thoughts (e.g. `pushThoughtFromTool` from realtime
  voice tools). M11 dropped the field; M13 sync could carry it back.
  Until then, all device-side thoughts are locally generated from the
  pet snapshot — no transport surface to worry about.
- **Web-side parity.** The val-side `c15r/mochi:shared/thoughts.ts`
  needs a suppression-bypass for critical needs so the same situation
  surfaces in the browser surface; that's a separate change tracked
  on the val (not part of this milestone).

## What landed in M1

- `firmware/main/thought.h` + `thought.cpp`: types, generation,
  bubble rendering. Single thought, SLEEPY only.
- `firmware/main/main.cpp` integration: per-render compute,
  bubble blit after chrome, touch dispatch via `Zone::Thought`,
  30 s post-action suppression, 60 s tick refresh on thought
  changes.
- `firmware/main/CMakeLists.txt`: `thought.cpp` registered.

Validation (next pass on hardware):

- Energy decays below 28 (or a forced low-energy snapshot) → bubble
  appears within ≤ 60 s of the next idle tick.
- Tap inside the bubble → device renders `sleeping` for 5 s, posts
  `slept` to `/api/mutate`, pet falls asleep server-side, bubble
  disappears.
- Tap outside the bubble (corner icons / pet body) → existing care
  flow unchanged.
- Pet wakes (via PWR long-press) and energy is still < 28 →
  bubble re-appears.
