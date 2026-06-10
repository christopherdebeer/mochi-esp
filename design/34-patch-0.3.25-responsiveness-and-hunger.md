# 34 — Patch 0.3.25: closing design/25 C3/C4 + the hungry thought (M2)

Status: implemented, 2026-06-10
Branch: `claude/mochi-beta-ota-patch-dbnf2p`
Channel: beta first (PR build → rolling `beta` pre-release), stable on merge.
Base: rebased onto the PR #24 train (`claude/overnight-telemetry-analysis-j4q2A`,
through 0.3.24 — OTA retry gating, cache-first travel, design/31–33), so the
beta build carries both lines and sorts above the 0.3.24 betas already in
the field.

A small ergonomics patch in the design/25 lineage: take the highest
user-felt items still open on that backlog, plus one piece of the
diegetic-interface vision that's been sitting promised-but-unbuilt in
`thought.cpp` since M1. Everything here is touch-loop feel and bubble
behaviour — no schema, no protocol, no partition changes.

---

## 1. design/25 C3 — post-voice freeze → async pull

`main.cpp`'s voice-stop path ran `pet_sync_pull_now()` inline on the
main loop — a synchronous `/api/state` GET that could hold touch, PWR,
and render hostage for ~15 s at the exact moment the kid turns back to
the panel after a conversation.

Fix: `pet_sync_request_pull()` — a sentinel message (`EVENT_NONE`) on
the existing push-worker queue. The worker runs the pull off-loop; the
travel block picks the new location up a tick later via its existing
`pet_sync_current_location()` poll. `pet_sync_push_now()` skips
sentinels when draining pre-sleep.

**Bonus fix found en route:** the push worker's 5-minute periodic
resync called bare `do_state_pull()`, which refreshes location /
costume / places but **never commits the pulled pet stats** to the
snapshot (or NVS). Server-side care — web-app taps, voice-tool care on
another surface — was silently dropped between device mutates. The
worker's pulls (periodic + on-demand) now route through
`pet_sync_pull_now()`, which commits + persists.

## 2. design/25 C4 — travel refresh joins the hybrid policy

Travel swaps did `render_with_expression(..., full=true)` on every
arrival — a ~1 s blank-flash even when the place pack was warm from
the design/29 prefetch ring. In-place scene nav has used the
partial/full-every-`SCENE_NAV_FULL_EVERY`(=4) hybrid since design/17.

Travel now shares one file-scope swap counter with scene nav: both
paths are whole-scene swaps, so the ghosting budget is genuinely
shared — whichever path lands the 4th swap pays the cleaning full
refresh. Warm travel is now a partial-refresh blink.

## 3. design/29 — the departure bubble says where mochi is going

The instant tap ack on a `nav_place` tap read `"traveling..."` —
device-speak, not mochi-speak (design/29 sketched "off to the …" and
the generic text shipped instead). It now reads `"off to the
<place>..."` / `"heading home..."`, the same kid-readable place-id
register the travel-fail bubble already uses ("can't get to the %s").

## 4. design/12 M2 — the hungry thought

`thought_generate()` shipped M1 (SLEEPY) with a comment reserving the
M2 hungry rule. The web side has had it from the start
(`shared/thoughts.ts`: fullness < 35 is the *top* critical need).
On-device, a quietly starving pet surfaced nothing — the kid had to
divine hunger from the resting face, which contradicts the
care-needs-as-invitations posture (design/12, the diegetic-interfaces
memo).

Now: awake + fullness < 35 → cloud bubble `"hungry...\ntap feed"`,
tap = `EVENT_FED` through the existing CARE_EVENT dispatch (the bowl
icon was already in `ACTION_ICONS`, waiting). Ranked above SLEEPY to
match the web chain's order. The threshold deliberately keeps the web
value 35 (where device SLEEPY runs stricter at 10): feeding is the
loop the kid owns, and hunger with no invitation reads as neglect the
kid wasn't told about.

## 5. Voice transcript could silently corrupt the telemetry row

`voice_peer_get_transcript_json` `snprintf`-truncated into a 4 KB
buffer; the accumulator's worst case (12 turns × 2×159 chars +
JSON-escape overhead) runs ~8 KB. A truncated array is invalid JSON,
and it's spliced verbatim into the `/api/device/voice-session` POST
body — so a *long, good* conversation was exactly the one whose
telemetry row broke. Buffer is now 12 KB (PSRAM, freed immediately),
and the JSON builder drops the transcript whole (with a warn log)
rather than emitting a truncated splice.

## 6. Not taken (and why)

- **design/25 C1/C2** (travel pack fetch / cold care-sprite fetch on
  the loop): the design/29 prefetch ring + instant ack pulled the
  common case warm, and the #24 train's cache-first travel render
  (no ETag probe on the render path) closed most of what remained of
  C1; the cold-first-visit fetch and C2's care-sprite path still want
  the full async-worker restructure, which is bigger than a patch and
  touches the render-ownership model. Next minor.
- **C6** (silent render-fetch failure feedback): wants the same async
  restructure to do honestly; deferred with C1/C2.
- **Lonely → talk_seed thought (M3)**: needs TALK_SEED payload
  plumbing on `pet_thought_t` and a seed-text source; out of patch
  scope.
- **Thought-bubble ellipsis edge case** (review finding): can't fire
  with current geometry — interior width pins `max_chars` at 15,
  ellipsis needs ≥ 3. Checked, no action.

## 7. Verification

- `idf.py build` green (esp32s3, ESP-IDF v5.3) at 0.3.25.
- Not yet validated on hardware: the hungry bubble (needs a genuinely
  hungry pet or a low-fullness snapshot), travel partial-refresh feel
  vs ghosting over many swaps, and the post-voice responsiveness win.
  All beta-channel observable; travel + voice paths log to
  device_logs as before.
