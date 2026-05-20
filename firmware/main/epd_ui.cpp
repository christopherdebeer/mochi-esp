#include "epd_ui.h"

#include <stdio.h>
#include <string.h>
#include "board_pins.h"
#include "font8x8.h"

namespace epd_ui {

static constexpr int W = MOCHI_EPD_WIDTH;
static constexpr int H = MOCHI_EPD_HEIGHT;

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

void render_prov_idle(epaper_driver_display *epd, const char *ssid) {
    clear(epd);
    draw_text_centered(epd, 12, 2, "Hi! I'm Mochi.");
    draw_text_centered(epd, 44, 1, "Join this WiFi");
    draw_text_centered(epd, 60, 1, "from your phone:");
    draw_text_centered(epd, 92, 2, ssid);
    draw_text_centered(epd, 132, 1, "Then follow the");
    draw_text_centered(epd, 148, 1, "captive portal.");
    draw_text_centered(epd, 180, 1, "192.168.4.1");
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
    draw_text_centered(epd, 12, 2, "Pair me!");
    draw_text_centered(epd, 48, 1, "Visit on your phone:");
    draw_text_centered(epd, 68, 1, "mochi.val.run");
    draw_text_centered(epd, 84, 1, "/pair-device");
    draw_text_centered(epd, 116, 1, "Enter this code:");
    /* Code at scale 2 = 16-pixel-tall block. The MOCHI- prefix +
     * 4 random chars = 10 chars * 16px = 160px wide, fits with
     * 20px margin either side. */
    draw_text_centered(epd, 144, 2, code);
    draw_text_centered(epd, 180, 1, "(case insensitive)");
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

}  /* namespace epd_ui */
