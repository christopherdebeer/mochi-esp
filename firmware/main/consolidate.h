/*
 * consolidate — on-device sleep consolidation (design/19, server-
 * orchestrated; mirrors imagine.c's shape).
 *
 * When mochi.val.run advises consolidation (the `consolidationAdvised`
 * hint already rides on /api/state, surfaced by pet_sync) and the pet
 * is asleep, a reflection pass runs with the user's BYO OpenAI key,
 * which never leaves the device:
 *
 *   1. orchestration  GET  /api/consolidate/orchestration  → server-built
 *                          system+user prompt + model + params
 *   2. generate       POST api.openai.com/v1/chat/completions (JSON mode)
 *   3. persist        POST /api/consolidate/persist  (raw LLM result)
 *   4. telemetry      POST /api/usage/event  (kind=consolidate)
 *
 * The server computes the prompt (the pet-voice/privacy prompt in
 * shared/consolidate-client.ts) and re-validates the result on persist;
 * the firmware just runs the chat call with the BYO key and forwards
 * the structured output. Only the transcript-derived text crosses to
 * mochi.val.run (it already holds it); the OpenAI key stays in NVS.
 * Same trust boundary as voice + imagine.
 *
 * Concurrency: one pass at a time, refused while in flight or inside a
 * debounce window. The caller (main.cpp) gates on asleep + no voice /
 * imagine in flight (PSRAM budget + "reflect during rest, not mid-
 * conversation"); this module enforces the in-flight + debounce + key
 * guards. Silent/background — no UI phase machine (unlike imagine).
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the worker task + queue. Idempotent; returns true on success. */
bool consolidate_init(void);

/* Kick a consolidation pass. Returns false (no-op) if one is already in
 * flight or inside the debounce window. Eligibility is the server's call
 * (the orchestration step re-checks and returns eligible=false to skip);
 * the caller is expected to gate on asleep + no voice/imagine overlap. */
bool consolidate_start(void);

/* True while a pass is in flight. */
bool consolidate_in_flight(void);

#ifdef __cplusplus
}
#endif
