# 03 — Provisioning (SoftAP + captive portal)

Status: draft, 2026-05-17

How the device gets onto the user's home WiFi without a keyboard.

## Decision: SoftAP + captive portal

Three viable approaches were considered:

1. **SoftAP + captive portal** ← chosen
2. **BLE provisioning via mochi.val.run web app** — killed by iOS Safari
   not supporting Web Bluetooth. Revisit if/when that changes.
3. **WPS / SmartConfig** — proprietary, brittle, fading from vendor
   support. Skip.

SoftAP + captive portal works on every phone OS, requires no app install,
and uses well-trodden ESP-IDF components (`wifi_provisioning` with the
`scheme_softap` transport). The UX is familiar to anyone who has set up
a smart bulb or thermostat.

## UX flow

```
[device, first power-on]
  e-paper renders:
    "Hi! I'm Mochi."
    "Join this WiFi to finish setup:"
    "  Name: Mochi-XXXX"
    "  Or visit: http://mochi.local"

[user, on iPhone]
  Settings → WiFi → joins "Mochi-XXXX"
  → iOS detects no internet, auto-launches captive portal
  → user sees web page hosted by device:
        "Pick your home WiFi:"
        [list of nearby SSIDs, scanned by device]
        [password field]
        [Connect]
  → enters password, taps Connect

[device]
  saves wifi_creds to NVS
  e-paper updates: "Got it! Connecting..."
  reboots
  joins home WiFi
  e-paper updates: "Online. Now let's pair me to a pet →"
  enters WAITING-FOR-PAIRING state (see 02-boot-sequence.md)
```

## Captive portal mechanics

iOS and Android both detect captive portals by probing a known URL on
join. If the response is not what they expect, they auto-launch a
browser to the device-served portal.

| Platform   | Probe URL                                  | Expected response |
| ---------- | ------------------------------------------ | ----------------- |
| iOS        | `http://captive.apple.com/hotspot-detect.html` | `<HTML><BODY>Success</BODY></HTML>` |
| Android    | `http://connectivitycheck.gstatic.com/generate_204` | HTTP 204 no content |
| Windows    | `http://www.msftconnecttest.com/connecttest.txt` | `Microsoft Connect Test` |

Strategy: device runs a DNS hijack — every query resolves to the device's
SoftAP IP (`192.168.4.1` by default in ESP-IDF). The HTTP server returns
a redirect (HTTP 302 to `http://mochi.local/portal`) for the probe URLs,
which triggers the captive portal launch on iOS and Android.

A complication: iOS sometimes caches "captive portal complete" state. If
the user joins "Mochi-XXXX", completes the portal, but then forgets the
network and re-joins, iOS may *not* auto-launch the portal again. We
handle this by:
- Detecting the case in the portal page (device knows whether it has
  wifi_creds already)
- Showing "Already configured. Forget this network on your phone to
  re-pair." as the page content

## Device-side state

```
enum prov_state {
    PROV_IDLE,           // no SoftAP, normal STA mode
    PROV_AP_RUNNING,     // SoftAP up, no client connected
    PROV_AP_CLIENT,      // a phone is connected to SoftAP
    PROV_PORTAL_OPEN,    // captive portal page being served
    PROV_RECEIVED_CREDS, // user submitted form, validating
    PROV_VALIDATING,     // briefly switching to STA to test creds
    PROV_SUCCESS,        // creds work, will reboot
    PROV_FAILED,         // creds don't work, retry
};
```

The `PROV_VALIDATING` state is important: before persisting creds to NVS
and rebooting, the device briefly tries to join the user's WiFi with the
provided creds. If join fails (wrong password, weak signal), the device
returns to the SoftAP and tells the user. This catches the common case
of mistyped passwords without a frustrating reboot cycle.

## NVS schema

```
namespace: "wifi"
  ssid     : str
  password : str
  bssid    : optional, bytes (6) — pin to specific AP if user wants
```

Storing password in plaintext is the ESP-IDF default and acceptable for
this threat model (anyone with physical device access could read flash
anyway; we're not protecting against that).

## E-paper screens during provisioning

Each screen is final-state-interpretable (per `02-boot-sequence.md` crash
rules):

| State              | Screen content                                                |
| ------------------ | ------------------------------------------------------------- |
| `PROV_AP_RUNNING`  | "Hi! I'm Mochi. Join Mochi-XXXX on your phone."               |
| `PROV_PORTAL_OPEN` | (no change — user is on phone, device screen idle)            |
| `PROV_VALIDATING`  | "Trying your WiFi…"                                           |
| `PROV_SUCCESS`     | "Got it! See you in a moment." (then reboot)                  |
| `PROV_FAILED`      | "That password didn't work. Try again on your phone."         |

Partial refresh for transitions between provisioning screens (~300ms).
Full refresh only on reboot.

## What this milestone (M3) does NOT do

- **mDNS / `mochi.local` resolution.** Nice-to-have for the captive
  portal URL, but iOS/Android captive portals don't need it — DNS
  hijacking handles the redirect. Defer.
- **Pairing UI in the portal.** The portal only handles WiFi. Pairing
  is a separate flow that happens on mochi.val.run after the device is
  online. Keeps the portal page small and the device-side HTTP server
  simple.
- **Re-provisioning.** If the user's WiFi password changes, they need
  to factory-reset the device (long-press boot, hold for 10s). Smarter
  recovery is a future concern.

## Cross-references

- `01-bring-up-plan.md` — M3 milestone
- `02-boot-sequence.md` — PAIRING branch references this doc
- `04-pairing.md` (TODO) — what happens after WiFi connects
- ESP-IDF docs: `esp_wifi_provisioning`, `esp_netif`, `tcpip_adapter`
