// #region StdManifest
/**
 * @file ED_heap_tracer.h
 * @brief
 * usage:
 * #define CONFIG_HEAP_TRACING
 * MqttClient::start()  to activate
 * mqtt_heap_trace_snapshot_async(); to produce an anync snapshot of the heap
 *
 *not defining CONFIG_HEAP_TRACING causes linkage of empty functions
 *
 * @author Emanuele Dolis (edoliscom@gmail.com)
 * @version 0.1
 * @date 2025-09-20
 */
// #endregion

#pragma once

namespace ED_heap_tracer {

enum class TraceMode {
  LEAKS, // long-lived allocations - a 60s polling interval recommended
  ALL    // all allocations, including short-lived - a 1-2s polling interval recommended
};
void set_trace_mode(TraceMode mode);
TraceMode get_trace_mode();
// Call once before you want to start profiling allocations
void mqtt_heap_trace_start();
// Take a snapshot of current allocations and enqueue for logging
// Safe to call from normal task context (non‑blocking)
void mqtt_heap_trace_snapshot_async();

} // namespace ED_heap_tracer