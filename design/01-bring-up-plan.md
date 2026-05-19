# 01 — Bring-up plan (M1 → M4)

Status: draft, 2026-05-17

Four milestones from bare board to "device renders a Mochi sprite fetched
from the server". Each milestone is the smallest increment that gives
information about the next one. Touch, audio, RTC, SHTC3 are deliberately
deferred — they're separate validations after the spine works.

The discipline: a milestone is *done* when its acceptance criterion holds,
not when "most of it works". A flaky M2 is a problem M3 inherits and that
makes the whole exercise harder to debug.

---

## M1 — LED + USB serial ✅ done 2026-05-18

**Goal.** Toolchain works, board enumerates, firmware flashes, LED on GP3
blinks, USB CDC prints once per second.

**What it proves.**
- ESP-IDF (or whatever toolchain we land on) is correctly installed
- USB-C cable + driver chain works on the host
- The board enumerates as an esp32-s3 target
- Flashing via `idf.py flash` succeeds end-to-end
- USB CDC serial out works (this is the debug channel for every later
  milestone — if it's flaky, fix it now)

**Acceptance criterion.** LED blinks at 1Hz; `idf.py monitor` shows a
heartbeat line "tick N — uptime Ns" every second.

**Estimated effort.** 1 hour if the toolchain has been set up before; 3–4
hours if first-time ESP-IDF install. Most of the time is environmental.

**Risks.**
- GP3 is an ESP32-S3 strapping pin. If the LED doesn't blink, check whether
  the silkscreen is accurate — verify against the Waveshare wiki schematic.
- USB CDC requires `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in sdkconfig; if
  output goes to UART0 instead, monitor will hang.

---

## M2 — E-paper hello ✅ done 2026-05-18

**Goal.** SPI init for the SSD1681 e-paper controller, render a static
200×200 bitmap. Both full refresh and partial refresh exercised.

**What it proves.**
- SPI bus + pin assignments (CS, DC, RST, BUSY, MOSI, SCK) are correct
- Display controller responds to init sequence
- Frame buffer geometry (200×200 = 5KB at 1-bit) is right
- Refresh timing is understood — full refresh ~1.5s with visible flash,
  partial refresh ~300ms invisible. This budget shapes the rendering
  pipeline more than anything else.

**Acceptance criterion.** Display shows a static checkerboard or "MOCHI"
logo. Pressing the boot button cycles between three reference patterns
using partial refresh; every fourth press uses full refresh.

**Estimated effort.** 4–8 hours. Pin assignments from the wiki are the
bulk; once the bus is initialised the rest is mechanical.

**Open question.** Use the Waveshare vendor C driver (pragmatic, ugly) or
a community ESP-IDF e-paper component (cleaner, may need patching for
this exact controller). Start vendor; switch if it bites.

**Resolution.** Vendored the C++ driver from
`waveshareteam/ESP32-S3-ePaper-1.54` commit `3f96bee` into
`firmware/vendor/waveshare-eink/`. First-pixel time was ~3 hours
including the schematic-recovery diversion (no PDF on hand → cloned
the repo, used the `user_config.h` pin map as the authoritative source
since the repo doesn't ship a schematic either). Acceptance hit on
first flash: boot full refresh worked, partial refresh + cycling
worked, full refresh every 4th press worked.

**Known cost to revisit later.** Vendor's `EPD_DrawColorPixel` is a
function call per pixel doing bit math against the framebuffer; a
full 200×200 redraw via per-pixel calls measured ~800ms wall-clock
inside `do_partial_refresh()` (most of which is *not* the SSD1681
itself — partial refresh is meant to be ~300ms). When sprite
animation makes this ceiling matter (M11 region), replace the vendor
component with a clean driver that exposes packed-byte writes into
the framebuffer. Until then, ugly is fine.

---

## M3 — Provisioning (SoftAP + captive portal) ✅ done 2026-05-18

**Goal.** Device boots into SoftAP mode with no WiFi creds, shows "Mochi-
XXXX" SSID + portal URL on e-paper, user joins from a phone, captive
portal auto-launches, user enters home WiFi creds, device stores them in
NVS and reboots. On reboot, device joins home WiFi and prints assigned IP.

**What it proves.**
- WiFi stack works (both AP and STA modes)
- Captive portal DNS hijacking works on iOS and Android (the auto-launch
  hinges on the device responding to `captive.apple.com` and
  `connectivitycheck.google.com` probes with a redirect)
- NVS write/read survives reboot
- E-paper can be updated mid-flow (showing "joined!", showing IP, etc.)

**Acceptance criterion.** Cold boot with no NVS WiFi entry → device hosts
SoftAP within 5 seconds → phone joins → captive portal opens
automatically → user enters home WiFi password → device reboots → joins
home WiFi → e-paper shows the assigned IP.

**Estimated effort.** 8–12 hours. The ESP-IDF `wifi_provisioning`
component handles a lot of this, but the e-paper UI integration is
custom. iOS captive portal behaviour is the most likely source of pain.

**Risks.**
- iOS captive portals can fail to auto-launch on some carrier networks
  if there's already a stored MobileConfig. Test on a real iPhone early.
- The SoftAP power draw is non-trivial; don't enable it speculatively.

**Resolution.** Hand-rolled stack landed in five files
(`wifi_prov.{cpp,h}`, `wifi_sta.{cpp,h}`, `nvs_creds.{cpp,h}`,
`epd_ui.{cpp,h}`, `font8x8.{cpp,h}`) plus a refactored `main.cpp`.
Acceptance hit first try on a Vodafone WPA3-SAE network — iOS
captive portal auto-launched correctly, creds validated by real
STA join before NVS persist, reboot path took the already-
provisioned branch and got an IP in ~2s. Two small follow-ups
worth doing before M4 if they bite: (1) the wifi_event_cb retry
logic fires once during AP→STA handover and is harmless but noisy,
(2) iOS sends follow-up requests to the HTTP server after the form
POST that get rejected with ENFILE. Neither blocks acceptance.

---

## M3.1 — Captive portal: OpenAI API key field

Status: pending, prerequisite for M9.

Extends M3's captive portal HTML with a second field for the user's
OpenAI API key. The key is persisted in NVS under `openai_key`,
separate from the WiFi creds slot so factory-reset can wipe either
independently.

**No validation at submit time.** Accept whatever the user types;
surface authentication errors at the first voice session. Validating
at provisioning would mean burning one HTTPS round-trip per attempt
against the user's account, and the failure modes (typo, revoked key,
no internet) are recoverable post-hoc.

**Threat model.** The key transits the device's SoftAP in HTTP
plaintext, identical posture to the WiFi password. The user is on a
freshly-booted device's SoftAP for ~30 seconds during initial setup,
not a public network. Acceptable for v1.

**Why this rather than fetching from mochi.val.run during pairing.**
Architectural constraint: no OpenAI key ever sits on mochi.val.run.
The voice path is device ↔ OpenAI direct (see
`07-voice-architecture.md`); the server's only role is supplying
persona text and accepting transcripts. The key never leaves the
device after provisioning except in the `Authorization: Bearer`
header to api.openai.com.

**Acceptance criterion.** Cold boot with no NVS entries → captive
portal auto-launches → user enters WiFi creds + OpenAI key → device
joins WiFi → reads the key back from NVS on next boot.

**Estimated effort.** 1–2 hours. The captive portal HTML and POST
handler from M3 carry the bulk; this is a second form field and a
second NVS write.

---

## M4 — First sprite from server ✅ done 2026-05-18

**Goal.** Connected to home WiFi, device performs an HTTPS GET to
mochi.val.run for a known-good 1-bit bitmap, decodes it, and renders to
e-paper.

**What it proves.**
- TLS works (mbedTLS in ESP-IDF; root cert bundle works for val.run)
- HTTPS client works end-to-end
- The 1-bit bitmap format we choose (`05-sprite-format.md`) is correct
- Memory pressure during fetch+decode+render is acceptable

**Acceptance criterion.** Device on home WiFi → button press → fetches
`https://mochi.val.run/devsprite/test` (a new endpoint serving a fixed
1-bit 200×200 bitmap) → renders to e-paper. Round trip <3 seconds.

