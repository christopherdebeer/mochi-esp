#include "epd_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "board_pins.h"
#include "font8x8.h"

extern "C" {
#include "qrcodegen.h"
}

namespace epd_ui {

static constexpr int W = MOCHI_EPD_WIDTH;
static constexpr int H = MOCHI_EPD_HEIGHT;

/* maxVersion for QR encoding. V5 = 37×37 modules holds 106 bytes at
 * ECC Low / 84 at ECC Medium — enough for both a WIFI: URI (~32
 * chars) and our pair-device URL with embedded code (~52 chars).
 * Bumping further wastes ~75 bytes of scratch per call without
 * adding useful headroom; both payloads are bounded. */
static constexpr int QR_MAX_VERSION = 5;

void clear(epaper_driver_display *epd) {
    epd->EPD_Clear();
}

static void blit_glyph(epaper_driver_display *epd, char c,
                       int ox, int oy, int scale) {
    const uint8_t *g = font8x8_glyph(c);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            bool on = (bits >> col) & 1;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    int x = ox + col * scale + dx;
                    int y = oy + row * scale + dy;
                    if (x < 0 || y < 0 || x >= W || y >= H) continue;
                    epd->EPD_DrawColorPixel(x, y,
                        on ? DRIVER_COLOR_BLACK : DRIVER_COLOR_WHITE);
                }
            }
        }
    }
}

void draw_text(epaper_driver_display *epd, int x, int y, int scale,
               const char *text) {
    int gw = 8 * scale;
    int cur = x;
    for (const char *p = text; *p; p++) {
        if (cur + gw > W) break;
        blit_glyph(epd, *p, cur, y, scale);
        cur += gw;
    }
}

void draw_text_centered(epaper_driver_display *epd, int y, int scale,
                        const char *text) {
    int len = static_cast<int>(strlen(text));
    int gw = 8 * scale;
    int total = len * gw;
    int x = (W - total) / 2;
    if (x < 0) x = 0;
    draw_text(epd, x, y, scale, text);
}

/*
 * 200x200 budget: font line at scale=2 is 16px tall, scale=3 is 24px.
 * A scale=2 line needs ~3.2 ms of EPD_DrawColorPixel calls per glyph
 * (8x8x4 = 256 pixel writes); a 14-character line is ~45ms. Adding
 * up: a four-line provisioning screen is well under 250ms of CPU
 * before we even hit the panel refresh. That's fine.
 */

/*
 * Boot splash bytes — bundled at build time via
 * firmware/scripts/refresh-splash.sh fetching /devsprite/splash-v1/boot
 * (with /devsprite/test as fallback if no artwork uploaded yet) and
 * embedded by EMBED_FILES in firmware/main/CMakeLists.txt.
 *
 * Wire format is byte-identical to the panel framebuffer: packed 1-bit
 * MSB-first, row-major, 0 = ink, 1 = paper. We push it straight through
 * the vendor driver's bulk-load entry point — no per-pixel work, no
 * compositor pass, just a single memcpy into the controller RAM
 * inside EPD_LoadBuffer. Boot path is faster + the splash artwork is
 * data-driven rather than bit-bashed text.
 *
 * Future: OTA refresh stores a per-pet override at /lfs/splash.bin
 * via the X-Pet-Id-aware /devsprite/splash-v1/boot endpoint. When
 * that lands, render_boot_splash prefers LittleFS, falls back to
 * the embedded blob. The embedded blob is the brand-themed default
 * — never pet-specific.
 */
extern const uint8_t splash_bin_start[] asm("_binary_splash_bin_start");
extern const uint8_t splash_bin_end[]   asm("_binary_splash_bin_end");

void render_boot_splash(epaper_driver_display *epd) {
    const size_t len = (size_t)(splash_bin_end - splash_bin_start);
    epd->EPD_LoadBuffer((uint8_t *)splash_bin_start, len);
}

/* Stamp on-bits only, in the requested colour. Off-bits leave the
 * existing framebuffer pixel alone — used by overlay_boot_version
 * so the splash artwork shows through the gaps between glyphs. */
