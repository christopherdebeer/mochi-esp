# 26 — Idle tiers & light sleep

Status: firmware landed, 2026-05-28. The telemetry + idle-tier state
machine + automatic light sleep are all implemented. **Light sleep is
now enabled by default** (`CONFIG_MOCHI_LIGHT_SLEEP=y` in
`sdkconfig.defaults`) for the first OTA release that carries it — first
on-hardware validation happens via that release rather than a bench
pass, so the telemetry below is how we confirm it (and OTA rollback
covers a boot failure; a UX regression — e.g. touch not waking from
doze — would need a follow-up release). A measured Live baseline is
recorded below (§Measured baseline) from the existing `device_logs`
health telemetry; §Analysis has the queries to compare against it.

Author note: prompted by the question "is a low-power mode between deep
sleep and live possible?" — yes, and the e-paper + capacitive-touch
architecture makes light sleep the natural intermediate primitive.

## Context

Power today is **binary**:

- **Live** — CPU at full clock (no `CONFIG_PM_ENABLE`, so no DFS / no
  automatic light sleep). The main loop blocks in `touch::wait_event`
  ~1 s at a time (100 ms during voice); the touch poll task wakes at
  10 Hz; the substrate re-projects every 60 s
  (`SUBSTRATE_REFRESH_US`). WiFi runs on IDF's default modem-sleep
  (no explicit `esp_wifi_set_ps`).
- **Deep sleep** — `commit_sleep()` (`sleep_gesture.cpp`) parks the SoC
  on `esp_deep_sleep_start()`, wake on PWR (GPIO18) or BOOT (GPIO0) via
  ext1 any-low. **Waking is a full reboot**: the app restarts and
  rebuilds state from NVS (this is why `pet_sync_restore_snapshot_from_nvs`
  + pre-render-last-place exist — wake == cold boot).

The gap: there is no resting state. Either we burn full power keeping a
static e-paper image lit by a fully-awake SoC, or we drop to a floor
that costs a multi-second reboot to climb out of. A pet toy spends most
of its life *idle but glanceable* — exactly the regime neither state
serves.

The pivotal hardware facts that make an intermediate tier cheap:

1. **The e-paper holds its image with zero power.** A resting screen
   needs no SoC involvement at all — unlike an LCD, "keep showing the
   pet" is free.
2. **Touch already has an ISR on the INT line (GPIO21).** GPIO21 is an
   RTC-capable pin on the ESP32-S3, so it can serve as a light-sleep
   *and* deep-sleep wake source. A finger-tap can wake the SoC in
   microseconds.

Together these mean the SoC can sleep while the pet stays drawn, and a
touch wakes it **with full RAM and live state intact** — no reboot, no
re-pair, no scene re-fetch.

## Goals / non-goals

**Goals**
- Add at least one resting tier between Live and Deep sleep that
  preserves in-RAM state and wakes near-instantly on touch/button.
- Keep the pet glanceable at all times (screen never blanks for idle).
- Behave well on battery; degrade gracefully (no worse than today) when
  USB is plugged in.
- Make the tier transitions observable (diag events) so power can be
  measured.

**Non-goals (this iteration)**
- Replacing deep sleep — PWR-tap → deep sleep stays the explicit "put
  it away" action.
- Animation/refresh changes (orthogonal; see design/05).
- A timer-wake deep-sleep "check in overnight" behavior — possible
  later, tracked as an open question, not built here.

## Proposed power-state model

```
            no touch ~Tdoze            no touch ~Tsleep        PWR tap
   ┌──────┐ ───────────────▶ ┌──────┐ ───────────────▶ ┌────────────┐
   │ LIVE │                  │ DOZE │                  │ DEEP SLEEP │
   └──────┘ ◀─────────────── └──────┘ ◀ (reboot) ───── └────────────┘
       ▲   touch / button /      │   touch / button
       │   timer heartbeat       │
       └─────────────────────────┘
```

Three live-ish tiers + the existing floor:

### Tier L — Live (unchanged)
Active interaction. Full clock, 1 s (100 ms voice) touch cadence, WiFi
connected, 60 s substrate refresh. Entered from any wake.

