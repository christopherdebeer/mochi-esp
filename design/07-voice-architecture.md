# 07 — Voice architecture

Status: draft, 2026-05-18 (decision locked, no code yet)

How voice works on the eink-pet device. Companion to
`c15r/mochi:design/voice-realtime.md`, which remains canonical for
persona shape, tool surface, care injection, and the asleep-mode
flow. This doc covers only what's different on the device:
**transport**.

## The constraint that shapes everything

`mochi.val.run` plays no role in the device's voice path. The device
authenticates direct to OpenAI's Realtime WebRTC endpoint using a
BYO API key stored in NVS. The server's only voice-adjacent jobs
are supplying persona/instructions text and accepting transcript
writes — both text-only, no OpenAI key involved, no audio touched.

This is a stronger separation than the web prototype gets. The web
path stores the user's BYO key server-side and mints ephemeral
tokens; mochi.val.run sees the key briefly during mint. The device
path never lets the key leave NVS except in an `Authorization:
Bearer` header to api.openai.com directly.

The architectural restatement: **mochi.val.run handles fiction
(persona, transcripts, scene contracts, tool result interpretation);
the OpenAI key and audio are between the user, their device, and
OpenAI.** Other paths are possible (server-minted ephemeral tokens,
server-proxied audio); we explicitly rejected them.

## What lives where

```
┌─────────────────────────────┐         ┌───────────────────────────┐
│ Device                       │         │ mochi.val.run             │
│                              │         │                           │
│ NVS:                         │         │ Knows: pet state,         │
│   wifi_creds                 │         │   identity, persona,      │
│   pet_id                     │         │   memory, scene contracts │
│   openai_key   ◄─────────┐   │         │ Does NOT know: OpenAI key │
│                          │   │         │ Never sees: audio         │
│ Voice flow:              │   │         │                           │
│   1. fetch instructions  │───┼─HTTPS───►  /voice/realtime-instr   │
│      (text only)         │   │         │  /api/voice/tool          │
│   2. open WebRTC peer    │   │         │  /api/mutate (transcripts)│
│      authenticated with  │   │         │                           │
│      openai_key direct   │   │         └───────────────────────────┘
│      to OpenAI                                       
│   3. session.update with                             
│      fetched instructions                            
│   4. audio over WebRTC ───────WebRTC────►   api.openai.com         
│   5. tool calls back via                  Realtime endpoint        
│      data channel; substrate                                       
│      writes go to mochi                                            
│   6. transcripts posted to                                         
│      mochi after the fact                                          
└─────────────────────────────┘
```

The diagram makes the key invariant visible: the dashed line from
NVS' `openai_key` slot only ever connects to OpenAI's WebRTC
endpoint, never to mochi.val.run.

## Why direct authentication, not ephemeral tokens

OpenAI supports two auth modes for the Realtime WebRTC endpoint:

1. **Ephemeral tokens.** Server mints a token (`POST /v1/realtime/
   client_secrets`) using a standard API key, returns to client,
   client uses for SDP exchange. 2-hour TTL. The web prototype uses
   this.
2. **Direct API key.** Client uses the standard API key in the
   `Authorization` header for the SDP exchange. 30-minute session
   TTL.

The web prototype prefers ephemeral because the browser is
fundamentally untrusted (any page can read its local storage).
The device's NVS is not browsable from the outside under normal
operation, so the threat model is different: theft of physical
device implies access to the key. Acceptable, treated the same as
theft of phone implies access to bank app.

The 30-min vs 2-hour TTL is non-binding for us. Sessions are capped
at 10 min by policy (see "Session lifecycle" below); the TTL never
fires.

## Provisioning flow (M3.1)

The captive portal at first-boot collects two fields:
- WiFi SSID + password (existing M3)
- OpenAI API key (new M3.1)

Both persist to NVS in separate slots. The key is not validated at
submit time — failures surface at the first voice session, which
is recoverable. Validating would burn one HTTPS round-trip per
attempt against the user's account.

Re-provisioning paths:
- **Factory reset** (M8.5, existing). PWR + BOOT held 10 s wipes
  NVS entirely — both WiFi and API key. Next boot returns to
  captive portal.
