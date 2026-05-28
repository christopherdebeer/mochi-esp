/*
 * model_prefs — NVS-backed selection of the voice (realtime) + text
 * (consolidation) models, with curated option lists.
 *
 * Curated-only (design decision): the device never sends a free-text
 * model id, so every entry in the lists below must be a real, valid
 * model. Index 0 is the default and matches the firmware's historical
 * hardcoded behaviour, so an unset device behaves exactly as before.
 *
 *   voice — speech-to-speech realtime models (openai_signaling.c mint +
 *           the realtime_sessions telemetry row).
 *   text  — chat models for sleep consolidation; mirrors the server
 *           allowlist in c15r/mochi backend/api.ts. The device passes
 *           its choice as ?model= to /api/consolidate/orchestration.
 *
 * The dev_menu "AI models" modal cycles each through its list.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Copy the current selection (NVS value if set + still in the curated
 * list, else the default) into `out`. */
void model_prefs_voice(char *out, size_t cap);
void model_prefs_text(char *out, size_t cap);

/* Advance the selection to the next curated option and persist it. */
void model_prefs_cycle_voice(void);
void model_prefs_cycle_text(void);

#ifdef __cplusplus
}
#endif
