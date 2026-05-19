/*
 * NVS-backed device→pet pairing storage.
 *
 * Two keys in the "pair" namespace: pet_id (the UUID we use as the
 * X-Pet-Id bearer for every subsequent server call) and pet_name
 * (display only). Both written atomically once the user has paired
 * via mochi.val.run/pair-device. See design/04-pairing.md.
 *
 * Wiped together with wifi_creds on factory reset (long-press boot,
 * not yet implemented).
 */

#pragma once

#include <stddef.h>

/* The web prototype uses pet_ids of the form pet_XXXXXXXX-NNN — see
 * c15r/mochi:backend/identity.ts. 64 bytes is plenty. */
#define MOCHI_PET_ID_MAX    64
#define MOCHI_PET_NAME_MAX  24      /* matches validateName() server-side */

struct mochi_pair_creds {
    char pet_id[MOCHI_PET_ID_MAX + 1];
    char pet_name[MOCHI_PET_NAME_MAX + 1];
};

bool pair_creds_load(struct mochi_pair_creds *out);
bool pair_creds_save(const struct mochi_pair_creds *creds);
bool pair_creds_clear(void);
