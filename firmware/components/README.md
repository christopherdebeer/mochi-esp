# Vendored components

Local copies of source-only components from
[`espressif/esp-webrtc-solution`](https://github.com/espressif/esp-webrtc-solution).
These aren't published to the ESP Component Registry, so we keep them
in-tree pinned to a specific upstream commit.

## Provenance

Imported from `espressif/esp-webrtc-solution` at commit
`4e5419c1ec3e0750108009b7191536684ac129b5` (2026-05).

| Component | Source path | Notes |
|---|---|---|
| `codec_board` | `components/codec_board` | Pin-map + I²C/I²S init for the audio codec, registry-published as `tempotian/codec_board` but we **vendor** it because `board_cfg.txt` is `EMBED_TXTFILES`-baked at build time and we needed to add a `WAVESHARE_S3_EPAPER_1_54` entry — see the bottom of `components/codec_board/board_cfg.txt`. |

The other components from `esp-webrtc-solution`'s `components/` tree
are pulled from the ESP Component Registry instead, as managed
dependencies declared in `firmware/main/idf_component.yml` (or
transitively, via `esp_webrtc/idf_component.yml`):

| Registry path | Role |
|---|---|
| `espressif/esp_peer` | The WebRTC stack itself (prebuilt blob, target-specific) |
| `tempotian/media_lib_utils` | Threading, locking, malloc shims |
| `tempotian/av_render` | Audio playback backend (used by `esp_webrtc`'s audio path) |
| `espressif/esp_capture` | Audio capture frontend |
| `espressif/esp_codec_dev` | Codec abstraction |
| `espressif/nghttp` | HTTP/2 client (signalling) |
| `espressif/esp_websocket_client` | WebSocket client |

Pulling those from the registry saves ~10 MB vs vendoring the full
multi-target tree (esp_peer's prebuilt is ~1.2 MB per target × 7
targets in the upstream repo).

## Why these aren't on the registry

Espressif published `esp_peer` standalone but treats the upper-layer
helpers as part of the `esp-webrtc-solution` repo, intended to be
copied into project trees. There's no upstream packaging plan
visible. We pin the commit so future Claudes know exactly which
upstream the local copies match.

## Updating

To re-sync against a newer upstream:

```sh
cd /tmp && git clone --depth 1 https://github.com/espressif/esp-webrtc-solution.git ews
cp -r /tmp/ews/components/esp_webrtc        firmware/components/
cp -r /tmp/ews/components/webrtc_utils      firmware/components/
cp -r /tmp/ews/components/media_lib_utils   firmware/components/
```

Then update the commit hash in this README. Verify the
`espressif/esp_peer` registry version still matches (look at
`components/esp_peer/idf_component.yml` upstream — the repo's
declared version should equal the registry's).

## Not vendored

- `av_render`, `codec_board` — board-specific abstractions for Korvo
  hardware. We bypass them and write a minimal Waveshare-specific
  audio glue layer using the pin map already in `main/board_pins.h`
  (see M9 in `design/01-bring-up-plan.md`).
- `esp_peer` — registry-installed (see above).
