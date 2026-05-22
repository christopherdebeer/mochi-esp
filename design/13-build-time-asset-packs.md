# 13 — Build-time asset packs (MPK1) + scene-contract impact

Status: draft, 2026-05-22. Format + web authoring tool landed; firmware
reader (`firmware/main/mochi_pack.h`) landed header-only and not yet
wired into a build. No `.bin` is embedded yet.

**Predecessors:** [05-sprite-format.md](./05-sprite-format.md) (the
device cell wire format this reuses verbatim),
[06-scene-contracts.md](./06-scene-contracts.md) (the server-authored
contract vision this complements).

## Why

Every sprite the device shows today is fetched from `mochi.val.run`
at runtime (`/devsprite/cell/...`, cached in LittleFS via
`sprite_cache`). That is the right default — the server stays the
source of truth, scenes render dynamically, and OTA-free art updates
flow through ETags. But it leaves two gaps:

1. **Offline / first-boot art.** Only the boot splash is bundled
   (`assets/splash.bin`, embedded). Everything else needs network +
   a paired pet before the first cell arrives. A freshly flashed,
   unpaired, or offline device has no pet face, no icons.
2. **No build-time-authored zones.** The corner-icon tap zones are
   *hardcoded* in `main.cpp`. There is no artifact that pairs an
   image with its tap regions that we can author, version, and ship
   in the image — which is exactly what scene contracts will need.

A build-time asset pack closes both. SPRITE·FORGE (the web tool)
authors a `.bin` of device cells plus a companion `_meta.h` of
labels and tap zones; the firmware embeds the `.bin` with the same
`EMBED_FILES` mechanism as the splash and reads it with
`mochi_pack.h`.

## Format: MPK1

A pack is a flat container of fixed-stride entries. The design
constraint that drove every other choice: **each entry's cell blob is
byte-identical to a `/devsprite/cell` response.** A bundled cell and a
fetched cell are the same bytes, so they flow through the same
`compositor::blit_two_plane` call. Nothing downstream branches on
provenance.

```
header (16 bytes, little-endian envelope):
  0..3   magic     "MPK1"
  4      version   1
  5      format    0 = device 2-plane cell entries
  6..7   cell_w    u16 LE
  8..9   cell_h    u16 LE
  10..11 count     u16 LE
  12     label_len u8
  13     flags     bit 0 = entries carry a mask plane
  14..15 reserved  0

entries (count), each `stride` bytes:
  label      label_len bytes, NUL-padded UTF-8
  cell blob  8-byte cell header (w u16 BE, h u16 BE, flags u8,
             3 reserved) + ink plane [+ mask plane]

plane_bytes = ((cell_w + 7) / 8) * cell_h
cell_bytes  = 8 + (has_mask ? 2 : 1) * plane_bytes
stride      = label_len + cell_bytes
```

Conventions inherited from `05-sprite-format.md` unchanged:

- **ink plane**: bit `0` = ink/black, bit `1` = paper/white.
- **mask plane**: bit `0` = opaque (write the ink pixel), bit `1` =
  transparent (leave the scene visible).
- MSB-first within each byte, row-major, rows byte-aligned at
  `(cell_w + 7) / 8`.

Two deliberate endianness choices that look inconsistent but aren't:
the **envelope** header is little-endian (native to the ESP32-S3, and
the pack is only ever produced for this device), while the **per-cell**
header stays big-endian because copying the fetch format verbatim is
worth more than internal consistency — it's what buys the
byte-identical property.

### Why fixed stride, not a directory

SPRITE·FORGE resamples every cell to one square `outSize`, so all
entries are the same length. Fixed stride means `mpk_cell(i)` is O(1)
pointer arithmetic with no directory walk and no per-entry offsets to
store. If a future pack needs mixed cell sizes, that's a `format`-byte
bump to a directory variant — old packs keep working because the
reader dispatches on `format`.

### Why pixels in `.bin`, zones in `.h`

Pixels are bulk binary → embedded blob, read at runtime. Zones are a
handful of integers + names → compile-time C constants in `_meta.h`,
folded into the image by the linker, type-checked by the compiler, and
greppable in source. This split already worked for the generic SPRT
target; MPK1 keeps it. The `_meta.h` zone tables are the payload that
matters for scene contracts (next section).

## Reader

`firmware/main/mochi_pack.h` — header-only, no allocations. `mpk_open`
validates the magic + version and precomputes the strides; `mpk_find`
does a linear label search (packs are tens of entries); `mpk_ink` /
`mpk_mask` / `mpk_cell` return pointers straight into the embedded
blob. `mpk_zone_test` is the point-in-rect hit-test over a `_meta.h`
zone table.

