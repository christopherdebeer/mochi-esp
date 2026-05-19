# 05 — Device sprite format

Status: draft, 2026-05-18 (decisions locked, doc written before
endpoint implementation)

How the server hands a sprite to the device. Constrained by what the
device can render and what mochi.val.run already produces.

## Framing

Two existing pipelines. Neither extends to the device cleanly, so M4
introduces a third path that runs alongside them:

- **PNG sheets (existing).** Mochi's web prototype stores sprite
  sheets as full-color PNGs in `std/blob`, with category-aware chroma
  keying applied at upload time. The result is what `/sheets/:id/png`
  serves to the React frontend (`shared/sheets/template.ts`,
  `backend/sheets.ts`). Pet cells are 96×96; sheets are large
  multi-cell PNGs with a grid template describing where each frame
  lives. Decoding a PNG on the device is feasible (puff/tinfl) but
  costs ~10 KB of code we don't need to spend, and most of PNG's
  features (color, filters, palettes) are wasted on a 1-bit panel.
- **SVG layouts (existing).** `layoutSvg` and `guide.svg` are debug
  helpers, not a delivery format.
- **1-bit packed bitmaps (new, for device).** Server pre-renders a
  cell or composite scene into a flat 5000-byte buffer at the panel's
  exact bit packing, served raw over HTTPS. This is the only format
  the device needs to know about.

The device pipeline never has to deal with chroma keying, grid
templates, scaling, or PNG. The server does all of that and emits
finished pixels.

## Wire format

```
GET https://mochi.val.run/devsprite/<sprite-id>
→ 200 OK
  Content-Type: application/octet-stream
  Content-Length: 5000
  ETag: "<16-hex-chars>"
  Cache-Control: public, max-age=300, stale-while-revalidate=86400

  <5000 bytes of packed 1-bit pixels>
```

**No header.** The payload is exactly 5000 bytes, byte-for-byte the
contents of the device's framebuffer. The URL is the schema: a
sprite served at `/devsprite/X` is always 200×200×1bpp packed
MSB-first row-major. If we ever need a different size, animation,
tiling, or whatever, that goes at a different URL prefix
(e.g. `/devsprite-v2/`, `/devsheet/`) — old clients keep working
because they only know about `/devsprite/`.

**Bit packing (matches the vendor SSD1681 driver verbatim):**

```
For pixel (x, y) where 0 ≤ x,y < 200:
  byte_index = y * 25 + (x >> 3)
  bit_in_byte = 7 - (x & 0x07)         ← bit 7 = leftmost pixel
  pixel_value = (buffer[byte_index] >> bit_in_byte) & 1

  pixel_value = 0  → ink (black)
  pixel_value = 1  → paper (white)
```

This is the ordering the Waveshare driver expects when writing to
the SSD1681 RAM via `EPD_SendCommand(0x24)` + `writeBytes(buffer,
5000)`. Choosing this on the wire means the device can `memcpy()`
the response straight into the framebuffer. No bit reversal, no
byte-swap, no scan conversion. See
`firmware/vendor/waveshare-eink/epaper_driver_bsp.cpp:284-294`.

**Why no header.** I considered an 8-byte header (magic / version /
flags / width / height) for forward compatibility, and rejected it
*for M4*:

- The URL already encodes everything a header would carry. M4 only
  needs to fetch *the one test sprite*; no client ever needs to ask
  "what shape is this".
- Headers tempt callers into doing format detection at runtime, which
  the device should not be doing. Format = URL prefix; pin it there.
- 8 bytes more on the wire is irrelevant. The point is keeping the
  device side dumb.

The "no header" rule held for M4–M8.5's panel-size format. **It was
relaxed** for the cell endpoint that M8.5 introduced — see "Format
evolution" below.

## Endpoint shape (M4 scope)

For M4 we only need a single fixed test sprite:

```
GET /devsprite/test
```