**Estimated effort.** 6–10 hours, much of it server-side adding the
`/devsprite/test` endpoint to mochi.val.run.

**Prerequisite.** Sprite format decision (`05-sprite-format.md`) must
land. Without it M4 is rendering placeholder pixels, not a real sprite.

✅ Prerequisite met 2026-05-18: `05-sprite-format.md` landed, server
endpoint `https://mochi.val.run/devsprite/test` deployed and verified
(returns 5000 bytes of packed 1-bit pixels rendering a 96×96 fox
silhouette + caption, byte-for-byte ready to memcpy into the device
framebuffer).

**Pairing scope.** For M4, the device hardcodes the test pet ID — no
real pairing yet. Real pairing flow lands as M5 (`04-pairing.md`).

**Resolution.** Server side was already prepped (sprite-format doc +
`/devsprite/test` endpoint deployed). Device side landed in three
small files: `EPD_LoadBuffer` added to the vendor driver (one method,
five lines, marked NOT VENDOR CODE), `sprite_fetch.{cpp,h}` for the
HTTPS GET via esp_http_client + crt_bundle, and a `sdkconfig.defaults`
addition enabling the Mozilla CA bundle. Acceptance hit first try —
three consecutive presses produced 1186 / 1142 / 1022 ms round trips,
all well under the 3-second budget. Free win: the wifi_sta retry
logic from M3 handled a transient STA reconnect during boot without
user intervention.