### Tier D — Doze (new, primary "between" state)
Entered after `T_doze` of no touch/button (candidate: 45–90 s — set by
measurement). The pet shows its resting/sleeping face (already what the
substrate projects when idle); the screen does **not** change on entry
beyond that natural resting render, so entry is silent and flicker-free.

Behaviour:
- **SoC**: automatic light sleep via `esp_pm` (see Config). The SoC
  sleeps in the gaps between FreeRTOS ticks and during the main loop's
  `wait_event` block, transparently. RAM + peripherals retained.
- **Touch INT (GPIO21)** registered as a GPIO light-sleep wake source →
  a tap wakes the SoC in µs, the existing ISR fires, the loop dispatches
  the tap normally. Wake → Tier L.
- **WiFi**: kept associated at `WIFI_PS_MAX_MODEM`
  (`CONFIG_MOCHI_DOZE_WIFI_POWERSAVE`, the as-built choice — see WiFi
  section). The device stays reachable + synced; it does not go dark.
- **Substrate refresh**: stretched to 5 min (`power_substrate_refresh_us`).

This is the state the device lives in most of the time. It looks
identical to Live to the user (pet on screen) but the SoC is asleep
between events. What keeps running on a timer — the 5-min resync,
telemetry, consolidation — is in §Background work while dozing.

### Tier S — Deep sleep (unchanged)
Entered by explicit PWR tap, or after a long secondary timeout
`T_sleep` in Doze (candidate 30–60 min) to protect the battery if the
device is left untouched for hours. Wake = reboot + NVS restore, as
today.

## Wake sources

| Source | Pin | Doze (light) | Deep | Notes |
|---|---|---|---|---|
| Touch INT | GPIO21 | ✅ GPIO wake | (not today) | RTC-capable; ISR already installed |
| PWR button | GPIO18 | ✅ GPIO wake | ✅ ext1 any-low | |
| BOOT button | GPIO0 | ✅ GPIO wake | ✅ ext1 any-low | |
| FreeRTOS/timer | — | ✅ implicit | (open q) | the periodic tasks below (resync, telemetry, touch-poll) wake the SoC on their own timers — no dedicated heartbeat timer is needed under automatic light sleep |

Doze adds touch INT as a wake source — the key difference from deep
sleep, which deliberately wakes only on the physical buttons. (Earlier
drafts proposed a dedicated heartbeat timer; with `esp_pm` automatic
light sleep the existing periodic tasks already wake the SoC, so none
was added — see §Background work while dozing.)

## Config changes required (`sdkconfig.defaults`)

```
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_PM_DFS_INIT_AUTO=y          # or set freqs explicitly at runtime
# esp_pm_config: max=240, min=80 (or 40), light_sleep_enable=true
```

Two existing settings interact and must be revisited:

- **`CONFIG_FREERTOS_HZ=1000`** works *against* tickless idle — a 1 kHz
  tick means up to 1000 idle wakeups/s, capping the achievable sleep
  residency. The current 1 kHz was chosen "for animation frame pacing
  later" (it has no consumer yet). Recommend dropping to 100 Hz for the
  doze win, or measuring both. Tickless idle suppresses the ticks while
  idle regardless, but a lower base HZ reduces wake churn.
- **`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`** — an attached USB-Serial-JTAG
  host keeps the peripheral clock requested and can block light sleep
  entirely. Acceptable: on battery there's no host; on the bench you see
  Live-like draw. Worth a diag note so it's not mistaken for a
  regression during measurement.

## WiFi handling in Doze

Two options; pick by measurement + product feel:

1. **Drop WiFi on doze entry, reconnect on wake.** Simplest, lowest
   doze power. The design/21 non-blocking worker already reconnects
   gracefully off the critical path; the pet only needs periodic sync,
   so a reconnect on the next interaction (or heartbeat) is fine. Risk:
   server-initiated pushes (voice-driven travel) won't land until the
   next wake — acceptable, the device is idle by definition.
