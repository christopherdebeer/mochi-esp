# 04 — Device pairing

Status: draft, 2026-05-18 (informs M5)

How a Mochi device gets bound to a pet. Distinct from provisioning
(`03-provisioning.md`) — provisioning is "device joins WiFi", pairing
is "device knows whose pet it represents."

## Framing

The web prototype `c15r/mochi` already has an identity model: pets are
identified by `pet_id` (a UUID), authenticated by `(name, pin)`, and
the bearer for every API call is the `X-Pet-Id` header. This is what
the React frontend uses. Pairing **does not invent a new identity
substrate** — it reuses this one.

What pairing adds is a *second* client (the device) to that substrate,
and a way to bind a specific physical device to a specific pet without
making the user enter their PIN on a 200×200 e-paper that has no
keyboard.

## Constraints that shape the protocol

1. **Device has no keyboard.** Anything the user types must be typed on
   their phone, not on the device.
2. **Device should not see the PIN.** Reduces blast radius if a device
   is compromised or stolen — losing the device should not leak the
   pet's master credentials. This rules out "user types name+PIN into
   a form served by the device."
3. **The phone is signed in to mochi.val.run already** (or can be).
   That's where it makes sense for credentials to be entered.
4. **First-pair must work without prior server-side device knowledge.**
   The server has no idea this hardware exists until it shows up.

## Protocol

A two-actor handshake mediated by a short-lived pairing code:

```
[device, post-provisioning, no pairing in NVS]
   POST mochi.val.run/api/device/pair-init { hw_id }
   ← { code: "MOCHI-A4B2", expires_at: ... }
   e-paper renders:
     "Hi! I'm Mochi."
     "Visit mochi.val.run/pair-device"
     "Code: MOCHI-A4B2"
   poll loop:
     every 5s: GET /api/device/pair-check?hw_id=...
       → { paired: false }       (keep waiting)
       → { paired: true, pet_id, pet_name }   (done!)

[user, on phone or laptop]
   Visit mochi.val.run/pair-device
   → Page asks: "Which pet?" (enter name + PIN, same as login)
   → Page asks: "Which device?" (enter MOCHI-A4B2)
   → Submit → server matches code to pending row, writes the bind

[device, next poll]
   GET /api/device/pair-check?hw_id=...
   ← { paired: true, pet_id, pet_name }
   write NVS pair namespace
   reboot
   warm-boot path takes over (M5 stops here; M11+ uses pet_id for sync)
```

Only the **user**'s side ever sees the PIN. The device never does.

## Server endpoints

```
POST /api/device/pair-init
  body: { "hw_id": "70041ddbf948" }
  → 200 { "code": "MOCHI-A4B2", "expires_at": <unix> }

  Creates a row in `device_pairings` with status='pending', code, hw_id,
  created_at, expires_at = created_at + 10min. If a pending row already
  exists for this hw_id, return its existing code (idempotent retry).
```

```
GET /api/device/pair-check?hw_id=70041ddbf948
  → 200 { "paired": false }                                    (still pending)
  → 200 { "paired": true, "pet_id": "...", "pet_name": "..." } (now paired)
  → 410 { "error": "expired" }                                 (>10min stale)
```

```
POST /api/pair-device
  body: { "name": "Mochi", "pin": "1234", "code": "MOCHI-A4B2" }
  → 200 { "pet_name": "Mochi" }
  → 401 { "error": "name + pin doesn't match a pet" }
  → 404 { "error": "no pending device with that code" }
  → 410 { "error": "code expired" }
  → 429 { "error": "rate limited" }

  Server flow:
    1. findClaimByNameAndPin(name, pin) — reuses existing identity.ts
    2. lookup device_pairings WHERE code=? AND status='pending'
    3. atomic update: status='paired', pet_id=<from step 1>, paired_at=now
  Rate-limited via the same claim_attempts table mochi already uses.
```

```
GET /pair-device
  Static HTML page: form that hits POST /api/pair-device. Mirrors the
  signin UX the React frontend already has. No new auth model.
```

## Code shape

A 6-character code with a `MOCHI-` prefix and 4 random alphanumerics
from a confusable-free alphabet (no 0/O/1/I/L). Avoid I/O confusables
because the user has to read the code off an e-paper screen and type
it on a phone; entropy of 4 chars × 28-letter alphabet ≈ 19 bits is
fine for a 10-minute window with rate limiting.

Format: `MOCHI-A4B2`. Display + form input both case-insensitive.

## NVS storage on device

```
namespace: "pair"
  pet_id    : str (UUID, 36 chars)
  pet_name  : str (display only — doesn't gate anything)
```

Stored alongside `wifi` in the same NVS partition we already have. Both
namespaces are wiped together on factory reset.

## Why we record `hw_id` server-side

The bearer for substrate access is `pet_id` (via `X-Pet-Id`), exactly
like the web client. `hw_id` is *audit + revoke*, not auth.

Two reasons to log it:

1. **Revoke-by-device.** If a device is lost, the user can revoke that
   specific `hw_id` server-side without affecting their phone's web
   access. Subsequent device API calls return 410 → device wipes NVS
   and re-pairs.
2. **Multi-device understanding.** When we later want "which devices
   are bound to this pet?" the `device_pairings` table answers it.

Multiple devices can pair to the same pet — both behave as peer
substrate clients exactly as the web frontend does. No exclusivity
handshake.

## E-paper screens

Final-state-interpretable per `02-boot-sequence.md`'s crash rules:

| State                  | Screen content                                         |
|------------------------|--------------------------------------------------------|
| WAITING-FOR-PAIRING    | "Visit mochi.val.run/pair-device — Code: MOCHI-A4B2"   |
| Code expired           | "Code expired. Press to get a new one."                |
| Pairing successful     | "Hi <pet_name>!" → reboots to warm boot                |
| Network error          | "Couldn't reach Mochi. Check your WiFi."               |

All updates are partial refreshes. The canonical pet sprite (placeholder
species, no costume) sits above the status text — same shape as the
post-STA prompt in shipped firmware.

## What M5 does NOT do

- **Multi-pet plurality on one device.** One device, one pet, hard.
  Multi-pet sketched in `00-architecture.md` decision #3 but deferred.
- **Self-revoke from the device.** The factory-reset path (long-press
  boot) wipes NVS but doesn't tell the server. Acceptable for M5; a
  clean revoke needs the post-M5 server UI.
- **The user-side revoke UI.** That ships when there's >1 device per
  pet to revoke.
- **Pre-paired-from-factory.** Future bulk shipment might come with a
  pet_id already programmed. Not relevant for one-of-one.

## Cross-references

- `02-boot-sequence.md` — WAITING-FOR-PAIRING branch
- `03-provisioning.md` — what happens *before* pairing
- `c15r/mochi:design/identity.md` — name + PIN model the device reuses
- `c15r/mochi:backend/identity.ts:findClaimByNameAndPin` — primitive
  the new `/api/pair-device` endpoint wraps
