/*
 * key_portal — minimal local-network HTTP form for setting the
 * BYO OpenAI key without re-running provisioning.
 *
 * Why this exists: factory reset (PWR+BOOT 10s) is too coarse for
 * "I forgot to type my key in the captive portal". A user who's
 * already paired + connected to wifi shouldn't have to redo all
 * three to fix one missing field.
 *
 * Lifecycle:
 *   start() — spin up esp_http_server on :80, render QR + IP +
 *             "Tap to dismiss" on the e-paper. Idempotent.
 *   stop()  — shut server down, restore the previous render. Safe
 *             to call when not active.
 *   active() — true while the server is running.
 *
 * Triggers (driven by main.cpp):
 *   - boot path observes openai_key absent → start()
 *   - sleep_gesture::triple_tap_requested() at any time → start()
 *
 * Dismiss:
 *   - any touch event while active → stop()
 *   - successful POST /key → render success → 2s → stop()
 *   - 5min idle → stop() (safety)
 *
 * Threading: the http_server runs its own task. start()/stop() are
 * meant to be called from the main task. The success-auto-stop path
 * uses a small deferred timer rather than blocking inside the
 * handler — keeps the response flushing cleanly to the client.
 */

#pragma once

#include "epaper_driver_bsp.h"

namespace key_portal {

/* Spin up the form server + render the recovery screen. The IP is
 * read from esp_netif at start() time and embedded in the QR. */
void start(epaper_driver_display *epd);

/* Tear the server down + clear our active state. Caller is
 * responsible for restoring the next screen — we don't redraw the
 * pet ourselves because we don't own the compositor. */
void stop();

/* True while the http server is up. */
bool active();

/* Periodic tick from main's touch loop. Drives the idle-timeout
 * shutdown + the post-submit auto-stop. */
void tick();

}  /* namespace key_portal */