---

## After M4

The minimal firmware spine is done. Subsequent milestones validate
peripherals in parallel rather than serially:

- **M5** — Pairing endpoint + flow ✅ done 2026-05-18. Hand-shake
  between the device and `c15r/mochi`'s existing `(name, pin)`
  identity model: device hits `/api/device/pair-init` with its WiFi
  base MAC, gets a `MOCHI-XXXX` code, polls `/api/device/pair-check`;
  user visits `mochi.val.run/pair-device`, signs in with name+PIN,
  enters the code; device sees `paired:true`, persists `pet_id` to
  NVS, reboots into a post-pair touch loop. Device never sees the
  PIN. Three new server routes + one HTML form + a `device_pairings`
  table; ~210 lines on the device side (`pair_creds.{cpp,h}` +
  `device_pair.{cpp,h}`). Surprise found during bring-up: inline TLS
  handshake from `app_main` overflowed the 3.5 KB default main task
  stack — bumped to 8 KB. See project memory `project-eink-main-stack`.
- **M6** — Touch input ✅ done 2026-05-18. FT6336 capacitive controller
  on the shared I²C bus (GPIO 47/48), INT on GPIO 21, RST on GPIO 7.
  Vendored Waveshare's `i2c_bsp` + `ft6336_bsp` and wrapped in
  `firmware/main/touch.{cpp,h}` with an ISR + queue model. Acceptance:
  centre-tap cycles sprites, corner-taps draw a dot via partial
  refresh, all four corners + center hit zones validated on hardware.
  Surprise found during bring-up: GPIO 42 ("Audio_PWR") actually
  powers all I²C peripherals, not just the codec — leaving it off
  killed the bus universally. See project memory
  `project-eink-power-rails`.
- **M7** — RTC (PCF85063) ✅ done 2026-05-18. Hand-rolled
  `rtc.{cpp,h}` reading/writing BCD time registers 0x04..0x0A. No
  third-party SensorLib dependency. Coin-cell verified working: the
  chip held the Waveshare factory time across reflash, our reads
  came back showing the time advancing correctly between boots.
- **M8** — SHTC3 (temp/humidity) ✅ done 2026-05-18. Hand-rolled
  `shtc3.{cpp,h}` with three-command flow (wake → measure → sleep)
  and CRC-8 verification. Polling-mode measurement (cmd 0x7866 +
  15 ms delay) instead of clock-stretching (0x7CA2) — ESP-IDF v5's
  i2c_master defaults to a stretch tolerance shorter than the
  SHTC3's 12 ms measurement time, which produces ESP_ERR_INVALID_STATE.
  Sane room readings on first try (26.63 °C, 41.4 %RH).
