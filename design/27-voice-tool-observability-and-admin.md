# 27 — Voice tool observability, context injection & admin persona

Status: in progress, 2026-05-29. Prompted by a live voice tool-use test
(fw 0.3.3, boot `bc86936f`) where tool behaviour was hard to diagnose:
move worked but a rejection said "too far", care actions weren't
acknowledged, imagine/costume didn't fire, and almost none of it was
visible in over-the-air telemetry. This doc captures what the test
revealed and the plan to close the gaps.

Predecessors: [07](./07-voice-architecture.md) (voice transport + tool
routing), [16](./16-on-device-imagine.md) (imagine pipeline),
[18](./18-device-observability.md) (device_logs / device_diag),
[26](./26-idle-and-light-sleep.md) (the doze test this rode along with).
Server refs: `c15r/mochi` (legacy web voice + substrate) and
`c15r/mochi-device` (sprite/scene authoring studio).

## What the test showed

Reconstructed from `device_logs` + the server `events` table (the
server-side `events` rows are the only place tool calls are durably
recorded — `via:"voice-tool"`, `tool`, args).

1. **Tool calls are invisible over-the-air.** `voice_tools.c` and the
   `voice_peer.c` dispatch block log via `LOGI_DIAG` = `ESP_LOGI` +
   `voice_diag_log` (the **local** 32 KB LittleFS ring, USB-only). There
   is **no `device_diag_event`** in the tool path — so `device_logs` has
   `speech_started` and `session end` but never a tool name, args, or
   result. (Contrast `voice_peer.c:638`, which already ships
   `speech_started` to `device_diag` — the pattern is right there.)

2. **"Too far" is a misread of the readiness gate, not a distance rule.**
   `move_to_location` (`c15r/mochi:backend/voice-tools.ts` `runMoveToLocation`)
   has **no geometry** — no distance, adjacency, or teleport. Its only
   rejections are: missing id, `asleep` ("can't move while asleep"),
   place-not-found, **status not `ready`/`revising`**
   (`"<name> isn't quite a place i can reach yet"`), and already-there.
   The "too far" the tester heard was almost certainly the *not-ready*
   gate (place still generating); the later "teleport" was the same move
   succeeding once `ready`. A real distance/adjacency model would be
   net-new substrate — see Non-goals.

3. **Care wasn't acknowledged because the model never learns it happened.**
   The device injects **no** pet-state into the session and, crucially,
   does **not** push a note when the human taps a care zone mid-session.
   The legacy web client *does* (`frontend/voice-realtime.ts`
   `notifyCare` → a system item `"[from your body] … acknowledge briefly
   in your own voice"` + `response.create`). On the device the model is
   blind to the tap, so `acknowledge_care` never fires. Today's `events`
   confirm: fed/comforted/cheered rows with no `via:"voice-tool"` and no
   `acknowledge_care` call.

4. **imagine / costume didn't fire.** No `imagine` device_logs, no
   `cost_events`, no new `costumes` for the test window. `imagine_place`
   *is* fully wired on-device (queue → orchestration → gpt-image →
   upload → ready; the "v0 stub" comment in `voice_tools.c` is stale).
   `imagine_costume`/`wear_costume`/`take_off_costume` are **not** wired:
   the device tool-spec route ships an empty costume enum, there's no
   costume `/orchestration` endpoint, and the device can't render a
   costume sheet-swap yet.

5. **Device voice sessions log 0 tokens / $0.** `voice_peer.c:647` parses
   `response.usage.{input,output,total}_tokens` from `response.done`, but
   every device `realtime_sessions` row is zero — either the field path
   is wrong for `gpt-realtime` or the values aren't reaching the
   `pet_sync` POST / server column. Tracked here, fixed in a follow-up.

### The 12 server tools (ground truth)

From `c15r/mochi:shared/voice-tools-spec.ts buildVoiceToolSpecs`:
`settle_to_sleep`, `wake`, `request_care`, `receive_care`,
`note_about_human`, `set_internal_observation`, `move_to_location`,
`imagine_place`, `acknowledge_care`, `imagine_costume`, `wear_costume`,
`take_off_costume`. No `travel_to`. Dispatcher: `runVoiceTool`
(`backend/voice-tools.ts`); every handler writes an `events` row with
`via:"voice-tool"`. `imagine_place` on device is intercepted locally
(`voice_tools.c:223`); the rest POST `/api/voice/tool`.

## Goals / non-goals

**Goals**
- Make every tool call + result (incl. rejections) queryable from
  `device_logs` without a USB cable.
