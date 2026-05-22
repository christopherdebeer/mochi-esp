# 14 — MPK1 edges + per-zone actions (sketch)

**Status:** sketch, 2026-05-22. Update 2026-05-22: the `format=1` reader
(`mochi_pack.h` — format guard + entry-offset directory +
`mpk_zone_count`/`mpk_zone_get`/`mpk_zone_hit` + talk_seed label table),
the val producer (`c15r/mochi-device` `packMpkV1`), and host + validator
conformance have all landed and pass. SPRITE·FORGE authoring of zones and
consumer rewiring (deleting the name→action `strcmp` switch) remain.
Triggered by the
first real consumer of an MPK1 pack landing
([13-build-time-asset-packs.md](./13-build-time-asset-packs.md)) and
hitting the obvious next gap: zone names alone don't carry intent.

**Predecessors:** [13](./13-build-time-asset-packs.md) (the format
this extends), [06-scene-contracts.md](./06-scene-contracts.md) (the
server-side contract this complements),
[12-thought-bubble.md](./12-thought-bubble.md) (the per-action
payload pattern this reuses).

## What's broken today

`scenes_a` ships 21 named tap zones across 4 sprites. The firmware
hardcodes a switch from name → action:

```c
if      (strcmp(name, "food") == 0)     kind = EVENT_FED;
else if (strcmp(name, "heart") == 0)    kind = EVENT_COMFORTED;
else if (strcmp(name, "door") == 0)     scene_pack_advance(+1);
…
```

Three things go wrong fast:

1. **The pack is the world but the firmware decides what the world
   does.** A new artist authoring a scene with a `bowl` zone changes
   the `.mpk` and `_meta.h`, but the firmware switch doesn't know
   `bowl` so the tap silently routes to `EVENT_TAPPED`.
2. **Navigation is positional, not authored.** `door` always means
   `+1`. A shop scene whose door should jump to `home` (index 7), or
   an attic stairs that goes back to `living-room` (index 1), can't
   be expressed.
3. **Talk-seeds and richer actions can't be authored at all.** A
   `window` zone that should seed `"what do you see out there?"` has
   no place for the seed text in the pack — it would have to live in
   firmware too.

The fix is to give zones a **typed action payload** authored in
SPRITE·FORGE and serialised into the pack.

## Proposal: MPK1 `format` byte 1 — zones with actions

Today's pack uses `format = 0` (cells only; zones live in `_meta.h`
as compile-time C). Add `format = 1`: cells **plus** an inline zone
trailer per entry. Old readers that only understand `format = 0`
return -2 ("unsupported version") on a `format = 1` pack and ignore
it gracefully — same downgrade path as today's
"unsupported version" handling. New readers handle both: a `format =
0` pack reads cells exactly as today and zones still come from
`_meta.h`; a `format = 1` pack reads zones from the binary instead.

### Wire format (extension)

The 16-byte envelope is unchanged. `data[5] == 1` means each entry
has the existing cell blob followed by a zone trailer:

```
entry layout (format = 1):
  label       label_len bytes, NUL-padded UTF-8
  cell blob   8 + (has_mask ? 2 : 1) * plane_bytes  (unchanged)
  zone count  u8                                    (0..15)
  zones       count × 24 bytes:
    x, y, w, h    u16 LE × 4   (8 bytes, cell-local pixels)
    action_kind   u8           (1 byte, see table)
    action_data   u8           (1 byte, kind-dependent)
    label_idx     u16 LE       (2 bytes, see "labels" below)
    reserved      u8 × 12      (zero today; kind-extension space)
```

Stride-per-entry is no longer constant: it's
`label_len + cell_bytes + 1 + zone_count * 24`. That kills the O(1)
`mpk_cell(i) = base + i*stride` math — a `format = 1` pack needs an
**offset table**. Cheapest place for it: a 2-byte directory between
the envelope and the entries.

Two ways to add the directory:

- **(a) Always present in `format = 1`.** Header carries
  `entries_offset` and a count-sized `u32 LE` array of entry offsets.
  16 bytes envelope + 4 × count directory + entries.
- **(b) Build at open time.** Reader walks the entries once during
  `mpk_open`, populates an offsets array on the heap. Cheap (tens of
  entries × tens of bytes), no on-disk format change beyond the
  trailer. Costs `O(N)` startup work and a small heap allocation.

I lean (a). Wire-format directory matters because:

- The pack is *embedded*, so the directory lives in flash, not heap.
  Open-time walk would build a heap copy of data that's already
  immutably present in `.rodata` — wasteful.
- Validation: a corrupt entry length cascades through the rest of
  the pack in (b); the directory in (a) bounds the damage to one
  cell.
- Future formats can keep the same directory shape; `mpk_open`
  becomes uniform across `format` values that need it.

### Action kinds

Start with five and reserve the rest:

| `action_kind` | name              | `action_data` semantics                          |
| ---:          | :---              | :---                                             |
| 0             | `none`            | (gutter / decorative; never dispatched)          |
| 1             | `event`           | `event_kind_t` value (FED/COMFORTED/PLAYED/…)    |
| 2             | `nav_scene`       | absolute scene index (`u8`, packs cap at 256)    |
| 3             | `nav_relative`    | signed delta (`i8`, e.g. +1 for `door`)          |
| 4             | `talk_seed`       | (see "labels" — `label_idx` points to the seed)  |
| 5..255        | reserved          |                                                  |

