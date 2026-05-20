# 10 — QR codes for provisioning + pairing (exploration)

**Status:** exploration, 2026-05-20. Not a milestone yet.
**Predecessors:** [03-provisioning.md](./03-provisioning.md),
[04-pairing.md](./04-pairing.md).

## Why

Both provisioning (M3) and pairing (M5) currently demand the user
type a string off the e-paper into their phone:

- M3: a SoftAP SSID (`Mochi-7048`) — eight characters in WiFi
  settings.
- M5: a pairing code (`MOCHI-A4B2`) — typed into the form on
  `mochi.val.run/pair-device` after signing in.

Both phones now have native QR scanning from the Camera app (iOS 11+,
Android 9+). A QR code on the e-paper replaces those two
typing steps with a point-and-tap, and the e-paper happens to be an
ideal QR surface — high-contrast 1-bit, no glare, persistent without
power, and 200×200 px is more than enough for the payloads involved.

This doc scopes the work; it's not committing to a milestone yet.

## Two payloads, one infrastructure

### 1. Provisioning QR — WIFI: URI

iOS Camera and Android Camera both recognise the
[WIFI: URI scheme](https://en.wikipedia.org/wiki/QR_code#Joining_a_Wi-Fi_network)
and offer "Join Network" on tap.

```
WIFI:T:nopass;S:Mochi-7048;P:;H:false;;
```

- `T:nopass` — our SoftAP is open (matches the current `wifi_prov.cpp`
  config; we rely on iOS to flag it as captive once joined).
- `S:Mochi-XXXX` — SSID, where XXXX is the last two MAC bytes (same
  format `wifi_prov.cpp:56` builds today).
- `P:` — empty, since open.
- `H:false` — visible SSID.

Payload length: ~32 chars depending on the MAC bytes. Fits in
QR Version 2 (25 × 25 modules) at error-correction level L. Larger
correction levels still fit comfortably below Version 4.

Once the phone joins the SoftAP the captive-portal handlers in
`wifi_prov.cpp` take over unchanged — the QR replaces only the
"open Settings → WiFi → tap Mochi-XXXX" friction, not the portal
itself.

### 2. Pairing QR — deep link to `pair-device?code=…`

```
https://mochi.val.run/pair-device?code=MOCHI-A4B2
```

Payload: 51 chars. Fits in QR Version 3 (29 × 29 modules) at EC
level L; comfortably in Version 4 (33 × 33) at level M.

This needs a small server change in `c15r/mochi`:
`/pair-device` is currently a plain form. We extend the static HTML
to read `code` from `URLSearchParams` and pre-fill the code field
when present (and ideally focus the PIN field directly). The
backend `POST /api/pair-device` contract doesn't change — the user
still authenticates with name+PIN; the QR only saves typing the
six-character code, which is the part that's most awkward to read
off a 200×200 screen.

The PIN deliberately stays user-typed. Encoding pet credentials in
a QR scanned off the device would defeat the M5 constraint that
"device should not see the PIN" (see
[04-pairing.md § Constraints](./04-pairing.md)) — the QR would
be at least as much of a credential as the PIN itself.

## Rendering on the panel

### Library

[Nayuki's `qrcodegen` (C version)](https://github.com/nayuki/QR-Code-generator) —
single-file, MIT, ~20 KB compiled, generates a module bitmap. Drop
into `firmware/main/vendor/qrcodegen/` with a provenance note next
to the existing `vendor/waveshare-eink/` directory; same vendoring
pattern. The library is pure computation, no I/O, no allocation
from heap — exactly the shape we want next to `font8x8.cpp`.

Alternative: `ricmoo/qrcode` (~3 KB). Smaller but the Version-≤40
cap and slightly less ergonomic API don't justify the saving on an
8 MB-flash device.

### Drawing into the framebuffer

`epd_ui.cpp` gains a `draw_qr(epd, x, y, scale, payload)` helper:

1. Allocate `uint8_t modules[177*177]` on the main task stack
   (Version 40 worst case; in practice Version 3-4 = 33×33). Worst
   case ~31 KB which is too much for the main stack — heap-alloc
   the working buffer instead, free after render.
2. Call `qrcodegen_encodeText(payload, …, modules)`.
3. For each module, draw a `scale × scale` block via the existing
   slow `EPD_DrawColorPixel` path. This is the same write pattern
   `draw_text` uses; fine for a static screen (the QR refreshes
   on state changes only, never per-frame).

### Panel layout sketches

Both screens reuse the existing `epd_ui::draw_text_centered` for
captions; the QR sits in a generous square above.

```
PROVISIONING (M3)                    PAIRING (M5)
┌──────────────────────┐            ┌──────────────────────┐
│  Hi! I'm Mochi.      │            │   Hi! I'm Mochi.     │
│                      │            │                      │
│   ┌──────────────┐   │            │   ┌──────────────┐   │
│   │              │   │            │   │              │   │
│   │   [QR  WIFI: │   │            │   │   [QR  URL]  │   │
│   │    URI ]     │   │            │   │              │   │
│   │              │   │            │   │              │   │
│   └──────────────┘   │            │   └──────────────┘   │
│                      │            │                      │
│  Scan to join        │            │  Scan, or visit      │
│  Mochi-7048          │            │  mochi.val.run/pair  │
│                      │            │  Code: MOCHI-A4B2    │
└──────────────────────┘            └──────────────────────┘
```

The text fallback is intentional: someone with an older phone, or
QR scanning disabled, can still complete the flow by typing. We're
adding a faster path, not removing the existing one.

QR sizing: 29 modules × 4 px = 116 px square, with 8 px quiet
zone each side → ~132 px allocated. Leaves ~60 px of vertical
space for the caption above + below. Comfortable on the 200×200
panel.

## Scope of work

Effort estimate: ~6 hours total, split:

1. **Vendor qrcodegen** + add to `CMakeLists.txt`. (30 min.)
2. **`epd_ui::draw_qr` helper** with heap-alloc → encode → blit →
   free. (1 hr including pixel-block loop and a small fixture in
   `factory_reset`-style debug path.)
3. **`render_prov_idle` extension** to draw QR above the existing
   "Mochi-XXXX" text. (30 min.)
4. **`render_pair_prompt` extension** likewise. (30 min.)
5. **Server-side `?code=` autofill** in `c15r/mochi`'s
   `pair-device` page. (30 min.)
6. **Bench validation:** both QR codes scan from a real iPhone
   and a real Android phone; iOS auto-joins SoftAP; Android
   captive portal fires; pair URL deep-link pre-fills the code
   field. (2-3 hrs of soak across both phones.)

Risks:

- **iOS captive portal interaction with QR-joined networks.** When
  iOS joins an open network from a QR, it still runs the captive
  probe — should be identical to the manual-join path
  (`hotspot-detect.html` → 302 → portal). Worth verifying on a
  device that hasn't seen the SoftAP before.
- **Old Android camera apps** (pre-Pixel 4 / pre-2019) sometimes
  don't recognise the `WIFI:` scheme out of the box. Google Lens
  always does; the stock Camera on older Samsung/Xiaomi is the
  weak spot. Fallback text remains the safety net.
- **Refresh cost.** Drawing a 29 × 29 × 4 px QR through
  `EPD_DrawColorPixel` is ~13 k pixel calls — same order as
  `draw_text` for a long line. Currently ~800 ms in the slow
  path (see `01-bring-up-plan.md` M2 "Known cost"). Acceptable
  for a once-per-screen render; not acceptable if we ever animate
  the QR (we won't).

## Not in scope here

- **QR for OTA pinning** or firmware version surfacing. Out of
  scope; OTA is automatic per `08-ota-updates.md`.
- **Encoding pet credentials in QR.** Explicitly out — see
  "Pairing QR" above.
- **A "show QR again" gesture.** v1 is "QR is on the screen
  whenever the device is in the corresponding state"; richer UX
  is a follow-up if the bench shows it's needed.
- **Tracking which pair flow succeeded via the QR vs typed.**
  No analytics; the value is the UX, not the metric.

## Cross-references

- `03-provisioning.md` — M3 SoftAP + captive portal, where the
  provisioning QR sits on top
- `04-pairing.md` — M5 pair flow, where the pairing QR sits on
  top; "device should not see the PIN" constraint lives here
- `firmware/main/wifi_prov.cpp:56` — SSID format the QR mirrors
- `firmware/main/device_pair.cpp:104` — `request_code()` returning
  the `MOCHI-XXXX` string the pairing QR embeds
- `firmware/main/epd_ui.h:36,66` — `render_prov_idle` and
  `render_pair_prompt` are the two existing entry points that
  grow a QR
- Nayuki qrcodegen: <https://github.com/nayuki/QR-Code-generator>
