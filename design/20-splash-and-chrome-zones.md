# 20 · Boot splash + chrome zones (text / pet placement)

Status: firmware in progress (splash-only). Authoring + encoder shipped in
the `c15r/mochi-device` studio val; device render lands here.

## Goal

Turn the boot splash from a single bundled frame into a **random cell from a
bundle** (`spash-bundle-a`, 4×4 200×200) with **device-drawn chrome**:

- a **title** line (brand, e.g. "Mochi"),
- a **secondary/status** line (firmware version / short boot step),
- the paired pet's **`lonely`** expression (which already faces away),
  placed per the scene.

The placement of each is authored as a **zone** in the studio and carried in
the MPK1 pack — but the *content* (text string, pet sprite) is **device-
authored**, never in the pack.

## Pack format: two new zone kinds (backward compatible)

MPK1 `format=1` zones are a fixed 24-byte record (`x,y,w,h` u16 ×4 · `kind`
u8 · `data` u8 · `label_idx` u16 · 12 reserved). Two new `kind` values reuse
that record with **no layout change** — old readers ignore them, and they are
non-interactive (skipped by tap routing).

| kind | name | `data` byte | rect | content |
|------|------|-------------|------|---------|
| 6 | `MPK_ACTION_TEXT` | `(type & 0x0F) \| (light << 4)` — type 0 = title, 1 = status; bit 4 = light glyphs | where the text draws | device string (title = brand, status = version/boot step) |
| 7 | `MPK_ACTION_PET` | expression index (see `PET_EXPRESSIONS`; `lonely` = 13) | placement **and size** (sprite scaled to fit, nearest-neighbour) | device sprite (paired species pack) |

Mirrored definitions:
- studio: `c15r/mochi-device studio/api.ts` (`TEXT_KIND`/`TextType`/
  `encodeTextData`, `PET_KIND`/`PET_EXPRESSIONS`), packed by `mpk.ts`
  (`MpkAction.Text`/`MpkAction.Pet`), projected by `backend/devsprite.ts`.
- firmware: `firmware/main/mochi_pack.h` (`MPK_ACTION_TEXT`/`MPK_ACTION_PET`,
  `MPK_TEXT_TYPE`/`MPK_TEXT_LIGHT`), expression table in `epd_ui.cpp`.

The pet `lonely` cell already faces away, so no mirror/flip flag is needed.
Only the splash respects pet zones for now; other surfaces render the pet at
their own anchor (the index in `data` is ignored there).

## Device render (`epd_ui::render_boot_splash`)

1. Open the embedded `splash.mpk` (when present), pick a random cell with
   `esp_random`, and load its ink plane as the full 200×200 framebuffer.
2. For a `pet` zone, if the device is paired, load the expression from the
   bundled `pet_a.mpk` and `compositor::blit_two_plane_scaled` it
   (nearest-neighbour) into the rect (square, foot on the rect's bottom).
3. For `text` zones, draw the title / status string into the rect with the
   colour from the `light` bit (no background — the author placed it on
   suitable art).
4. **Fallbacks:** no `splash.mpk` → the bundled `splash.bin` frame. Any
   title/status the pack didn't place → a legible **default banner**
   (background-filled) near the top.

## Build / asset

`splash.mpk` is produced by `firmware/scripts/refresh-splash.sh` fetching
`/devsprite/pack/spash-bundle-a` and committed. It's **optional**:
`main/CMakeLists.txt` embeds it and defines `MOCHI_HAVE_SPLASH_MPK` only when
the file exists, so a clean checkout without the asset still builds (and uses
the `splash.bin` fallback).