- Give the model the context the web client already has: care taps and
  place/scene changes pushed into the live session.
- An admin/debug voice mode, toggled on-device, that drops persona and
  narrates tool availability + results — for diagnosing tool use live.
- Wire the tools that exist server-side but not on device (costume
  family), and fix `imagine_place` revise to use the real revise route.

**Non-goals (this iteration)**
- A distance / adjacency / "too far" travel model. It doesn't exist
  today (reachability is the flat set of `ready` places). Whether the
  product wants one is an open question, not built here.
- Device costume *rendering* (sheet-swap) — the blocker for true costume
  support. The server endpoint + device dispatch land here; rendering is
  tracked separately.
- Per-turn `realtime_turns` cost breakdown (design/18 ph3b remainder).

## Plan

### A. Tool-call telemetry → `device_logs` (firmware, this repo)

Emit a curated `device_diag_event` at two points so a field device's
tool behaviour is visible OTA:

- **Dispatch** (`voice_peer.c`, `response.function_call_arguments.done`,
  ~`:742`): `device_diag_event(DIAG_INFO, "voice", "tool call", ctx)`
  with `{tool, args_len}` (args value omitted — may contain free text).
- **Result** (`voice_tools.c send_function_call_output`, `:195`):
  `device_diag_event(level, "voice", "tool result", ctx)` with
  `{tool, ok, reason}` — `reason` is what makes "too far"/refusals
  visible. Carry the tool `name` into `send_function_call_output` (it
  currently only has `call_id` + result) so the row is self-describing.

Keep it curated (one call + one result per tool); both already mirror to
`voice_diag` for the USB dump.

### B. Care + environment injection (firmware, this repo)

Mirror the web client's `notifyCare` / `notifyEnvironment`:

- New `voice::send_note(text)` → `voice_peer_inject_note(text)`: like
  `voice_peer_send_text` but **role `system`**, and a `response.cancel`
  before `response.create` when phase == SPEAKING (interrupt, as the web
  client does). Keep `send_text` (user role) for other callers.
- **Care**: at `main.cpp:3092`, after `pet_sync_enqueue(kind,…)`, if
  `voice::is_active()` and `kind` maps to a care note, inject
  `"[from your body] your human just <…>. acknowledge briefly in your
  own voice."` Mapping mirrors the web `CARE_INJECTION_NOTES`:
  fed/played/comforted/cheered(+hugged)→text; tapped/auto→skip.
- **Place change**: where the travel block swaps the rendered place
  (the `last_location` change path in the main loop), if voice is active
  inject `"[notice] you're now at <place>."` so the model knows it moved.
- **Imagine done**: when `imagine.c` reaches `IMAGINE_DONE`, if voice is
  active inject `"[notice] the place/costume you imagined is ready."`
  (the web `notifyEnvironment` analogue).

### C. Admin / debug voice persona (firmware + `c15r/mochi`)

- **Device**: a boolean in `model_prefs` (`voice_debug`, NVS-backed,
  default off), a value-pill toggle on the dev_menu `ModelsModal`
  (same UI as the model cyclers), and `voice.cpp fetch_session_config`
  appends `?mode=debug` to `/api/voice/session` when set (mirrors the
  existing `?asleep` param plumbing).
- **Server** (`c15r/mochi`): `buildDebugPrompt()` in `shared/persona.ts`
  and a branch in `backend/voice-session.ts` (after the `asleep`
  resolution, ~`:49`): `mode==="debug"` → debug persona. The debug
  persona drops character, on request enumerates its available tools,
  and after each tool call **says the tool name + the `{ok, reason}` it
  got back** — turning live testing into a spoken trace. Tools array is
  unchanged (we want to exercise the real set).

### D. Server-side reject telemetry (`c15r/mochi`)

`runVoiceTool` only logs an `events` row on the mutating/success path;
refusals (`{ok:false, reason}`) vanish. Add a refusal `events` row
(`via:"voice-tool"`, `tool`, `ok:false`, `reason`) so "too far"/"tummy
full"/"can't move while asleep" are queryable — the server twin of A.

### E. imagine_place revise + costume wiring (firmware + both vals)

- **imagine_place revise** (firmware): the device maps the tool's
  `revising` arg into `from_place_id` (treats revise as a new birth).
  Call the real `POST /api/places/:id/revise` instead, resolving the
  place by name as the server's `findRevisablePlaceByName` does. Retire
  the dead `IMAGINE_FETCHING_PACK`/`IMAGINE_SWAPPING` enum states.