- **M8.5** — "Device feels like Mochi" ✅ done 2026-05-18. Not in
  the original bring-up plan; landed organically as the shape that
  closes the gap between "M8 peripherals work" and "this is a
  pet you can interact with":
  - **Pet-on-scene compositor** (`compositor.{cpp,h}`). On-device
    1-bit compositing with a two-plane (ink + mask) wire format
    so cells with cream/transparent regions composite cleanly
    over scene backgrounds. See project memory
    `project-eink-two-plane-cells`.
  - **Server cell + scene endpoints**. `/devsprite/cell/<sheet>/<cell>`
    emits 2-plane native-resolution cells. `/devsprite/<sheet>/<cell>?fit=area|fill`
    emits panel-area-shaped scenes (no header) for non-square
    layouts. Both ETag-cached server-side.
  - **5-zone touch UI** with care icons + status bar (time + pet
    name + battery percent + 1-pixel divider). Tap a corner →
    fetch matching expression cell → composite → render →
    settle to neutral after 5 s.
  - **LittleFS sprite cache** with per-sheet ETag invalidation
    (`sprite_cache.{cpp,h}`). HEAD probes at boot detect server-side
    artwork changes and drop stale cells. Warm boot is ~3 s of
    cache-only loads vs ~22 s of fresh fetches.
  - **Battery sense** + 10-sample diagnostic (`battery.{cpp,h}`).
    ADC1 ch3 via the 1:2 divider on VBAT_PWR; curve-fit calibration;
    piecewise LiPo discharge curve.
  - **Sleep gesture** (`sleep_gesture.{cpp,h}`). PWR-alone held 3 s →
    render `sleeping` cell + "Asleep — PWR to wake" status bar →
    `esp_deep_sleep_start` with PWR + BOOT as wake sources. E-paper
    persistence makes the asleep screen ambient with no power.
  - **Factory reset gesture** (`factory_reset.{cpp,h}`). PWR + BOOT
    held 10 s → wipe NVS (creds + pair) → reboot.
  - **Battery discharge logger**. Periodic mV log to `/lfs/battery.log`,
    auto-dumped to USB at next boot. Used for tuning the LiPo
    discharge curve to a real cell.

  **Important: the corner-icon UI is a placeholder.** The diegetic-
  interfaces vision (`c15r/mochi:design/diegetic-interfaces.md`)
  rules out fixed UI chrome — care actions should ultimately be
  per-scene zones authored alongside the scene image, not panel
  corners with global meaning. We built corner icons because they
  unblocked exercising the full touch + composite + cache pipeline
  without first building the scene-contract substrate. Treat the
  shape as scaffolding; expect to throw it away when scene
  contracts land. See `06-scene-contracts.md`.

