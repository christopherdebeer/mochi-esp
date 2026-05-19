/*
 * NVS-backed OpenAI API key storage.
 *
 * Single key in the "openai" namespace, separate from wifi creds and
 * pair creds. The architectural rule is in design/07-voice-architecture.md:
 * the device authenticates direct to api.openai.com using a BYO key
 * stored here; mochi.val.run never sees this value.
 *
 * Cleared independently of wifi creds and pair creds. Factory reset
 * (PWR + BOOT held 10 s in factory_reset.cpp) wipes all three.
 */

#pragma once

#include <stddef.h>

/* OpenAI keys today: 'sk-' (3) + project segment + '-' + ~150 chars
 * of opaque token. Max observed ~190 chars; 256 is comfortable. */
#define MOCHI_OPENAI_KEY_MAX  256

bool openai_key_load(char *out, size_t out_len);
bool openai_key_save(const char *key);
bool openai_key_clear(void);