Wiring is the splash pattern plus a lookup:

```c
extern const uint8_t _binary_pets_mpk_bin_start[]
    asm("_binary_pets_mpk_bin_start");

mpk_t pack;
mpk_open(_binary_pets_mpk_bin_start, &pack);
int i = mpk_find(&pack, "sleeping");
compositor::blit_two_plane(composite, W, H,
    mpk_ink(&pack, i), mpk_mask(&pack, i),
    pack.cell_w, pack.cell_h, PET_DX, PET_DY);
```

## How this changes scene contracts

`06-scene-contracts.md` frames the end-state as **server-authored**:
the server emits a scene image *and* a contract naming per-zone
semantic regions ("the bowl", "the door"), and the device renders +
routes input against the contract instead of hardcoded corner zones.
That direction is unchanged. What MPK1 adds is a **second source for
the same contract shape** — authored at build time, shipped in the
image.

The key alignment: a `_meta.h` zone (`mpk_zone_t` =
`{x, y, w, h, name}`) is structurally identical to what a server scene
contract's regions will be. One hit-test (`mpk_zone_test`) serves
both. So the device gets a single zone model with two providers.

This reshapes the contract roadmap in four concrete ways:

1. **The hardcoded corner zones become authored, not deleted.** The
   M8.5 corner-icon zones (`Zone::CornerTL..BR` in `main.cpp`) are the
   first thing scene contracts were meant to retire. Instead of
   special-casing them until the server contract lands, we can author
   them *now* as a bundled `ui-v1` pack's zone table. Same `{x,y,w,h,
   name}` rows, same hit-test the server path will use — so the
   migration to server contracts is later a matter of swapping the
   zone *provider*, not rewriting the dispatch. The thought-bubble
   hit-rect (`design/12`) is the same shape too and folds in.

2. **A precedence rule, not an either/or.** Bundled zones are the
   floor; a server contract, when present and fresh, overrides per
   scene. Concretely: resolve zones for the current scene from the
   server contract if cached, else fall back to the bundled pack's
   zone set. This gives offline/first-boot interactivity (bundled) and
   dynamic per-scene affordances (server) without a flag day. The
   precedence mirrors the planned splash chain in `05` (LittleFS
   override → embedded fallback), now applied to *contracts* rather
   than pixels.

3. **Contracts get a versioned, diffable on-device baseline.** Today a
   contract exists only as the future server payload. A bundled
   `_meta.h` is a checked-in, reviewable, OTA-shippable baseline
   contract — useful as the fallback above, as a test fixture for the
   dispatch code before the server side exists, and as documentation
   of the canonical zone names the server is expected to honor.

4. **It nudges the contract payload toward a pack, not a sidecar.**
   When the server contract lands, the cheapest device-side shape is
   "a scene cell + its zones in the same envelope" — i.e. a
   server-emitted MPK-shaped response (or a single-cell variant with a
   zone trailer) rather than image-and-JSON fetched separately. Worth
   prototyping the server contract as an MPK1 extension (a `format`
   byte that adds an inline zone trailer per entry) so fetched and
   bundled contracts share one parser end to end. That keeps the
   "byte-identical fetched vs bundled" property at the *contract*
   level, not just the cell level.

### Non-goals / open

- **No firmware wiring yet.** `mochi_pack.h` is in the tree but no
  `.bin` is embedded and no render path reads a pack. First real use
  is most naturally the `ui-v1` icon pack (it removes the boot-time
  80→48 downsample fetch and lets the corner zones be authored).
- **Inline zone trailer (`format` 1).** The server-contract-as-pack
  idea in point 4 is a sketch, not a spec. Needs the server side of
  `06` to firm up first.
- **Mask authoring fidelity.** SPRITE·FORGE derives the mask from the
  source alpha channel (or an optional white-key); the server derives
  it from category-aware chroma keying at upload. For pet/UI art with
  clean alpha the results match, but a sheet keyed differently than
  its alpha implies will differ from the server's cell for the same
  art. Author from alpha-clean PNGs to stay in sync. The optional
  paper-stroke halo matches the server's `STROKE` step
  (`05` § "Paper stroke").

## Cross-references

- `05-sprite-format.md` — the cell wire format MPK1 entries copy
- `06-scene-contracts.md` — the server-authored contract this pairs with
- `12-thought-bubble.md` — another `{x,y,w,h}` hit-rect that folds into
  the unified zone model
- `firmware/main/mochi_pack.h` — the reader
- `site/tools/sprite-forge.html` — the authoring tool (target: mochi-esp)