returns the same 5000-byte payload every time. The sprite is a
**96×96 fox silhouette** centered on the 200×200 panel, with a small
caption "mochi.val.run / devsprite/test" rendered below it via the
device-side font.

The fox is hand-drawn in TypeScript directly into a Uint8Array and
written through a thin `setPixel(x, y, ink)` helper that does the
same `byte_index = y*25 + (x>>3); bit = 7 - (x&7)` math the device
does. No image library is involved.

**What this proves.**
- The HTTPS path works end-to-end (TLS + root cert bundle + cleartext
  HTTP/1.1 over TLS — none of which has been exercised yet).
- The wire format is correct: device fetches, `memcpy`s, calls
  `EPD_Display()`, sees the fox.
- The mental model "server pre-renders, device writes raw" survives
  contact with reality before we touch the actual sheet → bitmap
  conversion.

**What this does NOT prove.**
- The general sheet → 1bpp conversion pipeline (deferred to M4.5 or
  M5 depending on order).
- Pairing or per-pet sprite serving (M5: introduces `X-Pet-Id`
  scoping on `/devsprite/<id>`).
- Per-frame serving for animation (M11+).

## Server side — implementation sketch

Lives at `mochi.val.run`'s `main.ts` as a top-level route, NOT under
`/api` (that namespace is reserved for the React frontend's
authenticated calls). New file: `backend/devsprite.ts`, mounted at
`/devsprite`. The route is **public, unauthenticated** — the device
has no session yet, and the test sprite is not sensitive.

```
// backend/devsprite.ts
import { Hono } from "https://esm.sh/hono@4.6.14";

export const devsprite = new Hono();

devsprite.get("/test", (c) => {
  const buf = renderTestSprite();   // returns Uint8Array of length 5000
  return new Response(buf, {
    headers: {
      "Content-Type": "application/octet-stream",
      "Content-Length": "5000",
      "Cache-Control": "public, max-age=300",
    },
  });
});
```

`renderTestSprite()` lives in the same file. It allocates a
`Uint8Array(5000)` initialised to `0xFF` (all white), then draws:
1. A 96×96 fox silhouette centered at (52, 52)..(147, 147).
2. A two-line caption underneath (font lives device-side; for M4 the
   caption is hand-pixelled like the fox).

Future sprites get their own renderers in the same file or split out
when there are more than two.

**Why not reuse `backend/sheets.ts`?** The keying / overrides / ETag
machinery there is for the chroma-keyed PNG pipeline that feeds the
React frontend. The device sprite path has none of those concerns
and shouldn't pay for them. Distinct file, distinct concern.

## ETag / caching

For now, no ETag. The test sprite is fixed in source; clients that
care about freshness can re-GET. M4's success criterion is "round
trip <3s", and a 5000-byte body over HTTPS round trip easily fits.
When we have real per-pet sprites in M5+, ETags become useful and we
add them then — exactly the same pattern `backend/sheets.ts` uses.

## Open questions tracked here

1. **Cell size on device.** Pet cells are 96×96 server-side; panel is
   200×200. Center at native size (default), 2× scale (192×192 nearly
   fills the panel), or composite with a place backdrop? M4 uses 96×96
   centered to keep variables minimum. Real answer waits for M11 when
   we know what the pet UI actually looks like.
2. **Endianness of bit packing.** Locked to MSB-first within each byte
   to match the SSD1681 vendor driver. If we ever swap to a
   community e-paper component (see `01-bring-up-plan.md` M2 open
   question), this assumption needs re-checking — most other
   e-paper libraries use the same convention but not all.
3. **Inversion.** SSD1681 treats `0` as ink and `1` as paper.
   Confirmed by reading the vendor `EPD_DrawColorPixel`: when
   `color == DRIVER_COLOR_WHITE` (0xFF) it sets the bit; when black
   (0x00) it clears the bit. Wire format mirrors this.

## Known asset-size mismatch (deferred refinement)

The Mochi web prototype's sheets were authored for an iPhone-style
360×360 viewport. Cell aspect ratios reflect that:

