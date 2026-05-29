# 08 — OTA firmware updates

**Status:** M14, landed alongside this doc.
**Predecessors:** [01-bring-up-plan.md](./01-bring-up-plan.md),
[02-boot-sequence.md](./02-boot-sequence.md).

## Why

Through M13 the only path to new firmware was USB. That's tolerable
during bring-up — the device sits a metre from the toolchain — but
it's a bad steady state for an artwork-on-the-shelf pet. We want to
ship a bug fix or a new scene contract by bumping a checked-in
version and having every device in the wild pull it in by the next
morning.

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

`.github/workflows/firmware-release.yml`. The release version is the
single source of truth in **`firmware/version.txt`** (e.g. `0.3.0`),
which ESP-IDF reads into `PROJECT_VER` → `esp_app_get_description()->version`.
No hand-tagging.

Two channels, matching what `ota_channel` fetches on-device:

- **stable** — on push to `main` (or a manual `workflow_dispatch`). Reads
  `version.txt`; if no release for `v<version>` exists yet, builds and
  publishes it as the *latest* release and tags `v<version>`. Pushes that
  don't change the version are a no-op, so the flow is: **bump
  `version.txt` in a PR → merge → released**.
- **beta** — on pull requests into `main`. Builds with version
  `<version.txt>-beta.<run_number>` baked in and refreshes a single
  rolling **pre-release** with the fixed tag `beta`. The fixed tag gives
  the device a stable URL (`/releases/download/beta/latest.json`); only
  the newest beta build is live. Fork PRs are skipped (no write token).

Both build in the `espressif/idf:release-v5.3` container (matches
`firmware/README.md`'s pinned IDF version) and publish the same three
artifacts to their release:

- `eink-pet.bin` — the application image.
- `bootloader.bin` + `partition-table.bin` — for the one-time USB reflash.
- `latest.json` — the OTA manifest:
  ```json
  { "version": "0.3.0",
    "url": "https://github.com/.../releases/download/v0.3.0/eink-pet.bin",
    "sha256": "..." }
  ```

The manifest `version` is the exact `esp_app_get_description()->version`
the build embedded, so the device's compare is string-for-string with
what it's running.

**Version precedence.** `ota_update`'s comparator is semver-aware
(§11 pre-release ordering): a `-beta.<n>` build sorts *above* the previous
stable but *below* the matching release. So a beta device rolls forward
through betas and lands on the final stable when it ships. Practically:
bump `version.txt` to the next target in the PR, so its betas (e.g.
`0.3.0-beta.42`) are ahead of current stable (`0.2.x`).

**Stable release notes.** The workflow appends a git-log changelog
(previous `v*` tag → HEAD) plus GitHub's PR-based "What's Changed" to
whatever was already drafted on the release (`append_body`), rather than
overwriting it.

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
ota_update::start_background_task(stable_url, beta_url)
 ↓                                       └──► sleeps 30 s, then polls
[existing boot path: RTC, SHTC3,           the channel's manifest
 voice init, sprite cache, pairing,        (ota_channel_get()); if it's
 first render, touch loop]                 newer than running, streams
 ↓                                         new .bin into inactive slot,
[touch loop]                               flips otadata, sets reboot_ready
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
- **Staged rollout / pet-id gating.** The beta channel (dev menu →
  RISK → `Channel: beta`, persisted in NVS via `ota_channel`) is the
  manual first step: opt a device into pre-release builds. A future
  val.town `/api/firmware/latest` could go further — proxy to GitHub
  and gate by pet_id (e.g. ring-0 dev devices first) automatically.
- **Update on USB-power only.** We could gate downloads on the
  battery sense reading >4 V (i.e. plugged in) to avoid draining a
  near-empty LiPo on a multi-MB download.