- **Re-key only.** Not yet designed. Possible shapes: (a) a long-
  press gesture re-enters SoftAP mode with key-only form; (b) a
  local HTTP server on home WiFi exposing a key-update endpoint;
  (c) just factory-reset and re-pair. (c) is the v1 answer.

## Voice session lifecycle

```
idle
  │ tap bottom-centre band
  ▼
fetching-instructions  ── GET mochi.val.run/voice/realtime-instructions
  │
  ▼
connecting             ── POST api.openai.com/v1/realtime SDP
  │                     ── DTLS handshake
  │                     ── data channel open
  │
  ▼
ready                  ── session.update with instructions
  │                     ── status bar "talking"
  │
  ▼
conversing  ◄─┐
  │           │ (model speaks / user speaks)
  │           │ tool calls route device → mochi → openai
  │           │ care events push [from your body] notes
  │           │ asleep transitions swap instructions via session.update
  │           │
  │           │ idle 30 s | hard cap 10 min | tap to stop | error
  ▼
closing                ── tear down peer
  │                     ── POST transcripts to mochi.val.run/api/mutate
  ▼
idle
```

Triggers:
- **Initiate.** Tap bottom-centre band — mirrors the web prototype's
  bottom-row voice chip placement. Tap-to-start.
- **Stop.** Tap bottom-centre band again. Tap-to-stop.
- **Hard cap.** 10 min absolute. Forces explicit re-engagement;
  bounds cost; well inside OpenAI's 30-min direct-auth ceiling.
- **Idle cap.** 30 s of no model audio AND no user VAD activity →
  close.
- **Asleep transition.** Keep session open, swap instructions via
  `session.update`. Same as web. The session stays alive across
  the transition; only the prompt changes.
- **Connection error.** Drop with status bar "voice disconnected
  — tap to retry". Common cause to plan for: revoked or invalid
  API key. The retry path eventually needs a re-key flow (see
  "Re-provisioning paths" above).

## Tool call routing

Tools flow device ↔ OpenAI direct over the WebRTC data channel.
For tools that touch the substrate, the device handles the
substrate write by round-tripping through mochi:

```
1. OpenAI emits  response.function_call_arguments.done
                 { call_id, name, arguments }
2. Device routes to mochi:
   POST mochi.val.run/api/voice/tool?name=<tool>
   body: { call_id, args, pet_id }
   → ToolResult { ok, reason | ... }
3. Device sends to OpenAI:
   conversation.item.create
     { item: { type: "function_call_output", call_id, output: <ToolResult json> } }
   response.create
4. Model takes the result, says one more thing in its own voice
   incorporating the outcome, turn completes.
```

The seven tools (`settle_to_sleep`, `wake`, `request_care`,
`note_about_human`, `set_internal_observation`, `move_to_location`,
`acknowledge_care`) all route the same way. The substrate retains
authority over every proposal; refusals come back as
`{ ok: false, reason: "..." }` and the model folds the reason
into its next utterance. See
`c15r/mochi:design/realtime-tools.md` for the full tool spec.

Latency: tools resolve between turns, not mid-utterance, so the
device→mochi round trip has a generous budget. Typical: ~100–300 ms
to mochi.val.run from a residential connection.

## Care injection

When device-side care actions fire during a live session (centre-
band gestures, eventually scene-contract zones), the device pushes
a `[from your body]` system note via the data channel and forces
a fresh response:

```
{ type: "response.cancel" }
{ type: "conversation.item.create",
  item: { type: "message", role: "system",
          content: [{ type: "input_text",
                      text: "[from your body] your human just hugged you.
                             acknowledge briefly in your own voice." }] } }
{ type: "response.create" }
```

The model interrupts whatever it was saying, reads the system note,
produces a fresh reply acknowledging the moment. Same shape as
`useRealtimeVoice.notifyCare()` on web; same event sequence.

## Transcript persistence

Device assembles per-turn transcripts from `conversation.item.
created` events on the data channel, POSTs to
`https://mochi.val.run/api/mutate` with `kind=talked` (or
`kind=set_internal_observation` when the pet is asleep, per the
existing routing logic in `useRealtimeVoice.persistTurn`). Text
only, no audio. Memory ends up the same shape as web.

