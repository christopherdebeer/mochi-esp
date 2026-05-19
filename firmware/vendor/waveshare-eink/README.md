# Vendored: Waveshare e-paper driver

These files are an unmodified snapshot of the Waveshare-supplied SSD1681
driver and pin map for the **ESP32-S3-Touch-ePaper-1.54 V2** board. They
exist here so the firmware build does not depend on the user having a
local clone of the vendor repo, and so future debugging has a frozen
reference for what the vendor sample actually does.

## Provenance

- Repo: `git@github.com:waveshareteam/ESP32-S3-ePaper-1.54.git`
- Commit: `3f96beedd2e8daa35996abd0c055a7d394336dfb`
- Date pulled: 2026-05-18
- License: not declared in repo. Treat as Waveshare reference code; do
  not redistribute as a standalone library.

## What's here

- `epaper_driver_bsp.cpp` / `epaper_driver_bsp.h` — copied verbatim
  from `02_Example/ESP-IDF/V2/07_BATT_PWR_Test/components/epaper_driver_bsp/`,
  with **one local addition** for M4: `EPD_LoadBuffer(const uint8_t*,
  size_t)` that `memcpy`s an external 1-bit packed framebuffer into
  the driver's internal buffer. Both files mark the addition with a
  "NOT VENDOR CODE" comment so future Waveshare diffs are clean.
  Without this hook the M4 sprite-fetch path would have to write
  pixel-by-pixel via `EPD_DrawColorPixel`, which is the same per-pixel
  ceiling that's already a known cost (see project memory
  `project-eink-vendor-driver`).
  C++ class `epaper_driver_display`, SPI master + bit-banged CS/DC/RST
  bringup, full + partial refresh, and the SSD1681 LUT tables.
- `ft6336_bsp.cpp` / `ft6336_bsp.h` — copied verbatim from
  `02_Example/ESP-IDF/V2/13_FT6336_Test/main/`. Singleton C++ wrapper
  around the FT6336 capacitive touch controller. Vendored 2026-05-18
  for M6.
- `i2c_bsp.cpp` / `i2c_bsp.h` — copied verbatim from the same FT6336
  example. Singleton wrapper around `i2c_master_bus_handle_t` (the
  ESP-IDF v5 I²C master API). Will be reused by future RTC (M7) +
  SHTC3 (M8) drivers — same I²C bus on GPIO 47/48.
- `user_config.h` — copied from the same example's `main/`. Contains
  the V2 board pin map (EPD SPI bus, EPD_PWR enable, BOOT/PWR buttons,
  I²C bus for RTC/SHTC3, etc.). The firmware does not include this
  file directly — it re-exports the relevant pins through
  `components/board_pins/` so the vendor file stays untouched.

## V1 vs V2

The board exists in two revisions; this is V2 (ESP32-S3-PICO-1-N8R8,
8MB flash + 8MB PSRAM, shipping since November 2025). V1 used the
ESP32-S3FH4R2 with 4MB+2MB and a different pin map. Do not mix.

## Why "vendor in" rather than wrap or fork?

Decided 2026-05-18 with the M2 milestone. Trade-off:
- **Pro:** fastest path to first pixel; faithful to vendor sample;
  trivial to diff against future Waveshare updates.
- **Con:** C++ in an otherwise C codebase, ugly globals (LUT tables
  live in module scope), no clean RAII/error handling, allocates
  framebuffer in PSRAM with no fallback path.

**Revisit:** consider replacing this with a clean ESP-IDF component
written against the SSD1681 datasheet — likely around the time we
need partial-region refresh for sprite animation (somewhere between
M5 and M7), or earlier if the C++ compile + PSRAM allocation pattern
starts causing real friction. Tracked informally in
`design/01-bring-up-plan.md` under the M2 "Open question".

## How the firmware uses these files

`firmware/main/CMakeLists.txt` adds the `vendor/waveshare-eink/` dir
to the source list and include path so the C++ driver compiles into
the `main` component. `main/main.c` is upgraded to `main/main.cpp`
when M2 lands so that we can use the C++ class directly without a
C wrapper. Pin numbers are pulled from `firmware/main/board_pins.h`,
which mirrors the relevant lines from `user_config.h`.

If we later move this to a proper ESP-IDF component, the wrapping
glue lives in one place and the vendor files stay verbatim.