2. **Keep WiFi at `WIFI_PS_MAX_MODEM`.** Connection stays alive,
   coexists with light sleep by waking on DTIM beacons. Higher doze
   power, but inbound state stays current. Heavier; probably not worth
   it for a kid's pet that's idle.

**As built: option 2** (`CONFIG_MOCHI_DOZE_WIFI_POWERSAVE=y`). The pet
is a synced object — keeping the link up at `WIFI_PS_MAX_MODEM` lets the
5-min `pet_sync` resync, server pushes, and consolidation just keep
working through doze with no reconnect dance, and DTIM-beacon modem
sleep still recovers most of the WiFi idle power. Option 1 (drop +
reconnect on wake) is the fallback if the §Measurement plan shows
`MAX_MODEM` doze power is still too high — it trades sync latency for
lower draw, and the design/21 worker makes the reconnect safe. The
choice is one Kconfig flag, so it's cheap to flip and re-measure.

## Implementation sketch

State lives in the main loop, which already tracks `last_event_us`
(updated on every dispatched touch) and gates idle work on it (e.g. the
`OTA_IDLE_GATE_US` check at `main.cpp:2423` is prior art for
"only when the user's been idle a while").

- Add an `idle_us = now_us - last_event_us` computation at the top of
  the loop and derive a `PowerTier` from it (`Live` < `T_doze` ≤ `Doze`
  < `T_sleep` ≤ deep-sleep request).
- On the Live→Doze edge: register the touch INT GPIO wake
  (`gpio_wakeup_enable(GPIO21, GPIO_INTR_LOW_LEVEL)` +
  `esp_sleep_enable_gpio_wakeup()`), set WiFi PS / drop WiFi, lengthen
  the effective substrate interval, emit a `DIAG_INFO "power" "doze"`
  event. With `esp_pm` + tickless idle enabled, the existing
  `wait_event(1000ms)` block then *is* the sleep — no explicit
  `esp_light_sleep_start()` call needed; the PM lock framework sleeps
  whenever no task holds a lock.
- On any touch/button (Doze→Live edge): undo the above, restore the
  fast cadence, emit `"wake"`.
- On the Doze→Deep edge (`T_sleep` exceeded): render the asleep frame
  and `commit_sleep()` exactly as the PWR-tap path does today.

Because automatic light sleep is transparent, the bulk of the work is
the tier state machine + the WiFi/refresh-cadence policy, not explicit
sleep calls. An explicit `esp_light_sleep_start()` with timer wake (the
"heartbeat" variant) is the fallback if `esp_pm` residency proves poor.

## Risks / validate on hardware

- **Light-sleep residency vs `FREERTOS_HZ`** — measure 1 kHz vs 100 Hz.
- **Touch INT as a wake source** — confirm GPIO21 actually wakes the SoC
  from light sleep on this board rev (the same wiring caveat the touch
  bring-up already flagged: if GPIO21 isn't wired to FT6336_INT, doze
  falls back to button-only wake + the 10 Hz poll, which is degraded but
  not broken).
- **e-paper driver state across light sleep** — the panel is idle and
  retains, but confirm the SSD1681/SPI peripheral doesn't need
  re-init on wake (light sleep retains peripheral config; expected fine,
  verify).
- **Voice / OTA / consolidate** must hold a PM lock (or keep the device
  in Live) while running so light sleep can't cut a TLS handshake or an
  active session. `esp_pm_lock_acquire` around those, or simply gate
  doze on `!voice::is_active() && !ota_in_flight() && !consolidate_in_flight()`
  (matches existing idle-work gates).

## Measurement plan (drives the timeouts)

Before fixing `T_doze` / `T_sleep`, measure on hardware (battery, USB
detached):

1. Live, idle screen, WiFi connected — baseline mA.
2. Doze, WiFi dropped, light sleep — target mA.
3. Doze, WiFi `MAX_MODEM` — comparison.
4. Deep sleep — floor µA (and the wake-reboot energy cost, to know how
   often a timer-wake deep sleep would be worth it).

The ratio of (1) to (2) sets whether Doze is worth a short or long
`T_doze`; the ratio of (2) to (4) sets `T_sleep`.

