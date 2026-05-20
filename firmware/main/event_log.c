#include "event_log.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "event_log";

#define LOG_PATH       "/lfs/events.bin"
#define HEADER_MAGIC   0x4D4F4348u   /* 'MOCH' */
#define HEADER_VERSION 1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t head;     /* index where the NEXT append will write */
    uint32_t count;    /* number of valid records (≤ CAPACITY) */
} header_t;

typedef struct __attribute__((packed)) {
    uint32_t kind;     /* event_kind_t in the low byte; rest reserved */
    uint32_t reserved;
    int64_t  at_ms;
} record_t;

_Static_assert(sizeof(header_t) == 16, "event_log header_t size");
_Static_assert(sizeof(record_t) == 16, "event_log record_t size");

static SemaphoreHandle_t s_mtx;
static header_t          s_hdr;
static bool              s_inited;

static off_t record_offset(uint32_t idx) {
    return (off_t)sizeof(header_t) + (off_t)idx * (off_t)sizeof(record_t);
}

static bool write_header_locked(FILE *f) {
    if (fseek(f, 0, SEEK_SET) != 0) return false;
    if (fwrite(&s_hdr, 1, sizeof(s_hdr), f) != sizeof(s_hdr)) return false;
    return true;
}

static void format_locked(void) {
    /* Truncate + write empty header. The records area is left
     * uninitialised; the head/count fields are the source of truth
     * for which slots are valid. */
    FILE *f = fopen(LOG_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "format: open failed errno=%d", errno);
        return;
    }
    s_hdr.magic   = HEADER_MAGIC;
    s_hdr.version = HEADER_VERSION;
    s_hdr.head    = 0;
    s_hdr.count   = 0;
    write_header_locked(f);
    fclose(f);
    ESP_LOGI(TAG, "formatted fresh ring (cap=%d)", EVENT_LOG_CAPACITY);
}

bool event_log_init(void) {
    if (s_inited) return true;
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (!s_mtx) return false;

    xSemaphoreTake(s_mtx, portMAX_DELAY);

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        ESP_LOGI(TAG, "no existing log; formatting");
        format_locked();
    } else {
        if (fread(&s_hdr, 1, sizeof(s_hdr), f) != sizeof(s_hdr) ||
            s_hdr.magic   != HEADER_MAGIC ||
            s_hdr.version != HEADER_VERSION ||
            s_hdr.head    >= EVENT_LOG_CAPACITY ||
            s_hdr.count   >  EVENT_LOG_CAPACITY) {
            ESP_LOGW(TAG, "header invalid (magic=%08lx ver=%lu head=%lu count=%lu)"
                          " — reformatting",
                (unsigned long)s_hdr.magic, (unsigned long)s_hdr.version,
                (unsigned long)s_hdr.head,  (unsigned long)s_hdr.count);
            fclose(f);
            format_locked();
        } else {
            fclose(f);
            ESP_LOGI(TAG, "loaded log: head=%lu count=%lu",
                (unsigned long)s_hdr.head, (unsigned long)s_hdr.count);
        }
    }

    s_inited = true;
    xSemaphoreGive(s_mtx);
    return true;
}

bool event_log_append(event_kind_t kind, int64_t at_ms) {
    if (!s_inited) return false;
    if (kind < 0 || kind >= EVENT_COUNT) return false;

    xSemaphoreTake(s_mtx, portMAX_DELAY);

    /* Open r+ to retain the header + records area; create if missing
     * (defensive — init() should have done it). */
    FILE *f = fopen(LOG_PATH, "rb+");
    if (!f) {
        format_locked();
        f = fopen(LOG_PATH, "rb+");
        if (!f) {
            ESP_LOGE(TAG, "append: open failed errno=%d", errno);
            xSemaphoreGive(s_mtx);
            return false;
        }
    }

    record_t rec = {
        .kind     = (uint32_t)kind,
        .reserved = 0,
        .at_ms    = at_ms,
    };
    bool ok = true;
    if (fseek(f, record_offset(s_hdr.head), SEEK_SET) != 0) ok = false;
    else if (fwrite(&rec, 1, sizeof(rec), f) != sizeof(rec)) ok = false;

    if (ok) {
        s_hdr.head = (s_hdr.head + 1) % EVENT_LOG_CAPACITY;
        if (s_hdr.count < EVENT_LOG_CAPACITY) s_hdr.count++;
        if (!write_header_locked(f)) ok = false;
    }
    fflush(f);
    fclose(f);

    xSemaphoreGive(s_mtx);
    if (!ok) ESP_LOGW(TAG, "append failed (errno=%d)", errno);
    return ok;
}

size_t event_log_load_recent(pet_event_t *out, size_t cap) {
    if (!s_inited || !out || cap == 0) return 0;
    if (cap > EVENT_LOG_SLICE_MAX) cap = EVENT_LOG_SLICE_MAX;

    xSemaphoreTake(s_mtx, portMAX_DELAY);

    size_t want = s_hdr.count < cap ? s_hdr.count : cap;
    if (want == 0) {
        xSemaphoreGive(s_mtx);
        return 0;
    }

    FILE *f = fopen(LOG_PATH, "rb");
    if (!f) {
        xSemaphoreGive(s_mtx);
        return 0;
    }

    /* Walk backwards from head-1, wrapping. Newest first. */
    size_t loaded = 0;
    uint32_t idx = s_hdr.head;
    for (size_t i = 0; i < want; i++) {
        idx = (idx == 0) ? (EVENT_LOG_CAPACITY - 1) : (idx - 1);
        record_t rec;
        if (fseek(f, record_offset(idx), SEEK_SET) != 0) break;
        if (fread(&rec, 1, sizeof(rec), f) != sizeof(rec)) break;
        if (rec.kind >= EVENT_COUNT) continue;  /* skip torn writes */
        out[loaded].kind = (event_kind_t)rec.kind;
        out[loaded].at   = rec.at_ms;
        loaded++;
    }
    fclose(f);

    xSemaphoreGive(s_mtx);
    return loaded;
}

size_t event_log_count(void) {
    if (!s_inited) return 0;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    size_t n = s_hdr.count;
    xSemaphoreGive(s_mtx);
    return n;
}

bool event_log_clear(void) {
    if (!s_inited) return false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    format_locked();
    xSemaphoreGive(s_mtx);
    return true;
}