static void blit_glyph_overlay(epaper_driver_display *epd, char c,
                               int ox, int oy, int scale,
                               COLOR_IMAGE colour) {
    const uint8_t *g = font8x8_glyph(c);
    for (int row = 0; row < 8; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (!((bits >> col) & 1)) continue;
            for (int dy = 0; dy < scale; dy++) {
                for (int dx = 0; dx < scale; dx++) {
                    int x = ox + col * scale + dx;
                    int y = oy + row * scale + dy;
                    if (x < 0 || y < 0 || x >= W || y >= H) continue;
                    epd->EPD_DrawColorPixel(x, y, colour);
                }
            }
        }
    }
}

void overlay_boot_version(epaper_driver_display *epd, const char *version) {
    if (!version || !*version) return;
    /* 30% from the top: 200 * 0.30 = 60. Glyphs are 8 px tall at
     * scale=1, so y=60 puts the text vertically centred-ish on that
     * line. */
    constexpr int y = 60;
    int len = static_cast<int>(strlen(version));
    int total = len * 8;
    int x = (W - total) / 2;
    if (x < 0) x = 0;
    for (const char *p = version; *p; p++) {
        if (x + 8 > W) break;
        blit_glyph_overlay(epd, *p, x, y, 1, DRIVER_COLOR_WHITE);
        x += 8;
    }
}

int draw_qr(epaper_driver_display *epd, int ox, int oy, int scale,
            const char *text) {
    if (!text || scale < 1) return 0;

    const size_t buf_len = qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION);
    uint8_t *qrcode = (uint8_t *)malloc(buf_len);
    uint8_t *tmp    = (uint8_t *)malloc(buf_len);
    if (!qrcode || !tmp) { free(qrcode); free(tmp); return 0; }

    /* boostEcl=false so the version stays predictable given the
     * payload — we sized our layout against the minimum version that
     * fits the input, not whatever the encoder upsells us to. */
    bool ok = qrcodegen_encodeText(
        text, tmp, qrcode,
        qrcodegen_Ecc_LOW,
        qrcodegen_VERSION_MIN, QR_MAX_VERSION,
        qrcodegen_Mask_AUTO, /*boostEcl=*/false);

    int rendered = 0;
    if (ok) {
        int size = qrcodegen_getSize(qrcode);
        int px   = size * scale;
        if (ox >= 0 && oy >= 0 && ox + px <= W && oy + px <= H) {
            for (int my = 0; my < size; my++) {
                for (int mx = 0; mx < size; mx++) {
                    if (!qrcodegen_getModule(qrcode, mx, my)) continue;
                    int x0 = ox + mx * scale;
                    int y0 = oy + my * scale;
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            epd->EPD_DrawColorPixel(x0 + dx, y0 + dy,
                                                    DRIVER_COLOR_BLACK);
                        }
                    }
                }
            }
            rendered = px;
        }
    }

    free(qrcode);
    free(tmp);
    return rendered;
}

int draw_qr_centered(epaper_driver_display *epd, int top_y,
                     int target_px, const char *text) {
    if (!text) return 0;

    /* Probe-encode once to learn the chosen version → module count,
     * pick the largest scale that fits target_px, then draw. */
    const size_t buf_len = qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION);
    uint8_t *qrcode = (uint8_t *)malloc(buf_len);
    uint8_t *tmp    = (uint8_t *)malloc(buf_len);
    if (!qrcode || !tmp) { free(qrcode); free(tmp); return 0; }

    bool ok = qrcodegen_encodeText(
        text, tmp, qrcode,
        qrcodegen_Ecc_LOW,
        qrcodegen_VERSION_MIN, QR_MAX_VERSION,
        qrcodegen_Mask_AUTO, /*boostEcl=*/false);

    int rendered = 0;
    if (ok) {
        int size = qrcodegen_getSize(qrcode);
        int scale = target_px / size;
        if (scale >= 1) {
            int px = size * scale;
            int ox = (W - px) / 2;
            if (ox < 0) ox = 0;
            for (int my = 0; my < size; my++) {
                for (int mx = 0; mx < size; mx++) {
                    if (!qrcodegen_getModule(qrcode, mx, my)) continue;
                    int x0 = ox + mx * scale;
                    int y0 = top_y + my * scale;
                    if (y0 + scale > H) continue;
                    for (int dy = 0; dy < scale; dy++) {
                        for (int dx = 0; dx < scale; dx++) {
                            epd->EPD_DrawColorPixel(x0 + dx, y0 + dy,
                                                    DRIVER_COLOR_BLACK);
                        }
                    }
                }
            }
            rendered = px;
        }
    }

    free(qrcode);
    free(tmp);
    return rendered;
}

