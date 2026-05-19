# 06 — Scene contracts (stub)

Status: stub, 2026-05-18. Placeholder for the device-side shape of
the diegetic-interfaces vision. Full design lands with M11.5.

## Pointer

The vision is in `c15r/mochi:design/diegetic-interfaces.md`. Read
that first. The short version, as it relates to firmware:

- Each scene the pet inhabits is defined by an **image** (what the
  user sees) plus a **contract** (what the device routes against).
- The contract names semantic regions on the panel — "the bowl",
  "the door", "the toy on the shelf" — with permissive bounding
  geometry. A tap is matched to whichever named region contains
  it; voice intents resolve to the same names.
- Gestures (long-press, drag, double-tap) are *modifiers* on the
  region they target, not nested affordances inside it. "Tap bowl"
  vs "long-press bowl" are two intents on one region, not one
  region with two sub-buttons.
- The realtime agent (server-side) is the live director that
  assembles the scene contract for the current pet state and
  emits it alongside the scene image.

## What the device side will need

Not built yet. M11.5 work, in rough order:

1. **Scene-contract loader.** Fetch the contract for the current
   scene (alongside the scene image), validate, hold in PSRAM.
   Format TBD — likely a small JSON or CBOR blob with named
   regions, bounding shapes (rect or polygon), and the intents
   each region accepts. Lives next to or appended to the scene
   bitmap; one wire fetch per scene change.
2. **Intent router.** Replaces the current hardcoded
   `find_zone(x, y)` switch in `main.cpp`'s touch handler. Looks
   up a tap point against the active scene's contract regions,
   produces a `(region_name, gesture)` intent, dispatches to the
   right local handler or pushes to the realtime agent for
   server-side resolution.
3. **Gesture modifier layer.** Long-press, drag-from, double-tap
   detection on top of the existing FT6336 event stream. Modifies
   the gesture half of the `(region, gesture)` intent.
4. **Pet-state hook.** Per `01-bring-up-plan.md` M11 note: the
   compositor needs read-access to the current pet mood/decay
   state so the scene-contract renderer can pick the right pet
   expression cell per frame.

## What gets retired when this lands

- The four-corner care icons in `main.cpp`'s touch handler.
- The hardcoded tap zones (top-left = food, top-right = play, etc.).
- The fixed iOS-style status bar at the top of the panel — care
  affordances should be *in the scene*, not in fixed chrome.
  (Time + battery + name may survive in some diminished form, or
  may move into a per-scene chrome region; vision doc is
  permissive on this.)

These exist today as scaffolding to exercise the rest of the
pipeline (touch + composite + cache + status bar render). They
unblocked M8.5; they are not the long-term shape.

## Status quo (M8.5, current)

What's actually wired on the device today, for reference when the
real design lands:

- Touch input via FT6336 produces `(x, y)` events on a queue.
- A hardcoded `find_zone()` maps the panel into 5 zones: 4 corner
  squares (~80×80 each) + 1 center band; corners pick a care icon
  (food / play / tickle / clean), center cycles pet expression.
- The matching cell is fetched from
  `/devsprite/cell/pet-v1/<expression>` (cache-first, LittleFS
  via `sprite_cache.{cpp,h}`), composited over the cached scene,
  rendered, and times out back to neutral after 5 s.
- Scene is a single hardcoded cell (`scene-v1/0`), fetched at boot
  via `/devsprite/scene-v1/0?fit=fill`, cached, composited under
  every pet expression.

This is one scene. The pet inhabits one room. The corner icons
are the same in every conceivable scene because there are no
other scenes. M11.5 unfreezes all of that.

## Cross-references

- `c15r/mochi:design/diegetic-interfaces.md` — the vision this
  file is the device-side stub of
- `01-bring-up-plan.md` — M11.5 milestone definition; M9/M10
  reframings (audio + realtime agent are the input + director
  side of the same loop scene contracts run on)
- `05-sprite-format.md` — the wire format scene contracts will
  ride alongside; "Future direction" section there points here
- `02-boot-sequence.md` — boot eventually grows a "fetch current
  scene contract" step in the WARM BOOT branch
