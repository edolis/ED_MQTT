#include "ED_heap_tracer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <esp_debug_helpers.h>
#include <esp_heap_trace.h>
#include <esp_log.h>
#include <unordered_map>

namespace ED_heap_tracer {
static const char *HEAP_TAG = "ED_HEAPDIAGN";

/**
 * @brief
 *How to run
** Leak mode (long‑lived allocations, 60 s snapshots):

cpp
ED_heap_tracer::set_trace_mode(ED_heap_tracer::TraceMode::LEAKS);

**Churn mode (fragmentation hunt, 1–2 s snapshots):

cpp
ED_heap_tracer::set_trace_mode(ED_heap_tracer::TraceMode::ALL);
In churn mode you’ll see NEW/FREED entries for everything outstanding at that instant
— including short‑lived allocations — so you can catch the high‑turnover patterns that
fragment the heap.
 *
 *
 *
 *
 *
 *
 *
 *
 */

static TraceMode current_mode = TraceMode::LEAKS;

void set_trace_mode(TraceMode mode) {
    current_mode = mode;
}
TraceMode get_trace_mode() {
    return current_mode;
}



// Trace storage the tracer writes into
static heap_trace_record_t mqtt_trace_records[100];

// Async dump infra
static QueueHandle_t heap_dump_q = nullptr;
static TaskHandle_t heap_dump_task_h = nullptr;

// We track tracing state ourselves (no heap_trace_is_enabled needed)
static bool mqtt_heap_tracing_active = false;

// Snapshot payload we pass to the dump task
typedef struct {
  heap_trace_record_t recs[100];
  size_t count;
} heap_snapshot_t;

static bool is_valid_record(const heap_trace_record_t &r) {
  return r.address != nullptr && r.size > 0;
}

static std::unordered_map<void *, size_t> prev_allocs;

static void heap_dump_task(void *param) {
    heap_snapshot_t snap;
    for (;;) {
        if (xQueueReceive(heap_dump_q, &snap, portMAX_DELAY) == pdTRUE) {

            // Sort largest first
            for (size_t i = 0; i + 1 < snap.count; ++i) {
                for (size_t j = i + 1; j < snap.count; ++j) {
                    if (snap.recs[j].size > snap.recs[i].size) {
                        auto tmp = snap.recs[i];
                        snap.recs[i] = snap.recs[j];
                        snap.recs[j] = tmp;
                    }
                }
            }

            // Totals for outstanding
            size_t outstanding_bytes = 0;
            for (size_t i = 0; i < snap.count; ++i) {
                outstanding_bytes += snap.recs[i].size;
            }

            const size_t free_heap     = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
            const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

            std::unordered_map<void*, size_t> cur_allocs;
            cur_allocs.reserve(snap.count * 2);

            const size_t max_to_print = 30;
            size_t printed_new = 0, printed_freed = 0;
            size_t bytes_new = 0, bytes_freed = 0;

            // NEW allocations
            for (size_t i = 0; i < snap.count; ++i) {
                const auto &r = snap.recs[i];
                cur_allocs[r.address] = r.size;

                if (!prev_allocs.count(r.address)) {
                    bytes_new += r.size;
                    if (printed_new < max_to_print) {
                        ESP_LOGI(HEAP_TAG, "NEW: addr=%p size=%u pc0=%p pc1=%p",
                                 r.address, (unsigned)r.size,
                                 r.alloced_by[0], r.alloced_by[1]);
                        ++printed_new;
                        if ((printed_new % 10) == 0) vTaskDelay(1);
                    }
                }
            }

            // FREED allocations
            for (const auto &kv : prev_allocs) {
                if (!cur_allocs.count(kv.first)) {
                    bytes_freed += kv.second;
                    if (printed_freed < max_to_print) {
                        ESP_LOGI(HEAP_TAG, "FREED: addr=%p size=%u",
                                 kv.first, (unsigned)kv.second);
                        ++printed_freed;
                        if ((printed_freed % 10) == 0) vTaskDelay(1);
                    }
                }
            }

            ESP_LOGI(HEAP_TAG,
                     "Diff: %u new (%u bytes), %u freed (%u bytes), outstanding=%u bytes, free=%u, largest=%u",
                     (unsigned)printed_new, (unsigned)bytes_new,
                     (unsigned)printed_freed, (unsigned)bytes_freed,
                     (unsigned)outstanding_bytes,
                     (unsigned)free_heap, (unsigned)largest_block);

            prev_allocs.swap(cur_allocs);
        }
    }
}

static void ensure_heap_dump_task() {
  if (!heap_dump_q) {
    heap_dump_q = xQueueCreate(2, sizeof(heap_snapshot_t));
  }
  if (!heap_dump_task_h) {
    // Low-ish prio, tiny stack is fine (we only log)
    xTaskCreatePinnedToCore(heap_dump_task, "heap_dump_task", 6144, nullptr,
                            tskIDLE_PRIORITY + 1, &heap_dump_task_h, 0);
  }
}

// Call this once before the operation you want to profile
void mqtt_heap_trace_start() {
#if CONFIG_HEAP_TRACING
    if (!mqtt_heap_tracing_active) {
        ESP_ERROR_CHECK(heap_trace_init_standalone(mqtt_trace_records, 100));
        ESP_ERROR_CHECK(heap_trace_start(
            current_mode == TraceMode::LEAKS ? HEAP_TRACE_LEAKS : HEAP_TRACE_ALL
        ));
        mqtt_heap_tracing_active = true;
        ESP_LOGI(HEAP_TAG, "Heap trace started in %s mode",
                 current_mode == TraceMode::LEAKS ? "LEAKS" : "ALL");
    }
#endif
}
// DO NOT print here. Just snapshot, enqueue, and immediately resume tracing.
// This is safe to call from mqtt_task/event handler without tripping WDT.
void mqtt_heap_trace_snapshot_async() {
#if CONFIG_HEAP_TRACING
    if (!mqtt_heap_tracing_active) return;

    ESP_ERROR_CHECK(heap_trace_stop());
    mqtt_heap_tracing_active = false;

    ensure_heap_dump_task();
    heap_snapshot_t snap = {};
    for (size_t i = 0; i < 100; ++i) {
        if (is_valid_record(mqtt_trace_records[i])) {
            snap.recs[snap.count++] = mqtt_trace_records[i];
        }
    }
    if (heap_dump_q) {
        (void)xQueueSend(heap_dump_q, &snap, 0);
    }

    ESP_ERROR_CHECK(heap_trace_init_standalone(mqtt_trace_records, 100));
    ESP_ERROR_CHECK(heap_trace_start(
        current_mode == TraceMode::LEAKS ? HEAP_TRACE_LEAKS : HEAP_TRACE_ALL
    ));
    mqtt_heap_tracing_active = true;
#endif
}



} // namespace ED_heap_tracer