- **M9** — Bring up `esp-webrtc-solution` on the Waveshare board.
  Voice and touch are peer inputs to the same intent stream; M9
  brings up the audio half via the canonical embedded path.
  Architecture lives in `07-voice-architecture.md`.

  **Status (as of 2026-05-19) — bidirectional voice working
  end-to-end.** Long-press centre starts a session; mochi greets
  the user with their real persona (fetched from
  `mochi.val.run/api/voice/realtime-instructions`); user speaks;
  mochi hears + responds in voice. Substeps:
  - **M9.a** ✅ codec_board ES8311 init via vendored
    `WAVESHARE_S3_EPAPER_1_54` board entry.
  - **M9.b** ✅ vendor `i2c_bsp` retired; all I²C consumers
    co-exist via `firmware/main/i2c_bus.{cpp,h}`.
  - **M9.c** ✅ captive portal collects BYO OpenAI key into NVS.
  - **M9.d** ✅ NVS-loaded BYO key mints an ephemeral
    Realtime-API token (GA, `/v1/realtime/client_secrets`).
  - **M9.e** ✅ `esp_peer`-direct glue: signaling pump,
    main-loop worker, manual DCEP open. Reaches
    `DATA_CHANNEL_OPENED` + first `session.created` event.
  - **M9.f.1** ✅ long-press voice trigger; full session config
    in mint body; Opus playback through ES8311.
  - **M9.f.1.5** ✅ phase-driven pet expression (curious /
    comforted / cheerful_wave / neutral); centre-tap-during-
    session sends a fixed text talk-back over the data channel.
  - **M9.f.1.6** ✅ idle-cap (60 s silence) + hard-cap (5 min)
    + remote-disconnect handling; touch-loop polls
    `voice::stop_requested()` so caps don't deadlock the worker
    on its own join.
  - **M9.f.2** ✅ mic capture loop. ES8311 → Opus encode @ VOIP
    24 kHz/20 ms → `esp_peer_send_audio`. Mint body includes
    `audio.input` config: PCM 24 kHz format, server-side
    `gpt-4o-mini-transcribe` STT, `semantic_vad` with
    `eagerness=low`.
  - **M9.f.2.1** ✅ half-duplex mic-mute during SPEAKING phase
    (defence-in-depth against feedback loops without AEC);
    user + assistant transcripts captured into the diag log.
  - **M9.f.3** 🔄 software-reference AEC engaged (on-hardware
    validation pending). `voice/voice_aec.{c,h}` owns the ref ring
    buffer (64 KB PSRAM, lock-free SPSC), 24↔16 kHz linear
    resamplers, and the esp-sr `aec_create`/`aec_process` binding.
    `espressif/esp-sr ^2.4.4` is declared in `idf_component.yml`;
    `VOICE_AEC_USE_ESP_SR=1`; `voice_peer.c::open_audio_playback`
    calls `voice_aec_set_enabled(true)` immediately after init.
    Reference tap in `pc_on_audio_data` between decode and I²S
    write; process call in `mic_task` between I²S read and the
    half-duplex mute gate. The mute is retired from the hot path:
    `voice_peer_mic_should_mute()` short-circuits to false whenever
    AEC is enabled, so a healthy session runs full-duplex. The
    gate only fires if `aec_create` fails — a graceful degradation
    back to M9.f.2.1's behaviour. Validation steps in
    `07-voice-architecture.md` § "Software-reference AEC".
  - **M9.g** ✅ persona fetch from
    `GET /api/voice/realtime-instructions` (re-introduced
    server-side route in mochi-val for the device path; the
    browser client retired its server counterpart in the
    "no keys on server" cleanup wave but the device has no JS
    runtime so it needs the server build).
  - **M9.h** ✅ tool-call routing. Async dispatch worker POSTs
    to `mochi.val.run/api/voice/tool` and feeds the result back
    to OpenAI as `function_call_output` + `response.create`.
    All 12 tools route the same way.

  **Remaining for full M9 acceptance.**
  - Software-reference AEC on-hardware bring-up: AEC is engaged
    by default (mute retired from the hot path) but has never run
    on the device. Validate diag counters
    (`pushed/pulled/under/over/proc`), confirm `muted` counter
    stays at 0, check for audible bleed, exercise barge-in, and
    tune `AEC_FILTER_LENGTH_MS` / mode if needed.
  - 5-min stability soak across the acceptance criteria below.
  - End-to-end on-hardware tool-call validation (M9.h is wired
    + builds + the dispatcher worker started OK in test logs;
    not yet observed firing a real tool against val.run).

  **Why this is the shape.** Espressif maintains
  `espressif/esp-webrtc-solution` as a managed component (v1.0 GA),
  and the bundled `openai_demo` solution targets ESP32-S3 talking
  direct to OpenAI Realtime over WebRTC with AEC integrated by
  default. OpenAI's own `openai-realtime-embedded` repo points to
  it as the canonical embedded reference. Going straight to the
  integrated solution skips the dead-weight intermediate of
  "capture + playback works on bare metal" — the integrated
  solution wouldn't compose against a hand-rolled audio pipeline
  anyway, and the failure modes we'd discover with a hand-rolled
  pipeline are different from the failure modes we'll hit with the
  managed component.

  **What it brings.** WebRTC peer connection (RTP, SCTP data
  channel, DTLS, libSRTP), OpenAI signalling
  (`esp_signaling_get_openai_signaling`), Opus codec, AFE-based AEC,
  tool-call routing over data channel. Resource cost on ESP32-S3
  at 240 MHz: ~22 % CPU, 48 KB SRAM, 1.1 MB PSRAM for AFE alone,
  plus the WebRTC stack itself.

  **What's actually work, not just integration.** The reference
  board is ESP32-S3-Korvo-2 with dual mic + ES7210 quad-channel
  ADC providing a hardware echo reference. The Waveshare board has
  only ES8311 — single analog mic, no hardware reference channel.
  AEC still works but uses a software reference loop (sample the
  I²S output, feed it into AFE_VC as the reference channel).
  Typically 5–10 dB worse cancellation than hardware reference.
  Porting the `audio_board` abstraction from Korvo to our hardware
  is the main M9 effort, alongside the BYO-key auth path.

  **Architecture: zero server involvement in voice.** Device
  authenticates direct to OpenAI's Realtime WebRTC endpoint using
  a BYO API key stored in NVS (collected at provisioning per
  M3.1). `mochi.val.run` plays no role in the audio path. See
  `07-voice-architecture.md` for the full architectural rationale.

  **Acceptance criteria.**
  - WebRTC peer connects to `api.openai.com`, DTLS handshake completes
  - Mic captures non-silence (`d_out > 0`) when speaking — this is
    the failure mode from `esp-webrtc-solution` issue #31, easy to
    miss because the rest of the stack looks fine even with a dead
    mic
  - Speech round-trip happens: speak → model responds → audio
    plays intelligibly through the speaker
  - AEC suppresses self-echo enough that the model doesn't trans-
    cribe its own output back at itself — measured by absence of
    spurious `input_audio_buffer.committed` events during model
    speech in a quiet room
  - Tool call surfaces correctly through the data channel (test
    with `set_internal_observation`, simplest of the seven, no
    substrate write required to validate the channel)
  - Stable across a 5-min conversation: no PSRAM exhaustion, no
    WiFi drops, no e-paper task starvation

  **Risks recorded for context.**
  - **Single-mic AEC quality.** Software reference loop is
    documented in the AFE component but most reference code
    assumes Korvo-class hardware. May need iteration on loopback
    configuration.
  - **`d_out=0` failure mode.** `esp-webrtc-solution` issue #31
    shows even Korvo boards can have mic-capture failures that
    look like everything-else-working. Test the I²S input path
    explicitly with a level meter before declaring success.
  - **PSRAM headroom.** AFE (1.1 MB) + WebRTC stack + LittleFS
    sprite cache + compositor framebuffers in 8 MB. Tight but
    should fit; measure.
  - **E-paper task starvation under AEC load.** AFE pegs ~22 %
    CPU continuously. Compositor partial refreshes during voice
    events need to not get preempted into the SSD1681 BUSY-pin
    timeout window.
  - **First-turn connection latency.** ~200–500 ms on browser;
    likely 1–2 s on device. Status bar needs a "connecting…"
    state for the gap.