| Sheet         | Native cell           | Our use            |
|---------------|-----------------------|--------------------|
| `pet-v1`      | 96×96 (template-space)| Foot-anchored on a 200×172 scene area; fits with margin. |
| `ui-v1`       | 80×80                 | Downsampled to 48×48 for corner icons; some thin features erode. |
| `scene-v1`    | 360×336 (≈1.07:1)     | Fitted into 200×172 (≈1.16:1) → letterboxed left+right. |

For the eink-pet device, the *correct* asset shape would be:

- Scene cells at the device area's aspect (200×172) so we don't
  letterbox.
- UI icons at the actual rendered icon size (48×48 today) so we
  don't downsample-erode.
- Pet cells could stay 96×96 — they composite fine inside the
  scene area at native size.

Refinement options when we revisit (M11+ era when the pet UI
settles):

1. Add a `?fit=fill` mode to the panel-size endpoint that crops to
   fill rather than letterbox-fit. Loses some scene edge content;
   eliminates margins.
2. Author a second `scene-v1-eink` sheet at native eink dimensions.
3. Stretch the scene area to 200×200 (status bar overlays the scene
   instead of being its own band).

None of these are blocking for current bring-up.

## Device-first authoring (splash-v1)

The asset-size mismatch above frames the existing sheets as
mobile-first, downsampled-to-device. `splash-v1` (2026-05-18) is
the first sheet authored in the *other* direction: the template
declares the cell at **native device resolution** (200×200) and the
mobile prototype either downsamples to its viewport or doesn't
use the sheet at all. The endpoint
`/devsprite/splash-v1/boot` returns a 5000-byte panel framebuffer
with no conversion, no center-on-white, no letterboxing — every
PNG pixel is a device pixel after threshold.

The pattern, for future device-first sheets:

- Template `cellWidth × cellHeight` = native device dimensions
- `cols × rows = 1×1` initially (one cell per "image"); expand the
  grid when variants of the same image are wanted (e.g. seasonal
  splashes at the same dimensions)
- `gapX = gapY = 0`, `margin = 0` — every source pixel matters
- Category `"ui"` for chrome-style assets, will get its own
  category as the pattern accumulates more examples

Other sheets that should follow eventually:
`scene-v1-eink` at 200×172 (eliminates the letterbox the existing
360×336 sheet forces), `ui-v1-eink` at 48×48 native icons (avoids
the downsample-erode of fine line work).

### Boot splash as bundled-blob + OTA override

The boot splash is the device's first frame on every cold boot,
before WiFi, before NVS read. It must work offline. The pipeline:

1. **Build-time fetch.** `firmware/scripts/refresh-splash.sh`
   pulls `/devsprite/splash-v1/boot` into
   `firmware/main/assets/splash.bin`. Falls back to
   `/devsprite/test` if no artwork uploaded yet (placeholder for
   first-time builds).
2. **CMake embed.** `EMBED_FILES "assets/splash.bin"` in
   `firmware/main/CMakeLists.txt` bundles the 5000 bytes into
   firmware via the linker's `_binary_splash_bin_start` symbol.
3. **Boot path render.** `epd_ui::render_boot_splash()` does a
   single `EPD_LoadBuffer` with the embedded bytes — no per-pixel
   work, no font path. Boot is faster + the splash artwork is
   data-driven.

The build-time bundled splash is the **brand-themed default**, no
pet specifics. It's deliberately not regenerated per-build (only
when `refresh-splash.sh` is explicitly run), so the firmware
image is reproducible without network access.

### Pet-context dispatch via Pet-Id header

`/devsprite/splash-v1/boot` is designed for a server-side dispatch
pattern: same URL, optional `X-Pet-Id` header, different bytes
returned based on which pet is asking.

- **No header** (or no recognised pet): brand-themed default
- **Recognised pet with a custom splash**: that pet's variant
- **Recognised pet, no custom splash**: brand default (fallthrough)