## Open questions

1. Timer-wake **deep** sleep for an overnight "check in + update splash"
   without staying in doze — worth it once (2)/(4) is known?
2. Should Doze entry change the pet's face deliberately (a visible
   "settling" cue) or stay silent? Silent avoids an e-paper flash;
   a cue is more legible. Lean silent.
3. `T_doze` user-configurable in Settings (design/22), or fixed?

## Background work while dozing

Doze is not "off" — the device keeps doing its periodic work; automatic
light sleep just micro-sleeps the SoC *between* these and the FreeRTOS
tick fully wakes it when a timer fires. WiFi stays associated (above),
so all of this works without a reconnect:

| Work | Cadence | Wakes the SoC? | Notes |
|------|---------|----------------|-------|
| Touch poll task | 100 ms | yes | also the wake path if the INT line isn't wired |
| Main loop tick (`wait_event`) | 1 s | yes | evaluates the gates below |
| **`pet_sync` resync → `/api/state`** | **5 min** (`RESYNC_INTERVAL_MS`) | yes | the "intermittently wake to sync" path; own task, tier-independent |
| Substrate re-projection | 5 min in doze (`power_substrate_refresh_us`) | yes | only repaints if the resolved sprite/thought changed |
| Health + power telemetry | 5 min | yes | the snapshot rows §Telemetry queries read |
| Diagnostic flush | periodic, idle-gated | yes | POSTs the buffered records |
| OTA check | when idle ≥ 60 s (`OTA_IDLE_GATE_US`) | yes | pre-existing idle gate, unchanged by the tier work; *not* the sleep strategy |

The 100 ms poll + 1 s tick cap light-sleep residency (the SoC can only
stay down ~100 ms at a stretch); that's the first thing the telemetry
will show and the first knob to turn (suspend the poll task in doze /
lengthen the tick) if residency is poor. Lowering `FREERTOS_HZ` from
1000 helps here too.

### Consolidation interplay

Two unrelated "sleeps": the **pet** being asleep (substrate mood state)
and the **device** dozing (power tier). They're orthogonal.

- Sleep consolidation (design/19) is gated on *pet asleep* +
  substrate-advised, checked every idle tick — **not** on the device
  power tier. So a dozing device still kicks a reflection pass when one
  is due (the 5-min resync is what surfaces the advice).
- The power tier *protects* the pass: `power_update`'s `inhibited` input
  is `voice || imagine || consolidate_in_flight || dev_menu || key_portal`.
  While any of those is live the device is **pinned Live** (never dozes,
  so light sleep can't cut a TLS handshake or the BYO-key reflection
  call mid-flight). When it finishes, the device falls back to doze on
  the next idle window.
- Net flow: doze → 5-min resync wakes the SoC → pull advises a pass and
  the pet is asleep → `consolidate_start()` → device pinned Live for the
  pass → back to doze.

The same inhibit rule is why a voice session or an OTA download never
races light sleep — they hold the device Live for their duration.

## Measured baseline (existing Live-only draw)

Recovered 2026-05-28 from the existing `device_logs` `health` telemetry
(623 snapshots; `batt_mv` / `batt_pct` / `up_s` per record), regressing
battery against uptime within each boot session (deep sleep reboots, so
`up_s` resets per `boot_id`):

| boot | samples | span (h) | mv range | mV/h | %/h |
|------|---------|----------|----------|------|-----|
| b491d534 | 213 | 17.7 | 3046–3940 | −36.8 | **−4.24** |
| ba26691c | 120 | 3.7 | 3962–4148 | −32.8 | −3.69 |
| baa75ab2 | 80 | 6.6 | 3748–3908 | −23.6 | −2.61 |
| b41e9d1d | 73 | 6.0 | 3908–4038 | −15.7 | −1.75 |
| bf782899 | 11 | 0.8 | — | +175 | +19.2 (charging/USB) |