Reasons for the shape:

- `event` and `event_kind_t` are 1:1 with what `voice_tools.c` and
  `event_log.c` already speak. No translation layer.
- Two flavours of nav (absolute + relative) because authors will
  want both: a `door` zone that's *always* +1 in a corridor pack, and
  a `door` zone that goes *to* the bedroom regardless of where it's
  authored.
- `talk_seed` separates the *trigger* from the *content* — the seed
  text is variable-length and shouldn't bloat every zone. Hence
  labels.

### Labels (for variable-length payloads)

A pack-global label table at the end:

```
labels:
  count  u16 LE
  for each label:
    len    u8
    bytes  len bytes, UTF-8
```

`talk_seed.label_idx` indexes into this table. Keeping labels
deduplicated and pack-global means a 50-character seed shared across
12 scenes costs 50 bytes once, not 600.

The author-time tooling (SPRITE·FORGE) is responsible for
deduplication. The reader treats the table as immutable.

## Reader API additions

`mochi_pack.h` grows three accessors that work uniformly across
`format` values:

```c
typedef enum {
    MPK_ACTION_NONE         = 0,
    MPK_ACTION_EVENT        = 1,
    MPK_ACTION_NAV_SCENE    = 2,
    MPK_ACTION_NAV_RELATIVE = 3,
    MPK_ACTION_TALK_SEED    = 4,
} mpk_action_kind_t;

typedef struct {
    uint16_t          x, y, w, h;
    const char       *name;
    mpk_action_kind_t kind;
    uint8_t           data;       /* kind-dependent */
    const char       *seed_text;  /* set iff kind == TALK_SEED */
} mpk_zone_v1_t;

/* Returns the number of zones for sprite i (0 in format=0 packs). */
uint8_t mpk_zone_count(const mpk_t *p, uint16_t i);

/* Fills *out with zone z of sprite i. Both format=0 and format=1
 * packs work — format=0 reads from a *_meta.h zone array supplied
 * by the caller; format=1 reads inline. */
bool mpk_zone_get(const mpk_t *p, uint16_t i, uint8_t z,
                  mpk_zone_v1_t *out);
```

Calling code (`scene_pack.c`) collapses to one path. The
`SCENES_A_ZONES` table from `_meta.h` becomes redundant for
`format = 1` packs — `mpk_zone_get` reads everything it needs from
the binary. We keep `_meta.h` exports only as a back-compat path for
`format = 0`.

## Authoring (SPRITE·FORGE)

The web tool already has a zones panel per sprite. Three additions:

1. **Action picker per zone.** Dropdown for `kind`; an inline
   `data` editor that morphs by kind (number for nav, dropdown of
   `event_kind_t` names for event, multi-line text for talk-seed).
2. **Pack-level scene index map.** A list of named scenes (so an
   author can name sprite 7 "bedroom" and select that as `nav_scene`'s
   `data` rather than typing `7`).
3. **Label-table emit.** Talk-seeds and any future variable-length
   payload feed into one table; the tool dedupes.

The export then writes `format = 1` instead of `format = 0`, plus
the entry directory and labels block.

## Migration

Three checkpoints, each independently shippable:

1. **Reader gains `format = 1` support, `_meta.h` still authoritative
   for `scenes_a`.** SPRITE·FORGE keeps emitting `format = 0`. No
   firmware behaviour change. Validates the parser.
2. **SPRITE·FORGE gains the action picker and emits `format = 1`.**
   Re-export `scenes_a`. The hardcoded `if (strcmp("food", …))`
   switch in `main.cpp` deletes; `mpk_zone_get` returns
   `(EVENT_FED, …)` directly.
3. **Server scene contracts emit `format = 1` MPK1 too.** The
   precedence chain from `13` (server overrides bundled) now uses
   the same parser end-to-end. The `_meta.h` files stop being needed
   except as test fixtures.

Step 1 is mechanical and safe to land before any artist re-authors.
Step 2 needs SPRITE·FORGE work — a half day. Step 3 is a server
project covered by `06`'s open issues.

## Open questions

- **Cell-mixed sizes.** If `format = 1` is the time to also break
  fixed stride for *cells* (so different zones can carry different
  cell sizes — e.g. a small `bowl` icon and a big `bedroom` scene
  in one pack), do we want that? Probably not yet; one orthogonal
  change at a time.
- **Versioning labels.** A naive `label_idx` is just a number. If
  packs grow to thousands of seeds (unlikely soon), we may want a
  fingerprint or LRU eviction policy for any in-RAM index. Not now.
- **Action vocabulary growth.** `MPK_ACTION_*` is a `u8`. The
  reserved space is generous, but every new kind requires a
  reader update. If we end up with kinds that should be
  device-extension-loadable, the `u8` becomes "category" and
  `data` becomes a 2-byte selector. Not a 2026 problem.
- **Server contract in MPK1 envelope.** Mentioned in `13`'s point 4;
  this doc takes the orthogonal axis (richer zones), not the
  delivery one (server-supplied vs bundled). Both can land
  independently — `format = 1` is about *what's in a pack*, the
  server-as-source idea is about *where a pack comes from*.

## Cross-references

- `13-build-time-asset-packs.md` — the format this extends
- `06-scene-contracts.md` — server-side analogue
- `12-thought-bubble.md` — typed-action precedent
- `firmware/main/mochi_pack.h` — the reader to extend
- `site/tools/sprite-forge.html` — the authoring tool to extend