- **M9.5** — Mochi integration of voice.
  Persona, tools, transcripts, asleep-mode flow. Lands on top of
  the working M9 audio path. Mostly server-text-plumbing, the
  audio path itself is unchanged from M9.

  **Persona delivery.** Device fetches instructions from
  `GET https://mochi.val.run/voice/realtime-instructions?pet_id=…&asleep=…`
  (text only, no keys involved, no audio). After WebRTC
  `connected`, device pushes `session.update` over the data
  channel with the fetched instructions. Asleep-mode swap uses
  the same mechanism: fetch new instructions on the pet's sleep
  transition, push `session.update`, optionally `response.cancel`
  on the falling-asleep edge. Identical to the web prototype's
  transport-agnostic instruction-swap pattern in
  `c15r/mochi:design/voice-realtime.md`.

  **Tool call routing.** Tool calls flow device ↔ OpenAI direct
  over the WebRTC data channel. For tools that touch the substrate
  (`note_about_human`, `set_internal_observation`,
  `request_care`, `move_to_location`, `acknowledge_care`,
  `settle_to_sleep`, `wake`), the device receives the
  `function_call_arguments.done` event, POSTs to
  `https://mochi.val.run/api/voice/tool?name=<tool>` with the
  args, gets the `ToolResult` back, sends to OpenAI via
  `conversation.item.create { function_call_output }` +
  `response.create`. Tools resolve between turns, not mid-
  utterance, so the device→mochi round trip has a generous
  latency budget.

  **Care injection.** When device-side care actions fire during a
  live session (centre-band gestures, eventually scene-contract
  zones), the device pushes a `[from your body]` system note via
  the data channel and forces `response.create`. Same shape as
  `useRealtimeVoice.notifyCare()` on web; same event sequence
  (`response.cancel` → `conversation.item.create` →
  `response.create`).

  **Transcript persistence.** Device assembles per-turn transcripts
  from `conversation.item.created` events on the data channel,
  POSTs to `https://mochi.val.run/api/mutate` with `kind=talked`
  (or `kind=set_internal_observation` when asleep). Text only, no
  audio. Memory ends up the same shape as web.

  **Session lifecycle.**
  - **Initiate:** tap bottom-centre band — mirrors the web
    prototype's bottom-row voice chip, tap-to-start.
  - **Stop:** tap bottom-centre band again. Tap-to-stop.
  - **Hard cap:** 10 min absolute. Bounds cost; well inside
    OpenAI's 30-min direct-auth ceiling.
  - **Idle cap:** 30 s of no model audio AND no user VAD activity.
  - **Asleep transition:** keep session open, swap instructions.
  - **Token expiry mid-session** shouldn't fire given the 10-min
    cap; if it does, drop with "voice disconnected — tap to
    retry."

  **Acceptance criteria.**
  - End-to-end: tap bottom centre → mochi.val.run instructions
    fetched → WebRTC peer connects → conversation happens →
    transcripts persist server-side → tap to close
  - Tool call routes correctly: voice triggers
    `set_internal_observation`, device POSTs to mochi, mochi
    writes the observation, device returns result to OpenAI,
    model acknowledges in its next utterance
  - Care injection: tap a care affordance during a live session,
    model interrupts current speech and acknowledges the action
  - Asleep transition: pet goes asleep mid-session, instructions
    swap, model goes quiet in-character

  **What this deliberately defers.** Cost surfacing on device.
  Telemetry stays on (session counts, token estimates, error
  events) but surfacing it to the user lives in a web admin
  interface in a much later milestone. The user is on their own
  BYO key and the 10-min cap is the safety net for v1.
