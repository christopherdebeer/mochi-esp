# 16 — on-device imagine (sketch)

**Status:** v0 implemented, 2026-05-22. **Server half landed + verified
live** (see "Server changes — as built"). **Firmware pipeline landed**
(`imagine.c` fleshed out from scaffold; builds clean idf v5.3, shipped in
release `v0.0.17`) — pending on-device validation (needs hardware + BYO
key + a real generation spend). One polish item remains: the
re-render-on-DONE wire in `main.cpp` (step 7 below).

> **Update 2026-05-22 — reconciliation.** The first pass of this doc
> assumed the imagine flow lived in `backend/devsprite-dashboard.ts` and
> generated at 1024×1536. Both wrong. The real flow is
> `frontend/places-client.ts` `generatePlace()`, and it generates at
> **1504×720** (2× the canonical 752×360 day/night scene sheet). There
> is **no per-place guide builder** — every place reuses the canonical
> `scene-v1` grid guide, rasterised client-side. `GET /api/places/:id`
> never existed. The sections below are corrected to the as-built code.

**Predecessors:** [05](./05-sprite-format.md) (the cell wire format
that bundled and generated cells share), [06](./06-scene-contracts.md)
(server-side scene definitions this layers on),
[07](./07-voice-architecture.md) (BYO key + voice tools — the trigger
surface), [13](./13-build-time-asset-packs.md) (MPK1 — the artefact
this produces), [14](./14-mpk1-edges-and-actions.md) (zone authoring,
relevant when generated scenes carry zones).

## What we're after

Today the device plays back assets that were authored on a desktop
(SPRITE·FORGE in a browser, then dropped into `firmware/main/assets/`
at build time, then flashed). That makes the toy *finished* the moment
it ships.

The web side already supports speaking a place into existence — the
val.run dashboard's "imagine_place" flow takes a seed name + vibe,
calls `gpt-image-2` via `POST /v1/images/edits` with a layout guide
and a style exemplar, lands a 1024×1536 sheet, runs it through
chroma-key + cell extraction, and stores it as a new place the pet
can travel to.

The question this doc answers: **can the device do that flow itself,
without the web app, while keeping the OpenAI key on-device?**

Yes. The architecture:

```
┌──────────────────────┐  voice tool  ┌──────────────────┐
│  user: "imagine a    │──────────────│ val.run records  │
│  beach at sunset"    │              │ pending place    │
└──────────────────────┘              └──────────────────┘
                                              │
                                  guide + exemplar URLs returned
                                              ▼
┌─────────────────────────────────────────────────────────┐
│                       device                            │
│                                                         │
│  fetch guide.png      api.openai.com/v1/images/edits    │
│  fetch exemplar.png ──▶  multipart: prompt+guide+ex    │
│       │                       │                         │
│       │                       ▼                         │
│       │             1024×1536 sheet (b64_json)          │
│       │                       │                         │
│       └──────POST /sheets/:id/png ─────────▶ val.run    │
│                                                  │      │
│                              extracts + builds   │      │
│                              .mpk on demand      │      │
│                                                  ▼      │
│  download GET /api/places/:id/pack.mpk                  │
│  write to littlefs:/imagined/<id>.mpk                   │
│  swap scene_pack source → re-render                     │
└─────────────────────────────────────────────────────────┘
```

The key trick: the *generation call* goes device → OpenAI. The user's
key never leaves the device. val.run sees the resulting PNG (it has
to — it does the binarisation) but never the key. Same separation
pattern we already have for voice (audio + key stay device-side; only
text crosses to val.run).

## Why the device, not the web side

Two reasons that aren't "because we can":

1. **Keys must stay device-side.** That's the whole shape of the
   product — BYO key, kept in NVS, never round-tripped through
   val.run. The voice path proves this works; image gen is the same
   shape (POST to api.openai.com with `Authorization: Bearer ${key}`).
   Routing image gen through val.run would either require uploading
   the key (bad) or running val.run-side keys (a cost we aren't
   absorbing). Device-side gen is the correct architecture.

2. **The toy should be self-contained.** "Mochi, can you imagine a
   sunny meadow?" → 30 seconds of mochi-thinks → here's a meadow,
   *spoken into existence on the toy itself*. That's the experience.
   Today it requires opening a desktop browser and clicking buttons
   in a dashboard.

## Why val.run handles post-processing

The OpenAI response is a 1024×1536 PNG of the entire sheet. To turn
that into device cells we need:

- Chroma key (mask the magenta/green fill).
- Optional paper-stroke halo (1-px paper ring).
- Threshold + dither (line-screen, error diffusion) — what
  `applyEffect` does in SPRITE·FORGE.
- Per-cell extract (cropping by the layout guide).
- MPK1 packaging (envelope + per-entry header + ink/mask planes).

That pipeline is **already implemented** in val.run (`backend/sheets.ts`
+ `backend/devsprite-encode.ts`), in TypeScript, with all the edge
cases that broke and got fixed. Reimplementing it in C on the ESP32
would take ~1k LoC and a PNG decoder; we'd inevitably miss something
the TS version handles.

The flash budget also pushes this way:

| stage         | size                    | where           |
| :---          | :---                    | :---            |
| guide.png     | ~1 KB                   | downloaded      |
| exemplar.png  | ~80 KB                  | downloaded      |
| openai resp   | ~600 KB (b64) → 450 KB  | held in PSRAM   |
| posted to val | 450 KB raw PNG          | upload          |
| .mpk built    | ~40–160 KB              | downloaded back |

The intermediate 450 KB PNG never has to touch flash. The device
**only persists the .mpk**, which is the artefact the boot path
already understands.

## Voice-tool surface

Add three tools to the existing `voice_tools` dispatch. All return
quickly with structured ok/reason; the actual work is async on a
worker (the same shape as today's `care_direct` tool).

```jsonc
{
  "name": "imagine_place",
  "description": "Speak a new place into existence. Mochi will paint it during the wait.",
  "parameters": {
    "type": "object",
    "properties": {
      "seed_name": { "type": "string", "minLength": 2,  "maxLength": 80  },
      "seed_vibe": { "type": "string", "minLength": 3,  "maxLength": 400 },
      "from_place_id": { "type": "string" }   // optional style exemplar
    },
    "required": ["seed_name", "seed_vibe"]
  }
}
```

```jsonc
{
  "name": "revise_place",
  "description": "Re-imagine the current place with a new vibe.",
  "parameters": {
    "type": "object",
    "properties": {
      "seed_vibe": { "type": "string", "minLength": 3, "maxLength": 400 }
    },
    "required": ["seed_vibe"]
  }
}
```

```jsonc
{
  "name": "travel_to",
  "description": "Switch to an already-imagined place by name.",
  "parameters": {
    "type": "object",
    "properties": {
      "name": { "type": "string", "minLength": 2, "maxLength": 80 }
    },
    "required": ["name"]
  }
}
```

The first two return `{ "ok": true, "place_id": "...", "eta_s": 30 }`
and kick off the imagine pipeline. `travel_to` is synchronous — it
swaps the active scene_pack source.

## Imagine pipeline (device-side)

`firmware/main/imagine.{h,c}` exposes one entry point:

```c
typedef struct {
    char     place_id[40];
    char     seed_name[80];
    char     seed_vibe[400];
    char     from_place_id[40];   /* optional, "" = let server pick */
} imagine_req_t;

bool imagine_start(const imagine_req_t *req);
bool imagine_in_flight(void);
imagine_phase_t imagine_phase(void);   /* QUEUEING/FETCH/GEN/UPLOAD/PACK/DONE/FAILED */
```

A single worker task handles one request at a time (bounded resources;
one OpenAI call costs ~25 s and ~600 KB of PSRAM staging — we don't
want to overlap them).

The pipeline (as-built server contract):

1. **Queue** — POST `/api/places/queue` (`X-Pet-Id` header) with
   `seedName` / `seedVibe` / optional `fromPlaceId`. Replies
   `{ ok, placeId }`.

2. **Orchestration** — `GET /api/places/:id/orchestration`. One call
   returns everything: `prompt` (server builds the versioned style
   preamble — firmware never hardcodes it), `gen_w`/`gen_h`
   (1504×720), `exemplar_sheet_id`, `sheet_id`, and the relative
   URLs (`guide_url`, `exemplar_url`, `upload_url`, `pack_url`,
   `ready_url`, `failed_url`). The device prefixes its
   `mochi.val.run` base and follows them — no URL construction in C.

3. **Fetch references** — GET `guide_url`
   (`/sheets/scene-v1/guide.png`, ~12 KB, server-rasterised) and
   `exemplar_url` (`/sheets/<exemplar>/source.png`, ~80 KB) into
   PSRAM.

4. **Generate** — multipart POST to
   `https://api.openai.com/v1/images/edits` with `model=gpt-image-2`,
   `image[]=guide.png` (first), `image[]=exemplar.png` (second),
   `prompt`, `n=1`, `size=1504x720`, `quality=low`, BYO key in
   `Authorization: Bearer`. Response: `{ data: [{ b64_json }] }`.
   Decode the base64 into a PSRAM buffer (~600 KB).

5. **Upload** — POST `upload_url` (`/sheets/<sheet_id>/png`) with
   `Content-Type: image/png` and the raw decoded PNG bytes. The val
   stores `:source`, derives, and re-ETags.

6. **Mark ready** — POST `ready_url`. The val flips the row to ready.

7. **Fetch pack** — GET `pack_url`
   (`/devsprite/pack/<sheet_id>`). The val crops the day/night cells
   server-side and returns the assembled MPK1. This is the **same
   route `pack_cache` already understands**, so the existing
   boot-sync cache machinery (design/15) handles fetch + validate +
   persist; no new on-device download path.

8. **Swap source** — `scene_pack_load_path(littlefs_path)` (new
   accessor) opens the on-disk pack and replaces the active source.
   Re-render.

9. **Cleanup** — failure at any point POSTs `failed_url` with a
   short reason so the row goes terminal.

The first four steps are network-heavy; the rest are local. The model
should hold off on speaking again until step 4 starts; the existing
voice phase machine can read `imagine_phase()` to know whether to
narrate ("painting now…") or stay quiet ("here we are").

## Memory budget

In-flight, worst case:

| buffer                  | bytes    | location |
| :---                    | :---     | :---     |
| guide png               | ~2 KB    | PSRAM    |
| exemplar png            | ~100 KB  | PSRAM    |
| openai b64 response     | ~800 KB  | PSRAM    |
| decoded raw png         | ~600 KB  | PSRAM    |
| upload buffer           | ≤ 600 KB | PSRAM    |
| downloaded .mpk         | ~160 KB  | PSRAM    |

Total peak: ~1.5 MB. We have 8 MB of PSRAM and voice peers takes
~2 MB at idle, ~3 MB during a session. **Imagine + voice may not
overlap.** Either:

- imagine refuses to start while voice session active (cleanest,
  matches "Mochi is talking, not painting yet")
- voice session is paused for the duration of imagine (more
  intrusive)

Sketch picks the first.

## Server changes (val.run) — as built

Landed in `c15r/mochi` and verified live (2026-05-22). The reused
endpoints already existed; the three additions are the device's hooks.

**Reused as-is:**
```
POST /api/places/queue           seedName/seedVibe/origin → { ok, placeId }
POST /api/places/:id/ready       flips pending → ready, runs extraction
POST /api/places/:id/failed      { reason } → terminal
POST /sheets/:id/png             raw image/png OR { base64 } → stores + derives
GET  /sheets/:id/source.png      the exemplar style anchor
```

**Added (this work):**
```
GET /api/places/:id/orchestration   backend/places-device.ts (NEW module,
    mounted /api). Everything the firmware needs in one shot, computed
    server-side so the device stays dumb:
      { ok, place_id, sheet_id, status, gen_w:1504, gen_h:720,
        prompt,                ← buildPlacePrompt (versioned style preamble)
        style_version, exemplar_sheet_id,
        guide_url, exemplar_url, upload_url, pack_url, ready_url, failed_url }

GET /sheets/:id/guide.png           backend/sheets.ts. Server-side SVG→PNG
    of the grid-only guide via @resvg/resvg-wasm, rendered at scale×
    (default 2× → scene-v1 752×360 ⇒ 1504×720) on a #F5F0E6 paper field.
    Replaces the browser's <canvas> rasterise; the device can't do that.

GET /devsprite/pack/:sheet          UNCHANGED route, but devsprite.ts's
    effectiveTemplate now resolves scene-dyn place sheets (it only knew
    static + user sheets before), so the pack/cell routes serve a
    generated place's day/night cells. The firmware fetches the MPK1
    from here (pack_cache already understands /devsprite/pack/<sheet>).
```

So the device never sees `buildPlacePrompt`, the style version, or the
guide geometry — it asks `/orchestration` and follows the URLs. The
key-off-server invariant holds: only the generated PNG crosses to the
val (via `upload_url`); the OpenAI key stays on the device.

## Network etiquette

The OpenAI call costs the user real money. The substrate also
already enforces a daily budget (`day_cap` reason in
`/places/:id/revise`). Two layers of brake:

- **Server-side cap.** Already there. The queue endpoint refuses
  with `429 day_cap` past N generations/day/pet.
- **Device-side debounce.** Refuse to start a new imagine in less
  than 60 s of the previous one — protects against the model
  invoking `imagine_place` in a loop. Probably belongs in the
  voice tool dispatch.

## Failure modes

- **WiFi drops between fetch-guide and OpenAI.** Bail; mark failed.
- **OpenAI returns 429 / 4xx.** Bail; mark failed with the reason
  from the response body.
- **Generation succeeds but upload fails.** Two retries, then bail.
  We've already paid for the gen — losing it is a real cost.
  (Tomorrow's improvement: cache the generated PNG in littlefs
  until upload confirms; replay on next online tick. Out of scope
  for v0.)
- **Upload succeeds, /ready times out.** val.run's worker may
  still finish; on the next imagine_in_flight() tick, poll
  `GET /api/places/:id` for status. Idempotent.
- **Pack fetch fails.** As above; pack is on disk server-side,
  retry-friendly.

## v0 scope — progress

- [x] **`imagine_place` voice-tool spec** — in `shared/voice-tools-spec.ts`
  (`buildVoiceToolSpecs`), served per-pet via `/api/voice/tools`. Carries
  `name` / `vibe` / optional `revising`.
- [x] **On-device dispatch** — `voice/voice_tools.c` intercepts
  `imagine_place` and calls `imagine_start()` (effect is on-device, not a
  val.run round-trip).
- [x] **Server endpoints** — `/api/places/:id/orchestration`,
  `/sheets/:id/guide.png` (resvg-wasm), and dynamic-place resolution in
  `/devsprite/pack/:sheet`. Live + verified. (The original sketch's
  `pack.mpk` endpoint was unnecessary — the existing pack route serves it
  once `effectiveTemplate` resolves place sheets.)
- [x] **Firmware skeleton** — `imagine.{h,c}` phase machine + worker.
- [x] **OpenAI multipart generate** — `imagine.c` `openai_edit()`:
  multipart body in PSRAM, BYO-key Bearer auth, b64 extracted by scan +
  mbedtls (no cJSON on the ~900 KB body). The load-bearing step.
- [x] **Upload / ready / pack-fetch / swap** — `upload_png()`, ready POST,
  `sprite_fetch_blob` of the MPK1, `scene_pack_load_bytes()`.
- [ ] **UX polish** — re-render the panel the moment `imagine_phase()`
  hits DONE (and surface phase text — "painting…", "almost…" — via the
  thought bubble). Today the swap lands but paints on the next render
  tick/tap. The phase enum + accessors are already exposed for this.
- [ ] **On-device validation** — flash, trigger via voice, confirm the
  round-trip and the visible scene swap. Owner: device holder.

Not in v0 (deferred): cross-reboot persistence of the active imagined
place (in-session swap only today), and `imagine_costume` (scene path
first; the costume flow exists server-side but the device doesn't render
costume sprites yet).

## Open questions

- **Pet packs too?** Same flow could regenerate a pet's expression
  pack ("imagine_pet_style"). The art direction is harder (one
  exemplar per expression, not just a vibe), so v0 sticks with
  scenes where the dashboard's flow is already proven.
- **Caching the user's hand-picked place name list.** When a kid
  travels to "the meadow" by voice, we look it up by name. Need a
  device-side index of `name → place_id` synced from val.run.
  Probably piggyback on `pet_sync` substrate.
- **Permissions / kid-safety.** OpenAI's image gen has its own
  refusal model. Refusals come back as 4xx; we surface them as a
  generic "Mochi got distracted" — not "OpenAI refused". Same
  shape as the voice text-back path.
- **Streaming.** Could we stream the b64 response and decode
  incrementally? Probably not worth it at v0; the call is ~25 s
  serialised regardless.

## Cross-references

- `~/dev/mochi/backend/devsprite-dashboard.ts` — the browser-side
  reference implementation of this whole flow (lines 1340–1440 for
  the scene case, 2440–2530 for places).
- `~/dev/mochi/backend/api.ts` — `/places/queue`, `/ready`,
  `/failed`, `/revise` already exist.
- `firmware/main/voice/voice_tools.c` — the dispatch shape new tools
  plug into.
- `firmware/main/sprite_cache.cpp` — littlefs read/write helpers
  the .mpk persistence reuses.
- `firmware/main/scene_pack.{c,h}` — the consumer; needs a
  load-from-path accessor alongside the embedded-blob one.