- **imagine_costume** (server + firmware, partial): add a costume
  `/orchestration` endpoint in `c15r/mochi:backend/costumes-device.ts`
  mirroring `places-device.ts` (prompt from `shared/costumes-spec.ts`,
  exemplar = base species sheet, upload/ready/failed urls); stop sending
  an empty costume enum in `voice-tools-spec-route.ts`; dispatch
  `imagine_costume`/`wear_costume`/`take_off_costume` in `voice_tools.c`.
  **Device costume rendering (sheet-swap) is deferred** — until it lands,
  the costume generates + records server-side but won't visibly change
  the pet. Flag clearly so it's not read as a regression.

## Telemetry to add (summary)

| tag | msg | ctx | where |
|-----|-----|-----|-------|
| `voice` | `tool call` | `{tool, args_len}` | `voice_peer.c` dispatch |
| `voice` | `tool result` | `{tool, ok, reason}` | `voice_tools.c` result |
| (server `events`) | — | `{via:"voice-tool", tool, ok:false, reason}` | `runVoiceTool` refusal |

Query shape: `SELECT … FROM device_logs WHERE tag='voice' AND msg LIKE
'tool%'` joins the spoken/transcript context already captured.

## Files

Firmware: `voice/voice_peer.c` (dispatch telemetry, `inject_note`),
`voice/voice_tools.c` (result telemetry, costume dispatch, revise),
`voice.cpp` (`?mode=debug`, `send_note`), `voice.h`, `main.cpp` (care +
place-change injection), `imagine.c`/`imagine.h` (revise, done-notice,
enum cleanup), `model_prefs.{c,h}` (voice_debug), `dev_menu.cpp`
(ModelsModal toggle).

Server `c15r/mochi`: `shared/persona.ts` (`buildDebugPrompt`),
`backend/voice-session.ts` (`?mode=debug`), `backend/voice-tools.ts`
(refusal events), `backend/voice-tools-spec-route.ts` (costume enum),
new `backend/costumes-device.ts` (costume orchestration).

## Implementation status (2026-05-29)

- **A — tool telemetry**: shipped (firmware). `voice` tag `tool call`
  {tool,args_len} + `tool result` {tool,ok,reason} in `device_logs`.
  Build green.
- **B — care + environment injection**: shipped (firmware).
  `voice_peer_inject_note` (system role + cancel-when-speaking), care
  note on care taps during a session, imagine-ready notice. Build green.
- **C — admin debug persona**: shipped end-to-end. Firmware: `model_prefs`
  `voice_debug` + dev_menu "Debug" pill + `?mode=debug`. Server
  (`c15r/mochi`, deployed + verified): `buildDebugPrompt` +
  `voice-session.ts` branch. Normal path unchanged (verified 200).
- **D — server reject telemetry**: shipped + verified (`c15r/mochi`).
  Central refusal `events` row in `runVoiceTool` — confirmed
  `move_to_location` "don't know that place" now logs.
- **E — imagine revise + costume**: *partial / scoped*. The stale
  "v0 stub" comment is corrected (`imagine_place` runs the full device
  pipeline). Remaining, deliberately not rushed because both need work
  that can't be hardware-validated in this environment:
  - **True imagine_place revise**: today the device passes `revising`
    (a place *name*) as `from_place_id`, so a revise behaves as a fresh
    birth styled after the old place. A true in-place revise needs a
    device name→id resolve + the `POST /api/places/:id/revise` →
    orchestration flow on-device (the local intercept currently bypasses
    the server's `runImaginePlace` revise branch).
  - **imagine_costume / wear / take_off on device**: server substrate is
    complete, but there is (a) no costume `/orchestration` endpoint
    (needs a new `backend/costumes-device.ts` mirroring `places-device.ts`),
    and (b) **no device costume sheet-swap rendering** — the hard
    blocker. Until rendering lands, the device tool-spec must keep the
    empty costume enum (`voice-tools-spec-route.ts`) so the model isn't
    offered a tool whose effect can't be shown. Wiring this is a
    self-contained follow-up that wants on-hardware validation.

## Open questions

1. Distance / "too far" travel model — is a place graph wanted, or is
   the flat ready-set fine? (Today's behaviour is the latter.)
2. Debug persona: drop character entirely, or a thin "diagnostic mode"
   over the normal persona? Lean drop-character for unambiguous traces.
3. Care-note role: `system` (web parity) vs `user`. Using `system` here.
