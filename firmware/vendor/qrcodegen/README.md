# Vendored: Nayuki QR Code generator (C)

A frozen snapshot of the C implementation of Nayuki's QR-Code-generator
library. Used by the firmware to draw WiFi-join and pairing-deep-link
QR codes on the e-paper during provisioning (M3) and pairing (M5);
see `firmware/main/qr.{cpp,h}` for the device-side draw helper and
`design/10-qr-codes.md` for the rationale.

## Provenance

- Repo: <https://github.com/nayuki/QR-Code-generator>
- Branch / commit: `master` (HEAD at fetch time)
- Date pulled: 2026-05-20
- License: MIT — see header banner inside each file.

## What's here

- `qrcodegen.h` / `qrcodegen.c` — copied verbatim from the upstream
  `c/` directory. No local modifications.

The library is pure computation, no heap allocations, no I/O. Caller
supplies two `uint8_t[]` buffers of size
`qrcodegen_BUFFER_LEN_FOR_VERSION(maxVersion)` — one for the output
QR module bitmap, one for scratch. The firmware allocates both on
the heap (max ~600 bytes total at our chosen `maxVersion = 5`) for
the duration of each render and frees immediately after.

## When to revisit

The upstream library is stable and the API is unlikely to change.
Refresh only if a security advisory lands against this code (it
doesn't process untrusted network input on device — only well-formed
strings the firmware constructs itself — so the attack surface is
near zero).
