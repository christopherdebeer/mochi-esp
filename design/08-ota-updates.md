# 08 — OTA firmware updates

**Status:** M14, landed alongside this doc.
**Predecessors:** [01-bring-up-plan.md](./01-bring-up-plan.md),
[02-boot-sequence.md](./02-boot-sequence.md).

## Why

Through M13 the only path to new firmware was USB. That's tolerable
during bring-up — the device sits a metre from the toolchain — but
it's a bad steady state for an artwork-on-the-shelf pet. We want to
ship a bug fix or a new scene contract by tagging a release and
having every device in the wild pull it in by the next morning.

## Constraints

- 8 MB flash, ESP32-S3-PICO-1. No second chip, no SD-card-driven
  rescue path.
- Wall power is sometimes USB, often a LiPo. Updates must not soft-brick
  the device if power dies mid-flash.
- We don't want a separate fleet server. GitHub Releases is the
  artifact store + version source of truth.

## Partition table

Pre-M14 layout was a single 2 MB `factory` slot + ~6 MB `storage`.
M14 swaps factory for the canonical OTA trio:

```
nvs       0x9000   16 KB
otadata   0xd000    8 KB  (two sectors, A/B copies)
phy_init  0xf000    4 KB
ota_0     0x10000   2 MB
ota_1     0x210000  2 MB
storage   0x410000  ~4 MB  (LittleFS — sprite cache + event log)
```

Storage shrank from ~6 MB to ~4 MB. The current sprite cache lives
well under 1 MB, so this is comfortable; M12's event log won't
push us anywhere near the limit either.

**The first install of an OTA-capable image must come over USB.** A
new bootloader + new partition table can't be staged from the old
image's runtime. After that one reflash, all further updates go OTA.
The reflash wipes LittleFS too (it moves from 0x210000 → 0x410000),
so first-boot pays the full sprite-cache-cold cost once.

## Release pipeline

`.github/workflows/firmware-release.yml`:

- Triggered by any `v*` tag push.
- Builds in the `espressif/idf:release-v5.3` container (matches
  `firmware/README.md`'s pinned IDF version).
- Publishes three artifacts to the GitHub Release:
  - `eink-pet.bin` — the application image.
  - `bootloader.bin` + `partition-table.bin` — for users doing the
    one-time USB reflash.
  - `latest.json` — the OTA manifest:
    ```json
    { "version": "0.1.0",
      "url": "https://github.com/.../releases/download/v0.1.0/eink-pet.bin",
      "sha256": "..." }
    ```

The version field comes from the tag (`v0.1.0` → `0.1.0`). On the
device the same string is exposed via `esp_app_get_description()->version`
(ESP-IDF defaults to `git describe` during build, and the CI checkout
preserves tag history).

## Device flow

Code: `firmware/main/ota_update.{h,cpp}`. Wired in
`firmware/main/main.cpp` right after the already-paired branch's
WiFi join.

```
boot
 ↓
[wifi_sta::connect_any] ────────► (succeeds)
 ↓
ota_update::mark_valid_if_pending()    ← promotes a PENDING_VERIFY image
 ↓
ota_update::start_background_task(MOCHI_OTA_MANIFEST_URL)
 ↓                                       └──► sleeps 30 s, then
[existing boot path: RTC, SHTC3,           polls manifest;
 voice init, sprite cache, pairing,        if version differs from
 first render, touch loop]                 running, streams new .bin
 ↓                                         into inactive slot, flips
[touch loop]                               otadata, sets reboot_ready
 ↓
if ota_update::reboot_ready() && !voice::is_active()
   && idle for ≥60 s:
       esp_restart()
```

**Rollback.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` means a
freshly-installed image is marked `PENDING_VERIFY`. The bootloader
auto-falls back to the previous slot on the next reset unless the
new image promotes itself to `VALID` first. The promotion happens
in `mark_valid_if_pending()`, called right after WiFi joins —
i.e. only when provisioning, NVS, the STA stack, the radio and DHCP
all worked. Crashes earlier than that cost a reboot but no
permanent install.

**Idle gate.** The reboot happens only when no voice session is
active and there's been no touch in the last 60 s. That avoids
yanking the screen mid-tap or mid-conversation. A device that's
genuinely abandoned for hours updates silently overnight.

**Update interval.** 30 s settle delay at boot, then 24 h between
manifest polls. The settle delay matters because the boot-time
sprite fetches saturate the radio for ~10 s; we don't want OTA's
TLS handshake competing.

## Future extensions

- **Forced check.** A long-press in a dev-only gesture (or a touch
  combo) could wake the background task for an out-of-band poll.
  Useful for fleet testing without waiting 24 h.
- **Sha256 verification.** The manifest carries `sha256` but the
  device doesn't verify it yet — `esp_https_ota`'s built-in image
  header validation is what we rely on for corruption detection.
  Adding sha256 closes a small gap (a maliciously crafted image that
  passes IDF header checks but isn't ours).
- **Staged rollout / pet-id gating.** Today every device pulls the
  same manifest. A future val.town `/api/firmware/latest` could
  proxy to GitHub and gate by pet_id (e.g. ring-0 dev devices first),
  with no device-side change.
- **Update on USB-power only.** We could gate downloads on the
  battery sense reading >4 V (i.e. plugged in) to avoid draining a
  near-empty LiPo on a multi-MB download.
