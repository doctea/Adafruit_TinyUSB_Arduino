#ifndef TUSB_OSAL_PICO_TRACE_H_
#define TUSB_OSAL_PICO_TRACE_H_

#ifndef CFG_TUSB_PICO_TRACE
#define CFG_TUSB_PICO_TRACE 0
#endif

#if CFG_TUSB_PICO_TRACE

#include <stdint.h>
#include "pico/mutex.h"

#ifndef CFG_TUSB_PICO_TRACE_DEPTH
#define CFG_TUSB_PICO_TRACE_DEPTH 64
#endif

#if CFG_TUSB_PICO_TRACE_DEPTH < 1
#error CFG_TUSB_PICO_TRACE_DEPTH must be positive
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TUSB_PICO_TRACE_LOCK_WAIT = 1,
  TUSB_PICO_TRACE_LOCK_ACQUIRED,
  TUSB_PICO_TRACE_LOCK_FAILED,
  TUSB_PICO_TRACE_LOCK_RELEASE,
  TUSB_PICO_TRACE_TASK_ENTER,
  TUSB_PICO_TRACE_TASK_EXIT,
  TUSB_PICO_TRACE_TASK_SKIPPED,
  TUSB_PICO_TRACE_CDC_WRITE,
  TUSB_PICO_TRACE_CDC_PROGRESS,
  TUSB_PICO_TRACE_CDC_DONE,
  TUSB_PICO_TRACE_CDC_FLUSH,
  TUSB_PICO_TRACE_EVENT_COUNT
} tusb_pico_trace_event_t;

typedef struct {
  uint32_t sequence;
  uint32_t timestamp_us;
  uintptr_t object;
  uintptr_t caller;
  uint32_t value;
  int32_t owner;
  uint32_t irq_state;
  uint16_t exception;
  uint8_t event;
  uint8_t core;
} tusb_pico_trace_entry_t;

typedef struct {
  const char *source;
  uint32_t depth;
  bool frozen;
} tusb_pico_trace_control_t;

extern volatile tusb_pico_trace_entry_t tusb_pico_trace_buffer[2][CFG_TUSB_PICO_TRACE_DEPTH];
extern volatile tusb_pico_trace_entry_t tusb_pico_trace_last_event[2][TUSB_PICO_TRACE_EVENT_COUNT];
extern volatile tusb_pico_trace_entry_t tusb_pico_trace_first_self_wait[2];
extern volatile uint32_t tusb_pico_trace_count[2];
extern volatile uint32_t tusb_pico_trace_self_wait_count[2];
extern volatile tusb_pico_trace_control_t tusb_pico_trace_control;

void tusb_pico_trace_record(tusb_pico_trace_event_t event, const void *object,
                           uint32_t value, int32_t owner, uintptr_t caller);
bool tusb_pico_trace_mutex_lock(mutex_t *mutex, uint32_t timeout_ms) __attribute__((noinline));
void tusb_pico_trace_mutex_unlock(mutex_t *mutex) __attribute__((noinline));

#ifdef __cplusplus
}
#endif

#endif

#endif