This is the same shape as the version-in-etag pattern from the
devsprite paper-stroke change (workspace `d15c67f10e0d4b`) extended
along a second dimension. Both use **stable URL, server-side
dispatch by context** rather than URL-path proliferation. The
endpoint's ETag combines the upstream sheet etag with the relevant
context — version for paper-stroke, pet-id for splash variants —
so device caches invalidate correctly on either dimension changing.

`refresh-splash.sh` deliberately sends no Pet-Id header. The
bundled blob is the brand default; per-pet customisation is the
OTA layer's job.

### OTA fallback chain (planned, not built)

Splash refresh isn't urgent (a stale splash isn't broken; it's
just stale), but iteration matters — seasonal variants, pet-themed
updates, event-based refreshes. The intended chain when the OTA
layer lands:

1. **LittleFS override** (`/lfs/splash.bin`) — fetched at boot
   after WiFi comes up, HEAD-probed + ETag-invalidated on the
   same schedule as `sprite_cache`'s existing per-sheet probes.
   Request includes `X-Pet-Id: <pet-id>` for per-pet variants.
2. **Embedded blob** (`_binary_splash_bin_start`) — bundled
   fallback, used when LittleFS is empty (first boot post-pair,
   factory-reset) or stale (probe failed, no network).

`render_boot_splash` would prefer LittleFS, fall back to
embedded. Mechanism is straightforward — the HEAD-probe-and-
invalidate machinery in `sprite_cache.{cpp,h}` already handles
pets/UI/scenes; splash refresh would slot in as one more entry
in the probe table. Implementation deferred until a real
iteration cadence on the splash exists.

## Format evolution (M8.5)

M4 shipped only `/devsprite/test` (panel-shaped, no header). M8.5
added two new endpoints that serve sprite *cells* and *scenes* on
demand, both at sizes other than the panel.

### `/devsprite/cell/<sheet>/<cell>` — native-size cells, 2-plane

Emits a single sheet cell at its native size (96×96 for `pet-v1`,
80×80 for `ui-v1`) as a 2-plane 1-bit bitmap with an 8-byte header:

```
byte 0..1   width  (u16, big-endian)
byte 2..3   height (u16, big-endian)
byte 4      flags  (bit 0 = mask plane present; others reserved 0)
byte 5..7   reserved (must be 0)
byte 8..    ink plane  (width*height/8 bytes, MSB-first row-major)
byte ...    mask plane (same size, present iff flags bit 0 set)
```

Both planes match the panel format's bit convention (bit `0`
means "ink present" or "pixel opaque"). Concretely:

- **ink plane**: `0` = ink/black, `1` = paper/white. Same rule as
  the panel single-plane format.
- **mask plane**: `0` = opaque (write this pixel), `1` = transparent
  (leave the scene visible).

Device-side compositing is

```
for each pixel:
  if mask == 1: leave dst untouched      // transparent
  else:         dst = ink                 // opaque (paint ink or paper)
```

Why a mask plane at all: cells with cream/tan body fills had no
way in a single 1-bit plane to distinguish "this pixel is paper"
from "this pixel is transparent background". The body collapsed
to solid white, which then composited *as paper* over scene
backgrounds and made the pet line work float on a white box.
Mask plane fixes that: body pixels are `ink=1, mask=0` (opaque
paper), background corners are `mask=1` (transparent). See
project memory `project-eink-two-plane-cells`.

#### Paper stroke (outer-edge differentiator)

Cells served from the cell endpoint have a **1-pixel paper-
coloured stroke** added around the outer edge of the opaque
silhouette before they are returned. Every pixel that was
originally transparent (`mask=1`) but has at least one opaque
(`mask=0`) 8-neighbour becomes opaque paper (`mask=0, ink=1`).

The visual effect: when a pet outline (black ink, opaque) sits
against a dark area of a scene background, the surrounding ring
of paper pixels separates the two, so the silhouette doesn't
visually merge into the scene. Applies uniformly to pet cells
and UI icons because both flow through this endpoint.