**Headline:** in today's Live-only mode the device draws ≈ **4 %/hour**
when running normally → **~24 h of runtime per full charge** (from the
clean 17.7 h discharge run, 3940→3046 mV). Lighter sessions (−1.8 to
−2.6 %/h) reflect partial charging / less activity; one session was on
USB (+19 %/h). The discharge is reported capacity-independent (%/h and
mV/h) because the board has only a voltage divider (GPIO17), no current
sense — runtime falls straight out of %/h without needing the cell's mAh.

This is the number Doze has to beat. Re-run the query in §Analysis after
a doze-enabled build has logged a few hours to quantify the win.

## Projected savings (hypothesis — validate against telemetry)

Modelled from ESP32-S3 component currents (we have no current sense, only
the voltage divider), anchored to the measured 24 h / ~4 %/h baseline.
Battery capacity is unknown but **cancels in the multiplier**, so the
lifespan *ratios* are firmer than the absolute mA. These are the numbers
the `power` + `health` telemetry should confirm or refute.

**Where the baseline goes** — the dominant cost is the always-on 240 MHz
CPU (no DFS/PM today); light sleep attacks exactly that, WiFi is secondary:

| Term | ~mA (representative) |
|------|----------------------|
| CPU active @240 MHz, continuous | ~35–45 |
| WiFi associated, default MIN_MODEM (DTIM RX, avg) | ~3–8 |
| e-paper (retains, idle) + sensors + ADC | ~1–2 |
| **Baseline total** | **~40 mA → 24 h (measured anchor)** |

**Regimes:**

| Regime | Est. avg current | × baseline | Lifespan (while idle) |
|--------|------------------|-----------|------------------------|
| Baseline (Live, as-is) | ~40 mA | 1.0× | 24 h (measured) |
| **Doze: WiFi PS_MAX_MODEM + light sleep** (as built) | ~5–8 mA | ~0.15× | **~5–7 days** (~5–7×) |
| Doze: WiFi off + light sleep (fallback option) | ~2–4 mA | ~0.07× | ~10–15 days (~10–15×) |
| + suspend the 100 ms touch poll (follow-up) | ~1–3 mA | ~0.04× | ~2–4 weeks (~15–30×) |

**Reading of the rows:**
- **As-built (PS + light sleep): ~5–7×.** Almost entirely from light
  sleep collapsing the always-on CPU; WiFi PS_MAX_MODEM is a minor extra.
- **WiFi off vs PS: ~2× on top.** Dropping WiFi removes ~2–3 mA of
  idle + averaged 5-min resync — roughly halves doze current again — at
  the cost of no live inbound sync (reconnect + TLS burst on wake). For a
  mostly-idle pet, WiFi-off is the bigger battery win; PS buys "stays
  current / server can push travel" for ~2× the doze draw.
- **The cap that matters most:** the 100 ms touch-poll task holds the SoC
  out of deep light sleep (≤~100 ms per stretch) and octal-PSRAM
  retention sets a ~0.5–1.5 mA floor — that's why as-built is days, not
  weeks. Suspending the poll in doze (lean on GPIO21 INT) + dropping
  `FREERTOS_HZ` 1000→100 is the highest-leverage follow-up; it's what
  unlocks the bottom row.

**Caveats:** lifespan above is for *time spent idle*; real runtime is a
usage-weighted blend (Live during play). Absolute days scale with the
unknown cell; the multipliers don't.

### How to evaluate against telemetry

Each row is falsifiable from the rows logged once a doze-enabled build
runs (cross-reference §Analysis queries):