Privacy posture: no audio file ever exists. The transcript is the
durable record. This is identical to web; just worth restating
since the device's mic is a continuously-listening device people
might worry about.

## Session-config posture

The mint body (`voice/openai_signaling.c::get_ephemeral_token`)
binds the input-audio behaviour the model uses for the whole
session. The current shape is deliberately conservative to pair
with the absent-AEC posture (see below):

| field                                | value                          |
|--------------------------------------|--------------------------------|
| `audio.input.format.rate`            | `24000`                        |
| `audio.input.transcription.model`    | `gpt-4o-mini-transcribe`       |
| `audio.input.turn_detection.type`    | `semantic_vad`                 |
| `audio.input.turn_detection.eagerness` | `"low"`                      |
| `audio.output.format.rate`           | `24000`                        |
| `audio.output.voice`                 | `marin` (matches mochi-val)    |

Why `semantic_vad` with `eagerness=low` rather than `server_vad`:
the semantic VAD estimates whether the user has finished speaking
rather than firing on volume alone, which is a closer match for
the failure modes a single-mic, no-AEC board produces — speaker
bleed at moderate volume looks like ambient speech to a volume
threshold, but to semantic VAD it's nothing intelligible to
finish. `eagerness=low` further blunts the trigger.

`interrupt_response` and `create_response` are not set; defaults
apply. We have not had to disable interruption because the
half-duplex mute (next section) drops mic uplink before the model
can hear its own playback in the first place. If a future build
re-enables full-duplex mic uplink before AEC has been verified
on hardware, the safe interim move is to add
`"interrupt_response": false` to the `turn_detection` block — it
keeps in-flight responses from being cancelled by VAD starts that
were triggered by echo.

## Acoustic echo cancellation

The web path free-rides on WebRTC's browser-implemented AEC. The
device gets AEC via Espressif's AFE (Audio Front-End) algorithm,
included with `esp-webrtc-solution` and active by default.

**Hardware caveat.** The reference board for `esp-webrtc-solution`'s
`openai_demo` is the ESP32-S3-Korvo-2, which provides a hardware
echo reference channel via its onboard ES7210 quad-channel ADC.
The Waveshare ESP32-S3-Touch-ePaper-1.54 V2 has only the ES8311 —
single analog mic, no hardware reference channel. AEC still works
in software-reference mode (sample the I²S output, feed it into
AFE_VC as the reference channel), but typically with 5–10 dB worse
cancellation than the hardware-reference configuration.

For a quiet domestic environment, software-reference AEC is
expected to be adequate. For a noisy environment, the user will
notice the model occasionally transcribing its own voice back at
itself, which manifests as the conversation getting stuck in
ping-pong loops. If this becomes a real complaint, the next-step
options are:
- Aggressive AFE tuning (increase suppression at the cost of
  user-voice clarity)
- Hardware revision to add an ES7210 (real fix)
- Half-duplex fallback with tap-to-talk (tear out duplex entirely;
  loses barge-in)

### Half-duplex mute (current echo defence)

While the software-reference AEC is being brought up, the active
echo defence is a half-duplex mic-mute gate maintained in
`voice/voice_peer.c::voice_peer_mic_should_mute`:

- Active when phase = `VOICE_PHASE_SPEAKING` (model generating).
- Active for `VOICE_LOUD_DRAIN_MS` (700 ms) after the last "loud"
  (> `VOICE_DTX_FRAME_BYTES`) audio packet from the server, so
  the I²S DMA tail drain doesn't bleed into the mic.
- Mic task continues to consume I²S frames so DMA doesn't overrun;
  the gated frames are dropped, not held.

The cost is the model can't be interrupted mid-utterance. Worth
it against a self-interrupt loop until full-duplex with AEC is
verified.

### Software-reference AEC scaffold (M9.f.3 in progress)

The `voice/voice_aec.{c,h}` module owns the software-reference
pipeline. Reference tap is in `voice_peer.c::pc_on_audio_data`
between decode and I²S write; process call is in
`voice_mic.c::mic_task` between the I²S read and the half-duplex
mute gate.

Shape:

