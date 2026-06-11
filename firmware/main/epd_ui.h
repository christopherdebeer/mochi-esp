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
 * scale (1 = 8x8, 2 = 16x16, etc.). Opaque black-on-white, clipped
 * at the panel edges. Thin wrapper over the shared fb1bpp core
 * (design/36) writing into the driver's framebuffer. */
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
/* Boot splash. Renders a (random) cell from the embedded splash pack
 * (splash.mpk) when present — overlaying title + status text at the pack's
 * text zones, and the paired pet's expression at its pet zone — else falls
 * back to the bundled splash.bin. Title/status not placed by a zone get a
 * legible default banner. `paired` gates the pet. design/20. */
void render_boot_splash(epaper_driver_display *epd, const char *title,
                        const char *status, bool paired);

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

/* Transient feedback toast (design/36): a compact bordered card
 * centred on the panel, stamped OVER whatever the driver buffer
 * currently holds (the menu/pet frame stays visible around it) and
 * pushed with a partial refresh. line2 may be NULL/empty for a
 * one-liner. The card is feedback only — it has no hit rect; the
 * caller owns any linger, touch-drain, and repaint that follow. */
void toast(epaper_driver_display *epd,
           const char *line1, const char *line2);

}  /* namespace epd_ui */