| Hypothesis | Confirm from |
|------------|--------------|
| Baseline ≈ 4 %/h | §baseline discharge regression on `health` (already true) |
| As-built doze ≈ 5–7× | discharge regression on a doze-heavy boot vs baseline; expect ~0.6–0.8 %/h while idle |
| Achieved residency (the make-or-break) | `doze_ms/(live_ms+doze_ms)` from the §doze-residency query — model assumes high residency; the 100 ms poll is the risk |
| Live vs Doze draw, separated | regression of `pct_per_h` against `doze_pct` across boots → intercept = Live %/h, intercept+slope = full-Doze %/h |
| WiFi PS vs off delta (~2×) | compare boots built with `MOCHI_DOZE_WIFI_POWERSAVE` on/off (the `init` record's `wifi_ps` segments them) |

If the residency number comes back low (say <80 %), that explains any
shortfall vs the ~5–7× target and points straight at the poll-suspend
follow-up before any deeper change.

## Field result — v0.2.0 (hypothesis REFUTED) → v0.3.0 iteration

First doze-enabled build (v0.2.0, `FREERTOS_HZ=1000`, WiFi kept at
`PS_MAX_MODEM`) ran overnight on one device. Telemetry verdict:

- **Tier machine works, stability fine:** one boot all night (no
  light-sleep crash-loop), ~98% *doze-tier* residency
  (`doze_ms 26,546 s` vs `live_ms 495 s`), clean transitions, visible
  consolidation wake/doze cycles.
- **But ~no battery saving.** Matched voltage band (3800–3920 mV):
  doze boot **−20.4 mV/h** vs baseline Live **−21 to −23 mV/h** — within
  noise. The 5–7× projection was wrong. (Per-boot %/h *looked* better —
  −2.6 vs −4.2 %/h — but that's a LiPo-curve artifact: the baseline run
  discharged through the steep sub-3.4 V end; matched-band mV/h is the
  honest comparison and it's flat.)

**Root-cause hypothesis:** the tier counter reports *intent* (we set
`tier=doze`), not whether the SoC actually light-slept. The prime suspect
is **wake cadence**: the 100 ms touch-poll task + the 1 kHz FreeRTOS tick
wake the SoC ~10–1000×/s regardless of tier, so real light-sleep
residency never accumulates. WiFi-associated PM behaviour is the
secondary suspect.

**v0.3.0 changes (this release) — attack cadence + measure the truth:**
1. **`FREERTOS_HZ` 1000 → 100** — stop the tick waking the SoC 1000×/s.
2. **Touch poll 10 Hz → 1 Hz while dozing** (`touch::set_low_power`,
   driven from the doze/wake edges) — the INT line still wakes instantly;
   the poll is only the fallback, so worst-case wake-by-touch latency is
   ~1 s while dozing.
3. **`sleep_pct` telemetry** — core-0 idle (≈ light-sleep) fraction per
   snapshot, from the FreeRTOS idle run-time counter
   (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS`). This is the ground truth:
   if v0.2.0 was dozing-but-not-sleeping, `sleep_pct` will read low; the
   fix should push it high.

**Deliberately deferred: dropping WiFi.** The data points first at the
wake cadence (cheap, low-risk, reversible), and shipping a blind
`esp_wifi_stop`/reconnect to OTA risks connectivity loss with no way to
validate it here. `sleep_pct` from this build decides it: if residency
goes high but discharge still doesn't improve, WiFi is the remaining
draw and the WiFi-drop is the next beta; if residency is still low,
something else holds the SoC awake. Measure, then cut.

## Telemetry

All power telemetry flows through the existing `device_diag` →
`/api/device/diag` → `device_logs` pipeline (design/18), tag `power`,
with structured JSON in `ctx` (SQL-queryable via `json_extract`). Runs
regardless of `CONFIG_MOCHI_LIGHT_SLEEP`, so the Live baseline and the
doze-time accounting are always recorded.

| msg | when | ctx fields |
|-----|------|-----------|
| `init` | boot, once | `sleep_en, wifi_ps, doze_s, deep_s, cpu_max, cpu_min` — the active policy, so each run is segmentable |
| `doze` | Live→Doze edge | `prev_ms` (time spent Live), `batt_mv`, `up_s` |
| `wake` | Doze→Live edge | `prev_ms` (time spent dozing), `batt_mv`, `up_s` |
| `snapshot` | every ~5 min (with the health heartbeat) | `tier, live_ms, doze_ms` (cumulative), `doze_n`, **`sleep_pct`** (actual core-0 idle/light-sleep % since last snapshot; −1 if unavailable), `sleep_en, wifi_ps, doze_s, batt_mv, up_s` |

Tunability is via Kconfig today (`MOCHI_DOZE_TIMEOUT_S`,
`MOCHI_DEEP_SLEEP_TIMEOUT_S`, `MOCHI_DOZE_WIFI_POWERSAVE`,
`MOCHI_LIGHT_SLEEP`, `MOCHI_PM_CPU_{MAX,MIN}_MHZ`); the `init` record
echoes the chosen values so a SQL reader can correlate behaviour with
config across builds. Server-pushed tuning is a future option (carry the
timeouts in `/api/state`), not built.

## Analysis queries

Run against the `c15r/mochi` project DB (`device_logs`).

**Baseline discharge per boot** (works on existing health data):

```sql
WITH h AS (
  SELECT boot_id,
    CAST(json_extract(ctx,'$.up_s')   AS REAL)/3600.0 x,
    CAST(json_extract(ctx,'$.batt_mv') AS REAL) mv,
    CAST(json_extract(ctx,'$.batt_pct') AS REAL) pct
  FROM device_logs WHERE tag='health'
    AND CAST(json_extract(ctx,'$.batt_mv') AS REAL) BETWEEN 3000 AND 4300)
SELECT substr(boot_id,1,8) boot, count(*) n, round(max(x)-min(x),2) span_h,
  round((count(*)*sum(x*mv)-sum(x)*sum(mv))/(count(*)*sum(x*x)-sum(x)*sum(x)),1)  mv_per_h,
  round((count(*)*sum(x*pct)-sum(x)*sum(pct))/(count(*)*sum(x*x)-sum(x)*sum(x)),2) pct_per_h
FROM h GROUP BY boot_id HAVING n>=6 AND span_h>=0.5 ORDER BY span_h DESC;
```

**Doze residency + tier split per boot** (once `power` data exists — uses
the latest snapshot per boot for the cumulative counters):

```sql
WITH s AS (
  SELECT boot_id, at,
    CAST(json_extract(ctx,'$.live_ms') AS REAL) live_ms,
    CAST(json_extract(ctx,'$.doze_ms') AS REAL) doze_ms,
    CAST(json_extract(ctx,'$.doze_n')  AS INT)  doze_n,
    ROW_NUMBER() OVER (PARTITION BY boot_id ORDER BY at DESC) rn
  FROM device_logs WHERE tag='power' AND json_extract(ctx,'$.msg') IS NULL
    AND json_extract(ctx,'$.live_ms') IS NOT NULL)
SELECT substr(boot_id,1,8) boot, doze_n,
  round(live_ms/3600000.0,2) live_h, round(doze_ms/3600000.0,2) doze_h,
  round(100.0*doze_ms/NULLIF(live_ms+doze_ms,0),1) doze_pct
FROM s WHERE rn=1 ORDER BY (live_ms+doze_ms) DESC;
```

(Filter `power` snapshots vs transition events by `msg` if the sink
records it; otherwise snapshots are the rows carrying `live_ms`.)

**Discharge attributed to tiers** — join the §baseline discharge per boot
with the doze-residency query above: a regression of `pct_per_h` against
`doze_pct` across boots recovers the Live vs Doze draw (intercept = Live
%/h, intercept+slope = full-Doze %/h). Needs a handful of boots at
different residencies; that's the payoff of logging both.

## Cross-references

- `design/22-settings-actions-and-sleep.md` — PWR-tap sleep gesture,
  the Live/Splash/Settings wheel (doze must not fight the wheel).
- `design/21-nonblocking-wifi.md` — the reconnect-off-critical-path
  worker that makes "drop WiFi on doze" safe.
- `firmware/main/sleep_gesture.cpp` — `commit_sleep()`, the deep-sleep
  wake-source setup this builds on.
- `firmware/main/main.cpp` — `last_event_us`, `SUBSTRATE_REFRESH_US`,
  the `OTA_IDLE_GATE_US` idle gate (prior art for tier detection).
- `firmware/main/board_pins.h` — GPIO21 touch INT, GPIO18 PWR,
  GPIO0 BOOT, GPIO17 VBAT sense.
- `firmware/sdkconfig.defaults` — `FREERTOS_HZ`, console, where the
  `esp_pm` knobs land.