- **M11** — Port `shared/decay.ts` + `shared/engagement.ts` +
  `shared/mood.ts` to C. Pet-internal state. Diegetic-interfaces
  doesn't change this directly; it does add the requirement that
  M11 expose a "current pet state" hook the scene-contract
  renderer can read to pick the right pet expression per frame.
- **M11.5 (new)** — Scene-contract loader + intent router. Where
  the device stops baking-in zone semantics and starts taking them
  from the server alongside the scene image. See
  `06-scene-contracts.md`. This is the milestone where the
  M8.5 corner-icon UI gets retired.
- **M12** — Event log persistence (LittleFS)
- **M13** — Sync model (event push + delta pull) (`07-sync-model.md`)

M11–M13 are where "device runs Mochi" actually happens. M5–M9.5 are
peripheral validations whose order can flex (M9 has the broadest
hardware-integration surface, so it's the riskiest of these). M11.5
is the bridge between the M5–M9.5 peripheral world and the diegetic-
interfaces vision; without it the device renders a pet but can't
render the *world the pet inhabits* in any non-hardcoded way.

---

## Cross-references

- `00-architecture.md` — what lives on device vs server
- `02-boot-sequence.md` — runtime state machine M1+ informs
- `03-provisioning.md` — M3 detail
- `07-voice-architecture.md` — M9/M9.5 architecture, BYO-key posture
- workspace `c6a8376def6946` — hardware decision shape