The stroke is applied server-side at template resolution, so the
device pays nothing for it at composite time — the planes that
arrive on the wire already have the halo baked in.

Wire-format implication: a cell's "silhouette" as seen by the
compositor is now 1 pixel larger in every direction than the
artist drew. The stroke does NOT extend beyond the cell bounds —
out-of-bounds neighbours are treated as transparent, so silhouette
portions touching the cell edge are clipped flat at that edge.

The stroke logic is versioned (`STROKE_VERSION` in
`c15r/mochi:backend/devsprite-encode.ts`). The server folds the
current version into the wire ETag as `"<srcEtag>-s<N>"` so any
bump invalidates the device's LittleFS sprite cache on its next
HEAD probe — no device-side change required.

**The header is present here, in deliberate contradiction of the
"no header" rule above.** Reasoning: cells are not panel-shaped,
the device receives many of them at varying native dimensions,
and runtime size knowledge is required to composite. The cost of
the header (8 bytes / cell, ~1 % overhead at 96×96) is dwarfed by
the cost of encoding size in the URL prefix (`/devsprite-96/`,
`/devsprite-80/`, etc., proliferating per cell-source).

### `/devsprite/<sheet>/<cell>?fit=<mode>` — panel-area scenes

Emits a sheet cell *resized to the panel scene area* (200×172, the
panel minus the top 28 px status bar) as a single-plane raw
bitmap with no header — it is panel-area-shaped and dimensions are
implicit at the URL, the same shape rule the original
`/devsprite/test` followed. Two fit modes:

- **`?fit=area`** — letterbox-fit: scales to fit inside 200×172
  with paper-colored bars on the short axis. Preserves all source
  pixels at the cost of wasted panel area.
- **`?fit=fill`** — cover-fit: scales to fill 200×172, cropping
  overflow on the long axis. Wastes no panel area at the cost of
  source content near the cropped edges. Current default for
  `scene-v1` cells (360×336 source, 1.07:1 aspect, doesn't fit
  cleanly without one or the other).

### Why two endpoints, not one

Cells composite; scenes don't. Cells are small and need the mask
plane. Scenes are panel-sized and don't (they *fill* the area;
transparency is meaningless). Forcing both through one endpoint
either burdens scene fetches with a useless mask plane or burdens
cell fetches with implicit panel-area sizing they don't want.
Distinct shapes → distinct URLs.

ETag is now wired on both: server emits an `ETag` header per
cell/scene, device persists it alongside the cached body in
LittleFS, HEAD probes at boot decide whether to refetch
(`sprite_cache.{cpp,h}`).

## Future direction: scene contracts

The current model — server emits a scene image, device hardcodes
the corner-tap zones it routes input through — is a stepping
stone. The end-state vision lives in
`c15r/mochi:design/diegetic-interfaces.md`: the server emits a
*scene contract* alongside the image, naming per-zone semantic
regions ("the bowl", "the door", "the toy") that the device
renders against and routes taps and voice intents through. The
image *suggests* affordances; the contract *defines* them.

`06-scene-contracts.md` (stub) tracks the device side of that
shift. M11.5 is the milestone where the device starts honouring
scene contracts and stops baking in zone semantics; the M8.5
corner-icon UI gets retired then, and this format doc grows a
sibling describing the contract payload.

## Cross-references

- `00-architecture.md` — sprite generation lives server-side
- `01-bring-up-plan.md` — M4 (this doc unblocks it)
- `firmware/vendor/waveshare-eink/epaper_driver_bsp.cpp` — bit packing
   that this format must match
- `firmware/main/assets/README.md` — splash bundling workflow
- `c15r/mochi:backend/sheets.ts` — the existing PNG pipeline this
   one runs alongside, not on top of
- `c15r/mochi:shared/sheets/splash-v1.ts` — first device-first sheet
- workspace `d15c67f10e0d4b` — devsprite paper-stroke; same
   stable-URL-with-context-dispatch pattern as splash Pet-Id
