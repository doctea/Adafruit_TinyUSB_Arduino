#include "tusb_option.h"

#if CFG_TUSB_OS == OPT_OS_PICO

#include "osal_pico_trace.h"

#if CFG_TUSB_PICO_TRACE

#include "hardware/sync.h"
#include "pico/time.h"

volatile tusb_pico_trace_entry_t tusb_pico_trace_buffer[2][CFG_TUSB_PICO_TRACE_DEPTH];
volatile tusb_pico_trace_entry_t tusb_pico_trace_last_event[2][TUSB_PICO_TRACE_EVENT_COUNT];
volatile tusb_pico_trace_entry_t tusb_pico_trace_first_self_wait[2];
volatile uint32_t tusb_pico_trace_count[2];
volatile uint32_t tusb_pico_trace_self_wait_count[2];
volatile tusb_pico_trace_control_t tusb_pico_trace_control = {
  .source = __FILE__,
  .depth = CFG_TUSB_PICO_TRACE_DEPTH,
  .frozen = false
};

void tusb_pico_trace_record(tusb_pico_trace_event_t event, const void *object,
                           uint32_t value, int32_t owner, uintptr_t caller) {
  const uint32_t irq_state = save_and_disable_interrupts();
  const uint32_t core = get_core_num();
  if (!tusb_pico_trace_control.frozen) {
    const uint32_t sequence = tusb_pico_trace_count[core] + 1;
    const tusb_pico_trace_entry_t entry = {
      .sequence = sequence,
      .timestamp_us = time_us_32(),
      .object = (uintptr_t)object,
      .caller = caller,
      .value = value,
      .owner = owner,
      .irq_state = irq_state,
      .exception = (uint16_t)__get_current_exception(),
      .event = (uint8_t)event,
      .core = (uint8_t)core
    };
    tusb_pico_trace_buffer[core][(sequence - 1) % CFG_TUSB_PICO_TRACE_DEPTH] = entry;
    tusb_pico_trace_last_event[core][event] = entry;
    tusb_pico_trace_count[core] = sequence;
    if (event == TUSB_PICO_TRACE_LOCK_WAIT && value != 0 &&
        owner == (int32_t)lock_get_caller_owner_id()) {
      if (tusb_pico_trace_self_wait_count[core] == 0) {
        tusb_pico_trace_first_self_wait[core] = entry;
      }
      tusb_pico_trace_self_wait_count[core]++;
    }
  }
  restore_interrupts(irq_state);
}

bool tusb_pico_trace_mutex_lock(mutex_t *mutex, uint32_t timeout_ms) {
  const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
  tusb_pico_trace_record(TUSB_PICO_TRACE_LOCK_WAIT, mutex, timeout_ms, mutex->owner, caller);
  const bool acquired = mutex_enter_timeout_ms(mutex, timeout_ms);
  tusb_pico_trace_record(acquired ? TUSB_PICO_TRACE_LOCK_ACQUIRED : TUSB_PICO_TRACE_LOCK_FAILED,
                         mutex, timeout_ms, mutex->owner, caller);
  return acquired;
}

void tusb_pico_trace_mutex_unlock(mutex_t *mutex) {
  tusb_pico_trace_record(TUSB_PICO_TRACE_LOCK_RELEASE, mutex, 0, mutex->owner,
                         (uintptr_t)__builtin_return_address(0));
  mutex_exit(mutex);
}

#endif

#endif