```
pc_on_audio_data:
  opus_dec_decode
    → voice_aec_push_ref(pcm, samples)        ← reference tap
    → esp_codec_dev_write

mic_task:
  esp_codec_dev_read
    → voice_aec_process_in_place(pcm, samples) ← cancel here
    → voice_peer_mic_should_mute()             ← fallback gate
    → esp_opus_enc_process
    → voice_peer_send_audio_frame
```

The reference ring buffer (1 s of 24 kHz mono in PSRAM, lock-free
SPSC) and resamplers (24↔16 linear) are landed and exercised on
every call. The esp-sr `aec_create`/`aec_process` binding is
gated behind `VOICE_AEC_USE_ESP_SR` (default 0). The runtime
enable flag (`voice_aec_set_enabled`) defaults false, so the
mic path is unmodified at runtime today.

To enable on hardware:
1. Add `espressif/esp-sr` to `firmware/main/idf_component.yml`.
2. Set `VOICE_AEC_USE_ESP_SR=1` (build flag or top-of-file).
3. Wire a `voice_aec_set_enabled(true)` call once the peer is up
   (e.g., on data-channel open, alongside the existing voice setup).
4. Validate via diag log counters (`pushed/pulled/under/over/proc`)
   that ref ring traffic and processed-frame counts look healthy.
5. Tune `AEC_FILTER_LENGTH_MS` (4 → 8) and AEC mode
   (`FD_LOW_COST` → `FD_HIGH_PERF`) if echo bleed is audible.
6. Once stable, relax the half-duplex mute gate to allow barge-in.

## Status bar UX

Voice events surface through the status bar (existing M8.5
infrastructure). State strings:

| state                  | bar text                                  |
|------------------------|-------------------------------------------|
| idle (just provisioned, no voice ever)  | normal status (time / name / battery) |
| connecting             | "connecting…"                             |
| ready (listening)      | "listening"                               |
| model speaking         | "talking"                                 |
| tool call in flight    | "thinking"  (covers the device→mochi→openai round trip) |
| asleep (in session)    | normal status (model handles silently)    |
| disconnected (error)   | "voice disconnected — tap to retry"       |

The status bar is the only UI affordance during a session in v1.
Pet expression cells continue to update normally — voice doesn't
freeze the pet rendering.

## Open questions

These are open and will shape iteration after M9 / M9.5 land:

- **Re-keying without factory-reset.** The v1 answer is "factory
  reset and re-pair" because it's simple. A long-press gesture
  re-entering SoftAP mode with a key-only form would be friendlier,
  but adds complexity to the boot state machine. Wait for the
  first user complaint before building.
- **Connection error UX.** "voice disconnected — tap to retry" is
  thin. Distinguishing "no internet" from "bad API key" from
  "OpenAI down" matters for what the user does next. Defer until
  we hit the failure modes.
- **Session telemetry.** Telemetry stays on (session counts, token
  estimates, error events, transcript lengths) but is not surfaced
  on the device. Surfacing lives in a web admin interface in a
  later milestone. The structure of what gets logged is open.
- **First-turn latency hiding.** ~1–2 s on device for the first
  WebRTC turn. Can we hide it behind a longer pre-roll animation,
  or surface it honestly via the "connecting…" state? Test both
  on real users before deciding.
- **Multilingual.** The web prototype's voice-realtime doc notes
  Realtime can speak any language naturally. Device inherits this
  for free since persona is server-built. No new work needed; just
  noting that it works out of the box.

## Cross-references

- `c15r/mochi:design/voice-realtime.md` — persona shape, tool
  surface, asleep-mode flow. Canonical for everything except
  transport.
- `c15r/mochi:design/realtime-tools.md` — the seven tools and
  their substrate semantics.
- `01-bring-up-plan.md` — M3.1, M9, M9.5 milestones.
- `06-scene-contracts.md` — future home of per-scene voice
  affordances (M11.5+).
- workspace `96b0187f42e946` — M1–M8 bring-up receipt.
- workspace `d15c67f10e0d4b` — devsprite paper-stroke (unrelated,
  but the version-suffix-in-etag pattern from that entry is a
  useful reference for how to ship server-side changes without
  device updates — same pattern would let us iterate
  `/voice/realtime-instructions` server-side).
