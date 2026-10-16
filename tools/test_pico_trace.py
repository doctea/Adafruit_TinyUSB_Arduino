import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import textwrap


HEADERS = {
    "tusb_option.h": """
        #define OPT_OS_PICO 1
        #define CFG_TUSB_OS OPT_OS_PICO
    """,
    "pico/mutex.h": """
        #ifndef TEST_PICO_MUTEX_H
        #define TEST_PICO_MUTEX_H
        #include <stdbool.h>
        #include <stdint.h>
        typedef struct mutex { int32_t owner; } mutex_t;
        int32_t lock_get_caller_owner_id(void);
        bool mutex_enter_timeout_ms(mutex_t *mutex, uint32_t timeout_ms);
        void mutex_exit(mutex_t *mutex);
        #endif
    """,
    "hardware/sync.h": """
        #include <stdint.h>
        uint32_t save_and_disable_interrupts(void);
        void restore_interrupts(uint32_t state);
        uint32_t get_core_num(void);
        uint32_t __get_current_exception(void);
    """,
    "pico/time.h": """
        #include <stdint.h>
        uint32_t time_us_32(void);
    """,
}

HARNESS = r"""
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "osal_pico_trace.h"

static uint32_t fake_core;
static uint32_t fake_irq_state;
static uint32_t fake_exception;
static uint32_t fake_time;
static uint32_t fake_timeout;
static uint32_t fake_unlocks;
static bool fake_acquired;
static bool inject_interrupt;

uint32_t save_and_disable_interrupts(void) {
  const uint32_t previous = fake_irq_state;
  fake_irq_state = 1;
  return previous;
}

void restore_interrupts(uint32_t state) {
  assert(fake_irq_state == 1);
  fake_irq_state = state;
  if (inject_interrupt && state == 0) {
    inject_interrupt = false;
    fake_exception = 17;
    tusb_pico_trace_record(TUSB_PICO_TRACE_TASK_ENTER, NULL, 1, -1, 0);
    fake_exception = 0;
  }
}

uint32_t get_core_num(void) { return fake_core; }
uint32_t __get_current_exception(void) { return fake_exception; }
int32_t lock_get_caller_owner_id(void) { return (int32_t)fake_core; }

uint32_t time_us_32(void) {
  assert(fake_irq_state == 1);
  return ++fake_time;
}

bool mutex_enter_timeout_ms(mutex_t *mutex, uint32_t timeout_ms) {
  assert(fake_irq_state == 0);
  fake_timeout = timeout_ms;
  if (fake_acquired) mutex->owner = (int32_t)fake_core;
  return fake_acquired;
}

void mutex_exit(mutex_t *mutex) {
  assert(fake_irq_state == 0);
  fake_unlocks++;
  mutex->owner = -1;
}

int main(void) {
  mutex_t mutex = { .owner = -1 };
  fake_acquired = true;
  assert(tusb_pico_trace_mutex_lock(&mutex, UINT32_MAX));
  assert(fake_timeout == UINT32_MAX);
  assert(tusb_pico_trace_buffer[0][0].event == TUSB_PICO_TRACE_LOCK_WAIT);
  assert(tusb_pico_trace_buffer[0][1].event == TUSB_PICO_TRACE_LOCK_ACQUIRED);
  tusb_pico_trace_mutex_unlock(&mutex);
  assert(fake_unlocks == 1 && mutex.owner == -1);

  fake_acquired = false;
  mutex.owner = 1;
  assert(!tusb_pico_trace_mutex_lock(&mutex, 10));
  assert(fake_timeout == 10 && mutex.owner == 1 && fake_unlocks == 1);
  assert(tusb_pico_trace_last_event[0][TUSB_PICO_TRACE_LOCK_FAILED].value == 10);
  assert(tusb_pico_trace_self_wait_count[0] == 0);

  fake_exception = 18;
  fake_irq_state = 1;
  tusb_pico_trace_record(TUSB_PICO_TRACE_LOCK_WAIT, &mutex, UINT32_MAX, 0, 123);
  assert(fake_irq_state == 1);
  assert(tusb_pico_trace_first_self_wait[0].exception == 18);
  assert(tusb_pico_trace_first_self_wait[0].irq_state == 1);
  assert(tusb_pico_trace_first_self_wait[0].caller == 123);
  tusb_pico_trace_record(TUSB_PICO_TRACE_LOCK_WAIT, &mutex, 10, 0, 456);
  assert(tusb_pico_trace_self_wait_count[0] == 2);
  assert(tusb_pico_trace_first_self_wait[0].caller == 123);
  fake_irq_state = 0;
  fake_exception = 0;

  const uint32_t before_interrupt = tusb_pico_trace_count[0];
  inject_interrupt = true;
  tusb_pico_trace_record(TUSB_PICO_TRACE_CDC_WRITE, &mutex, 42, -1, 0);
  assert(tusb_pico_trace_count[0] == before_interrupt + 2);
  assert(tusb_pico_trace_last_event[0][TUSB_PICO_TRACE_CDC_WRITE].sequence == before_interrupt + 1);
  assert(tusb_pico_trace_last_event[0][TUSB_PICO_TRACE_TASK_ENTER].sequence == before_interrupt + 2);
  assert(tusb_pico_trace_last_event[0][TUSB_PICO_TRACE_TASK_ENTER].exception == 17);
  assert(fake_irq_state == 0);

  for (uint32_t index = 0; index < CFG_TUSB_PICO_TRACE_DEPTH + 3; index++) {
    tusb_pico_trace_record(TUSB_PICO_TRACE_TASK_EXIT, NULL, index, -1, 0);
  }
  const uint32_t final_count = tusb_pico_trace_count[0];
  assert(tusb_pico_trace_buffer[0][(final_count - 1) % CFG_TUSB_PICO_TRACE_DEPTH].sequence == final_count);
  assert(tusb_pico_trace_last_event[0][TUSB_PICO_TRACE_CDC_WRITE].value == 42);
  assert(tusb_pico_trace_first_self_wait[0].caller == 123);
  assert(tusb_pico_trace_count[1] == 0);

  fake_core = 1;
  tusb_pico_trace_record(TUSB_PICO_TRACE_LOCK_WAIT, &mutex, 10, 1, 789);
  assert(tusb_pico_trace_count[0] == final_count);
  assert(tusb_pico_trace_count[1] == 1);
  assert(tusb_pico_trace_buffer[1][0].core == 1);
  assert(tusb_pico_trace_first_self_wait[1].caller == 789);

  tusb_pico_trace_control.frozen = true;
  fake_acquired = true;
  assert(tusb_pico_trace_mutex_lock(&mutex, 20));
  tusb_pico_trace_mutex_unlock(&mutex);
  assert(tusb_pico_trace_count[1] == 1 && fake_unlocks == 2);
  assert(fake_irq_state == 0);
  puts("PASS: mutex results, interrupt restoration/nesting, per-core isolation, wraparound, snapshots, freeze");
  return 0;
}
"""


def main():
    source = Path(__file__).resolve().parents[1] / "src" / "osal"
    compiler = shlex.split(os.environ.get("CC", "cc"))
    with tempfile.TemporaryDirectory(prefix="tinyusb-pico-trace-") as directory:
        temporary = Path(directory)
        for name, contents in HEADERS.items():
            target = temporary / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(textwrap.dedent(contents), encoding="ascii")
        harness = temporary / "test.c"
        harness.write_text(HARNESS, encoding="ascii")
        flags = compiler + ["-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(temporary), "-I", str(source)]
        executable = temporary / ("test.exe" if os.name == "nt" else "test")
        subprocess.run(flags + ["-DCFG_TUSB_PICO_TRACE=1", str(source / "osal_pico_trace.c"),
                                str(harness), "-o", str(executable)], check=True)
        subprocess.run([str(executable)], check=True)
        subprocess.run(flags + ["-DCFG_TUSB_PICO_TRACE=0", "-c",
                                str(source / "osal_pico_trace.c"),
                                "-o", str(temporary / "disabled.o")], check=True)
        print("PASS: tracing-disabled translation unit compiles")


if __name__ == "__main__":
    main()