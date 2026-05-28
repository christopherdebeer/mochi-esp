# 25 — Review & progress: tap hit-test, render responsiveness, cleanup

Status: in review, 2026-05-28
Branch: `claude/pet-tap-hittest-render-blocking-MtJIX`
Base: `3de360f` (merge of #18)

This doc captures the work done on this branch and the backlog it
surfaced, for review. Two commits land behavioral fixes + a low-risk
cleanup pass; a three-axis code review produced a prioritized backlog,
most of which is intentionally **not** yet implemented (see §4).

All firmware here builds green against ESP-IDF v5.3
(`idf.py build`); the MPK1 host test compiles clean in C11 + C++17.
Nothing here has been validated on real hardware yet — see §5.

---

## 1. What shipped (committed)

### Commit `3799016` — tap hit-test + post-tap hold

Two behaviors the owner flagged as undesired.

**1a. Pet tap was triggering overlapping zone actions.**
Pet sprite packs are the 96×96 two-plane (ink + mask) format
(`design/05`); the mask marks opaque body pixels (`0`) vs transparent
background (`1`). The tap hit-test used the pet's *96×96 bounding box*,
and the overlap policy let authored scene zones (food/heart/play/door,
which the pet sits over in `scenes_a`) win over that box — so a tap on
the pet resolved as the zone behind it.

- Fix: `pet_silhouette_hit()` consults the already-loaded `pet_mask`
  (the pet always blits at `PET_DX,PET_DY`). A tap counts as a pet pat
  only on an *opaque* drawn pixel, and now takes priority over an
  overlapping zone (the scene-zone hit-test is skipped for a silhouette
  hit → mood bubble + `EVENT_COMFORTED`, never a stray zone action).
- Margin taps still fall through to the zone, so a finger genuinely on
  the bowl that happens to be inside the pet's bbox still feeds.
- Files: `firmware/main/main.cpp` (`pet_silhouette_hit` lambda; gated
  `scene_hit` on `!tapped_pet`; rewrote the overlap-policy comment).

**1b. The ~5 s post-tap expression hold swallowed input.**
The hold loop only polled the PWR-sleep gesture, never touch, so taps
in that 5 s window were ignored — the panel felt dead between care taps.

- Fix: the hold now also polls for a fresh finger-down (gated on a prior
  release via `saw_release`, so a finger still resting from the starting
  tap can't re-trigger) and cuts the hold short; the new press's queued
  ISR marker is serviced by `wait_event` on the next loop iteration.
  PWR-tap-to-sleep mid-hold is unchanged.

### Commit `9dce004` — cleanup pass (review-driven)

Low-risk only. See §3 for the full list. Net `+30 / -257` lines.

---

## 2. Review method

Three parallel review agents, scoped MECE (non-overlapping concerns,
collectively covering the request):

1. **Dead + duplicate code** (also the DRY pass)
2. **Memory safety** (bounds/overflow, NULL, use-after-scope, concurrency)
3. **Usability + responsiveness** (dropped/stale input, blocking ops on
   the touch loop, refresh flicker, feedback)

Each returned findings with `file:line`, severity, confidence, and a
fix-effort estimate. Findings below are deduplicated and each is owned
by exactly one category.

---

## 3. Implemented in the cleanup pass

### Dead code (zero call sites, verified across `firmware/` + `test/`)
- `epd_ui`: `render_online`; the M4 prompt trio (`render_prompt_fetch`,
  `render_fetching`, `render_fetch_failed`); `overlay_fetch_status`;
  `overlay_boot_version`; `draw_dot`; the unused public `draw_qr`
  (`draw_qr_centered` does its own encoding) — defs + header decls.
- `compositor`: `copy_full`, `blit_mask` (only `blit_two_plane` is
  live); fixed the one `main.cpp` comment that referenced `blit_mask`.
- `dev_menu`: the never-wired `is_modal` helper.
- `main.cpp`: stale `TRAVEL_PACK_BYTES` / `MOCHI_BASE_URL` macros (the
  320 KB travel buffer they described was removed in v0.1.7).

### Memory safety
- `mpk_open` now sanity-bounds wire-supplied `cell_w`/`cell_h`/`count`
  (`MPK_MAX_DIM 256` / `MPK_MAX_COUNT 1024`, new rc `-4`) before any
  size math — kills the `cell_bytes` uint32 overflow for absurd
  dimensions and bounds every downstream `mpk_ink/mpk_mask/mpk_cell`
  read on the network-pack paths.
- `persona_body`: NULL/empty guard on `resp->data` before `memcpy`,
  matching the existing guard in `imagine.c`'s `on_capture`.

### DRY
- `sprite_fetch_cell` reuses `fetch_ctx` + `on_event` instead of the
  byte-identical `cell_ctx` / `on_cell_event` pair.

---

## 4. Backlog (NOT implemented — needs decision or is higher-risk)

Ordered by user-felt impact. Effort = fix effort; all are reviewer
estimates.

### C — Responsiveness (highest impact; share one root cause)
The main loop is the single owner of touch + panel. Anything multi-second
it calls inline freezes ALL input for that long. The fixed 5 s hold was
one instance; these are the others:

| # | Symptom | Sev | Effort |
|---|---------|-----|--------|
| C1 | Travel place-pack fetch (`pack_cache_active_geom`, HEAD 8 s + GET 15 s) blocks the loop ~8–23 s on a "go to X" tap — no tap/PWR/voice for that whole window | high | medium |
| C2 | Per-tap care action that misses pack+cache blocks ~10 s on `sprite_fetch_cell`, or silently drops on failure | high | medium |
| C3 | Synchronous `pet_sync_pull_now` right after a voice session ends freezes ~15 s at the worst moment (`main.cpp:2452`) | high | small–medium |
| C4 | Full-panel refresh flash (~1 s) on every travel swap; scene-nav already uses the partial/full-every-4 hybrid, travel doesn't | medium | small |
| C5 | 1 s idle tick + 10 Hz touch poll → up to ~1 s tap latency on the poll-only path (mitigated if GPIO21→INT is wired); power/latency tradeoff | medium | small |
| C6 | Render-fetch failure at the dispatch site is a silent no-op (no "can't reach mochi" feedback, unlike the travel-fail path) | low | small |

**Recommendation:** do C1–C3 as one focused change applying the
cache-first / async-worker model `design/21` already mandates (immediate
acknowledgement frame, fetch on a worker, swap on a flag), rather than
piecemeal. C4/C6 fold in naturally; C5 is a separate tuning call.

### B — Memory safety (remaining)
| # | Item | Sev | Effort | Note |
|---|------|-----|--------|------|
| B1 | Full format=1 directory/length validation in `mpk_open` against the fetched blob length (the geometry clamp shipped; the directory-walk wild-read on a truncated/hostile format=1 pack remains). Needs a length-checked `mpk_open` threaded through callers. | high | medium | network packs gated by `looks_like_mpk1()` in the meantime |
| B2 | `voice_peer.c pc_on_data` treats the data-channel frame as a NUL-terminated C string (`strstr`/`strchr`/`cJSON_Parse`) | medium | small | **declined to patch blind** — appending a terminator risks an OOB *write* on an esp_peer-owned buffer; confidence on its termination guarantees is medium and the path is untestable here |
| B3 | Lazy `if (!s_mtx) s_mtx = xSemaphoreCreateMutex()` in `pet_sync`; latent double-create if init order ever changes (currently safe — first call is on the main task pre-worker) | low | trivial | eager init in a `pet_sync_init()` |
| B4 | Confirm `imagine` worker frees the queued request on every terminal branch | low | small | no concrete per-request leak found |

### A — DRY (remaining; medium-effort refactors)
| # | Item | Effort |
|---|------|--------|
| A1 | `esp_http_client_config_t` setup boilerplate repeated ~14× → `mochi_http_init(url, method, timeout, handler, user_data)` | medium |
| A2 | 1bpp glyph-blit loop reimplemented 5× (`ui_dialog`, `thought` ×2, `main.cpp` ×2; two use a hardcoded stride `25`) → shared `fb_blit_text` / `fb1bpp.h` | medium |
| A3 | NVS single-namespace string CRUD near-identical in `openai_key.cpp` + `pair_creds.cpp` → shared `nvs_str_load/save/erase` | small |

### Checked and sound (no action)
Compositor blits bounds-check the destination; `sprite_fetch_cell`
header/length validation; `scene_pack_blit_current` dims; network
accumulators' overflow flags; Opus decode cap; `thought.cpp` text
wrapping + pointer lifetimes; `epd_ui` splash size check.

---

## 5. Verification status

- ✅ `idf.py build` green at HEAD (esp32s3, ESP-IDF v5.3).
- ✅ MPK1 host test compiles clean (C11 + C++17). Cannot *run* it here —
  the `fmt1.bin` fixture lives in `c15r/mochi-device`.
- ❌ Not flashed / not validated on hardware. The tap and hold changes
  are touch-loop behavior best confirmed on a real panel + FT6336.

---

## 6. Open decisions for the owner

1. Take on the C1–C3 responsiveness change next (one async/cache-first
   pass)? Highest user-felt impact and a direct extension of this branch.
2. B1 (format=1 length validation) — do now as security hardening, or
   defer while `looks_like_mpk1()` gating holds?
3. C5 poll-rate / idle-tick tuning — acceptable power cost to cut tap
   latency, or leave as-is and rely on the INT line?
4. Open a PR for what's on the branch now, or keep iterating first?
