/*
 * Thin UI layer over the vendored Waveshare SSD1681 driver.
 *
 * Owns the text-rendering helpers (font8x8 + scaled blits) and a
 * few canned screen builders that the main flow calls. The driver
 * itself stays untouched in vendor/waveshare-eink/.
 */

#pragma once

#include <stdint.h>
#include "epaper_driver_bsp.h"

namespace epd_ui {

/* Fill the entire framebuffer with white (0xFF). */
void clear(epaper_driver_display *epd);

/* Render `text` starting at top-left pixel (x, y) at the given
 * scale (1 = 8x8, 2 = 16x16, etc.). Stops at the first NUL or when
 * the next glyph would clip the right edge.
 *
 * Note: this writes via EPD_DrawColorPixel — the slow path. Fine
 * for static screens (provisioning, status). NOT acceptable for
 * animation; see project memory on the per-pixel ceiling. */
void draw_text(epaper_driver_display *epd, int x, int y, int scale,
               const char *text);

/* Convenience: render `text` horizontally centred on the panel
 * at vertical pixel `y` and `scale`. */
void draw_text_centered(epaper_driver_display *epd, int y, int scale,
                        const char *text);

/* High-level provisioning screens. Each one renders a complete
 * frame; caller picks full vs partial refresh. */
void render_prov_idle(epaper_driver_display *epd, const char *ssid);
void render_prov_connecting(epaper_driver_display *epd);
void render_prov_failed(epaper_driver_display *epd);
void render_online(epaper_driver_display *epd, const char *ip_str);
void render_boot_splash(epaper_driver_display *epd);

/* Overlay short white text (small, scale=1) horizontally centred at
 * 30% from the top of the panel. Designed to stamp the firmware
 * version on top of the boot splash without redrawing the splash —
 * call after render_boot_splash but before EPD_Display. White glyph
 * pixels read against dark splash artwork; the surrounding off-bits
 * of each glyph are left as-is. */
void overlay_boot_version(epaper_driver_display *epd, const char *version);

/* M4 — sprite fetch prompts. */
void render_prompt_fetch(epaper_driver_display *epd, const char *ip_str);
void render_fetching(epaper_driver_display *epd);
void render_fetch_failed(epaper_driver_display *epd);
/* Overlay a short caption at the bottom of an already-loaded
 * sprite buffer. The caller is responsible for having called
 * EPD_LoadBuffer with the sprite first; this just adds the
 * "fetched in NNN ms" line on top. */
void overlay_fetch_status(epaper_driver_display *epd, uint32_t elapsed_ms);

/* Draw a small filled square (size×size) centered at (cx, cy).
 * Uses EPD_DrawColorPixel — the slow path. Only used for ad-hoc
 * marks like corner-tap feedback. */
void draw_dot(epaper_driver_display *epd, int cx, int cy, int size);

/* Encode `text` as a QR Code and draw it with the top-left corner at
 * (ox, oy), each module rendered as `scale × scale` pixels in
 * DRIVER_COLOR_BLACK against the existing (assumed-white)
 * framebuffer. Returns the pixel width of the rendered code (i.e.
 * module-count × scale) on success, or 0 on failure (encoder error
 * or won't fit inside the panel).
 *
 * The quiet zone is the caller's responsibility — leave at least
 * 4 × scale white pixels of margin around the requested rectangle
 * before painting any non-white pixel into it. Internally this
 * heap-allocates two qrcodegen scratch buffers of
 * BUFFER_LEN_FOR_VERSION(MAX) each (~600 bytes total) for the
 * duration of the call.
 *
 * Uses the slow EPD_DrawColorPixel path. A V4 (33-module) QR at
 * scale 3 is ~10k pixel writes — comparable to a long text line. */
int draw_qr(epaper_driver_display *epd, int ox, int oy, int scale,
            const char *text);

/* Encode + draw a QR centered horizontally at vertical pixel `top_y`,
 * automatically picking the largest module-scale that lets the code
 * (no quiet zone) fit inside `target_px` pixels on a side. Returns
 * the rendered pixel width, or 0 on failure. Use this when you know
 * the box the QR has to live in but want the encoder to pick the
 * version. */
int draw_qr_centered(epaper_driver_display *epd, int top_y,
                     int target_px, const char *text);

/* M5 — pairing screens. */
void render_pair_prompt(epaper_driver_display *epd, const char *code);
void render_pair_success(epaper_driver_display *epd, const char *pet_name);
void render_pair_failed(epaper_driver_display *epd);

/* Key-portal recovery screen — QR encoding the URL the user should
 * visit on their phone, with the IP printed below as a fallback for
 * scanners that aren't handy. ip_str is "192.168.x.y", url is
 * "http://192.168.x.y/". Caller picks full or partial refresh. */
void render_key_portal(epaper_driver_display *epd,
                       const char *ip_str,
                       const char *url);

}  /* namespace epd_ui */
