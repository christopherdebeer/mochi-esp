# 23 — Voice UX + latency

Status: firmware in progress. Built against ESP-IDF v5.3.5; on-hardware
validation pending. 2026-05-25.

## Problems (from the review)

1. **Trigger.** Voice started on an 800 ms *long-press of the centre
   zone* — slow, undiscoverable, and colliding with the pat/attention
   tap.
2. **Latency.** `voice::start_session()` ran **synchronously on the main
   loop**, doing three TLS handshakes back-to-back — `fetch_persona`,
   `fetch_tools`, then the **mint** (which `voice_peer_start` ran on the
   caller, not the worker). ~4–5 s of frozen UI, *then* the worker still
   needed the ~9 s ICE/DTLS/data-channel window.
3. **Expression.** Connecting reused `curious`, which is also the
   attention-tap / pat face — so the connect wait told the user nothing.
   And `design/07`'s status-bar state text was never implemented, so the
   ambiguous face was the only cue.

## Changes

### Latency

- **Combined bootstrap endpoint** (server): `GET /api/voice/session`
  returns `{ instructions, tools }` in one response (same builders as the
  split `/voice/realtime-instructions` + `/voice/tools` routes, which
  stay for back-compat). One handshake instead of two.
- **Prefetch + cache** (device): `voice::prefetch_config()` runs on the
  connectivity worker after WiFi is up and caches persona+tools in PSRAM.
  `start_session` uses the cache → **zero fetches** on the session-start
  path. Cold cache falls back to one combined fetch; both failing falls
  back to the built-in greeting + no tools.
- **Async mint** (device): the mint + signaling bring-up moved off the
  caller into `worker_task`, so `start_session` returns immediately
  (phase → Connecting) and the main loop stays responsive during the
  mint + connect. `voice_peer_start` is now genuinely non-blocking.

Net: from tap to "connecting" is now ~instant (no frozen frame); the
irreducible wait is the mint + ICE/DTLS, surfaced honestly (below)
instead of as a frozen face. The old hold + touch-drain + finger-lift
dance (which only existed to survive the blocking mint) is gone.

### Trigger — dedicated affordance

A **mic glyph at the far-left of the status bar** is the voice control
(`render_chrome`). Tap it to start; while a session is live it becomes a
filled **stop** square and **any tap stops** the session. Handled up
front in the touch loop (before the care/scene pipeline) so a tap during
a session never also logs a care event. Replaces the centre long-press
entirely.

### Feedback

- **Distinct connecting face:** `phase_expr` Connecting → `waking_up`
  (not `curious`). Ready → `comforted`, Speaking → `cheerful_wave`.
- **Status-bar state word:** during a session the centred pet name is
  replaced by `connecting` / `listening` / `talking` (from the voice
  phase), so the state is unambiguous regardless of the pet's face —
  finally implementing the `design/07` status-bar UX.

## Files

- server `c15r/mochi`: `backend/voice-session.ts` (new), mounted in
  `main.ts`.
- `firmware/main/voice.{h,cpp}` — combined fetch, PSRAM config cache,
  `prefetch_config()`, non-blocking start.
- `firmware/main/voice/voice_peer.c` — mint/signaling moved to the worker.
- `firmware/main/main.cpp` — mic affordance + state word in
  `render_chrome`; mic-tap trigger; `prefetch_config()` in `net_worker`;
  `phase_expr` connecting face.

## On-hardware validation (pending)

A clean build doesn't prove the radio/codec path. Flash and check:
1. Tap the mic glyph → "connecting" shows immediately (no frozen frame),
   pet shows `waking_up`, status bar reads `connecting`.
2. Session reaches `listening` (`comforted`); talking flips to `talking`
   (`cheerful_wave`). Any tap stops; status bar returns to name.
3. Confirm the prefetch warmed the cache (`voice config prefetched` in
   the log) and start logs `config: cache`.
4. Time the tap→listening latency vs. the old build.

## Deferred / open

- **Mic glyph placement** is unverified visually (200×200, tight 19 px
  bar). May need nudging; the time field shifted right to make room.
- **"thinking" state** (tool-call round trip) isn't surfaced — no phase
  is exposed for it. Add a phase if tool latency proves confusing.
- **Re-keying** and **connection-error UX** remain as `design/07` open
  questions.

## Cross-references

- `07-voice-architecture.md` — transport, session lifecycle, the
  status-bar UX this implements, and the first-turn-latency open question.