void render_prov_idle(epaper_driver_display *epd, const char *ssid) {
    clear(epd);
    draw_text_centered(epd, 4, 1, "Scan to join WiFi");

    /* WIFI: URI for "join this open network" — iOS Camera and modern
     * Android scanners recognise this scheme natively. T:nopass since
     * the SoftAP runs open (the captive portal is the next gate). */
    char uri[64];
    snprintf(uri, sizeof(uri), "WIFI:T:nopass;S:%s;P:;H:false;;", ssid);

    /* Target a 132 px box at y=20 — leaves ~30 px on either side as
     * implicit quiet zone, and frees the bottom 48 px for the manual
     * fallback. V2 (25 modules) at scale 4 = 100 px, well inside. */
    int qr_px = draw_qr_centered(epd, 20, 132, uri);
    int below_y = qr_px > 0 ? 20 + qr_px + 8 : 100;

    if (qr_px == 0) {
        /* Encoder failure path — fall back to the pre-QR text-only
         * shape so the device is still usable. */
        draw_text_centered(epd, 60, 2, "Hi! I'm Mochi.");
        below_y = 100;
    }

    draw_text_centered(epd, below_y,     1, "Or join manually:");
    draw_text_centered(epd, below_y + 16, 2, ssid);
}

void render_prov_connecting(epaper_driver_display *epd) {
    clear(epd);
    draw_text_centered(epd, 70, 2, "Trying your");
    draw_text_centered(epd, 100, 2, "WiFi...");
}

void render_prov_failed(epaper_driver_display *epd) {
    clear(epd);
    draw_text_centered(epd, 30, 2, "That didn't");
    draw_text_centered(epd, 56, 2, "work.");
    draw_text_centered(epd, 100, 1, "Try again on");
    draw_text_centered(epd, 116, 1, "your phone.");
}

void render_online(epaper_driver_display *epd, const char *ip_str) {
    clear(epd);
    draw_text_centered(epd, 40, 3, "Online");
    draw_text_centered(epd, 100, 1, "IP address:");
    draw_text_centered(epd, 124, 2, ip_str);
}

void render_prompt_fetch(epaper_driver_display *epd, const char *ip_str) {
    clear(epd);
    draw_text_centered(epd, 20, 2, "Mochi online");
    draw_text_centered(epd, 56, 1, ip_str);
    draw_text_centered(epd, 96, 2, "Tap centre to");
    draw_text_centered(epd, 124, 2, "cycle sprites");
    draw_text_centered(epd, 168, 1, "Corners = dot");
}

void render_fetching(epaper_driver_display *epd) {
    clear(epd);
    draw_text_centered(epd, 70, 3, "Fetching");
    draw_text_centered(epd, 120, 2, "sprite...");
}

void render_fetch_failed(epaper_driver_display *epd) {
    clear(epd);
    draw_text_centered(epd, 50, 2, "Fetch failed");
    draw_text_centered(epd, 100, 1, "Check log over USB");
    draw_text_centered(epd, 140, 1, "Tap BOOT to retry");
}

