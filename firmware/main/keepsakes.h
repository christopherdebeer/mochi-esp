// Keepsakes — device-native discovery collection (design/33).
//
// Offline-first store of which keepsakes this device has pocketed, backed
// by a single NVS u32 bitmask keyed by table index. The keepsake DEFINITIONS
// (id + short display name) live here as a small table mirroring the
// substrate registry (shared/keepsakes.ts) so the backpack can render names
// offline; the id strings must match the registry (they're the payload of an
// MPK_ACTION_COLLECT zone). See backend/keepsakes.ts for the server side.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Number of known keepsakes (table size).
int keepsakes_total(void);
// id / short display name for table index, or NULL if out of range.
const char *keepsakes_id(int idx);
const char *keepsakes_name(int idx);
// Table index for an id, or -1 if unknown (a stray/garbage zone).
int keepsakes_index(const char *id);

// Has this device collected keepsake `idx`?
bool keepsakes_have(int idx);
// Record keepsake `idx` as collected. Returns true if it was newly added
// (false if already had it or idx invalid). Persists to NVS.
bool keepsakes_add(int idx);
// Count of collected keepsakes.
int keepsakes_count_collected(void);

// The raw collected bitmask (bit i = table index i). For sync/merge.
uint32_t keepsakes_mask(void);
// OR server-known collected bits into the local set (server→device merge).
void keepsakes_merge_mask(uint32_t mask);

#ifdef __cplusplus
}
#endif