void overlay_fetch_status(epaper_driver_display *epd, uint32_t elapsed_ms) {
    /*
     * The sprite occupies the top 2/3 of the panel. The 200x200
     * test sprite has caption text starting at y=140 (16px high)
     * and y=162. We overwrite a 20-pixel slab below the sprite's
     * caption with our "NNN ms" status — between y=184 and y=200.
     * Pixels above are left as-is so the sprite is preserved.
     */
    char buf[24];
    snprintf(buf, sizeof(buf), "%lu ms", (unsigned long)elapsed_ms);

    /* White-out the bottom slab first via per-pixel writes. Cheap:
     * 200x16 = 3200 setPixel calls, well under 100ms. */
    for (int y = 184; y < H; y++) {
        for (int x = 0; x < W; x++) {
            epd->EPD_DrawColorPixel(x, y, DRIVER_COLOR_WHITE);
        }
    }
    draw_text_centered(epd, 184, 2, buf);
}

void render_pair_prompt(epaper_driver_display *epd, const char *code) {
    clear(epd);
    draw_text_centered(epd, 4, 1, "Scan to pair me");

    /* Deep link with the code pre-baked so the phone form only needs
     * name+PIN. mochi.val.run/pair-device reads ?code= and pre-fills
     * the code field (see backend/device-pair.ts). The URL is 50–52
     * chars depending on the code; encodes to QR V4 (33 modules) at
     * ECC Low. */
    char url[96];
    snprintf(url, sizeof(url),
             "https://mochi.val.run/pair-device?code=%s", code);

    int qr_px = draw_qr_centered(epd, 18, 105, url);
    int below_y = qr_px > 0 ? 18 + qr_px + 6 : 18;

    if (qr_px == 0) {
        /* Encoder failure path — keep the pre-QR text-only flow. */
        draw_text_centered(epd, 18, 2, "Pair me!");
        below_y = 60;
    }

    draw_text_centered(epd, below_y,      1, "Or visit on your phone:");
    draw_text_centered(epd, below_y + 16, 1, "mochi.val.run/pair-device");
    /* Code at scale 2 = 16-pixel-tall block. MOCHI- + 4 = 10 chars *
     * 16 px = 160 px wide, fits with 20 px margin either side. */
    draw_text_centered(epd, below_y + 38, 2, code);
}

void render_pair_success(epaper_driver_display *epd, const char *pet_name) {
    clear(epd);
    draw_text_centered(epd, 60, 2, "Hi");
    /* Pet name might be longer than the panel can fit at scale 3;
     * render at scale 2 to be safe. */
    draw_text_centered(epd, 96, 3, pet_name);
    draw_text_centered(epd, 152, 1, "Loading you in...");
}

void render_pair_failed(epaper_driver_display *epd) {
    clear(epd);
    draw_text_centered(epd, 50, 2, "Pairing");
    draw_text_centered(epd, 80, 2, "failed.");
    draw_text_centered(epd, 124, 1, "Code may have expired.");
    draw_text_centered(epd, 144, 1, "Tap to retry.");
}

void draw_dot(epaper_driver_display *epd, int cx, int cy, int size) {
    int half = size / 2;
    for (int dy = -half; dy < half; dy++) {
        for (int dx = -half; dx < half; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= W || y >= H) continue;
            epd->EPD_DrawColorPixel(x, y, DRIVER_COLOR_BLACK);
        }
    }
}

void render_key_portal(epaper_driver_display *epd,
                       const char *ip_str,
                       const char *url) {
    clear(epd);
    draw_text_centered(epd, 4, 1, "Set OpenAI key");

    /* The URL is short (~24 chars: "http://192.168.1.255/"), so the
     * encoder picks V1 or V2. 105 px target with margins gives a
     * crisp readable code from a phone held a few inches away. */
    int qr_px = draw_qr_centered(epd, 18, 105, url);
    int below_y = qr_px > 0 ? 18 + qr_px + 6 : 30;

    if (qr_px == 0) {
        /* Encoder failure path — fall back to typed URL. */
        draw_text_centered(epd, 30, 1, "Open on phone:");
    }

    draw_text_centered(epd, below_y,      1, "or open on phone:");
    draw_text_centered(epd, below_y + 16, 1, ip_str);
    draw_text_centered(epd, 184,          1, "Tap to dismiss");
}

}  /* namespace epd_ui */
