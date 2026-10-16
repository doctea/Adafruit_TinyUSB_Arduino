# RP2 USB lock tracing

This is baseline instrumentation, not a deadlock fix. It preserves mutex
timeouts, return values, task scheduling, and CDC write behavior. It does not
enable USB debug printing or assign UART pins.

## Library and build selection

The editable Git repository for this Compulidian setup is:

```text
C:\Users\doctea\Documents\Arduino\Compulidian\lib\Adafruit_TinyUSB_Arduino
```

Branch or configure a fork remote in that repository. No branch, commit, or
remote has been created automatically. Its starting revision was `1b73172`
(library version 3.7.7).

Compulidian now selects this checkout with:

```ini
Adafruit TinyUSB Library=symlink://lib/Adafruit_TinyUSB_Arduino
```

Do not edit the generated dependency copy under `.pio/libdeps` or the copies
bundled inside `.platformio/packages`. A linked dependency can still appear
under `.pio/libdeps` in build output; the symlink specification is intentional.

From the Compulidian project directory, build manually first:

```sh
pio run -e rpipico_16mb_usb_trace
```

This environment inherits the 16 MB flash and LittleFS configuration and enables
`CFG_TUSB_PICO_TRACE=1` with debug symbols and the normal `-Os` optimization.
Other environments leave tracing off.
For another project, select the same checkout and add that build flag globally,
not just in a sketch. Microlidian has not been reconfigured in this workspace.

Before uploading, check that the generated
`.pio/build/rpipico_16mb_usb_trace/firmware_rpipico_16mb_usb_trace.bin` is no larger
than 2,097,152 bytes: Compulidian stores sample banks starting at 2 MB, even though
the flash chip is larger. Stop if the build crosses that boundary. Then upload:

```sh
pio run -e rpipico_16mb_usb_trace --target upload
```

The matching ELF is:

```text
.pio/build/rpipico_16mb_usb_trace/firmware_rpipico_16mb_usb_trace.elf
```

The current generated VS Code launch profiles still select `rpipico_16mb`.
Select/reinitialize the trace environment before using VS Code debugging, or
start a matching GDB session from the Compulidian directory:

```sh
pio debug -e rpipico_16mb_usb_trace --interface=gdb
```

Start debugging before reproducing the failure; normal debug startup may reset
the board. Do not restart a debug session or reset after the failure, because
the trace is ordinary RAM and does not survive a reset. Use `continue` to run
from the initial breakpoint.

## First hardware experiment

Keep the existing MIDI timer, raw task calls, and timeouts unchanged for this
baseline. Try incoming MIDI clock with CDC output first, then add outgoing
MIDI and console input. Record the host applications and which streams are
active. Also test a disconnected console and an open console whose host stops
reading; these are different conditions.

Before running, optionally set GDB breakpoints on `__assert_func` and `abort`
to catch C assertions before they disappear into an exit loop. TinyUSB's
`TU_ASSERT` return paths are not all C assertions and are not intercepted by
this instrumentation.

On a freeze, pause without resetting and halt both cores. In a GDB terminal,
use Ctrl-C; in VS Code use Pause. Check `info threads`: if the server exposes
only one core, obtain the other core's stack through the debug probe/server
before drawing conclusions about cross-core ownership.

Run these commands directly in a GDB terminal. In PlatformIO's VS Code Debug
Console, wrap each command with `-interpreter-exec console "..."`, for example:

```text
-interpreter-exec console "set variable tusb_pico_trace_control.frozen = 1"
-interpreter-exec console "thread apply all bt full"
```

Do not use the `-exec` prefix with the PlatformIO adapter: it is interpreted as
an unsupported GDB/MI command, rather than as a request to execute GDB CLI text.

```gdb
set pagination off
set logging file tinyusb-lock-baseline.txt
set logging overwrite off
set logging on
set variable tusb_pico_trace_control.frozen = 1
p tusb_pico_trace_control
info threads
thread apply all bt full
p tusb_pico_trace_count
p tusb_pico_trace_self_wait_count
p tusb_pico_trace_first_self_wait
p tusb_pico_trace_last_event
p tusb_pico_trace_buffer
set logging off
```

The log is written in GDB's working directory (`pwd` shows it). Preserve it
with the exact ELF and test conditions. After capture, reset for the next
test; the freeze control only stops recording, not USB execution.

## Reading the records

The default storage is approximately 5 KB: 64 ring entries per core, the last
entry of each event type, and the first same-owner wait on each core. Override
`CFG_TUSB_PICO_TRACE_DEPTH` globally to change ring capacity. A core's newest
entry is at `(tusb_pico_trace_count[core] - 1) % depth` when its count is nonzero.
Use `sequence` to order a core's ring; sequence numbers are not shared between
cores. `timestamp_us` wraps approximately every 71 minutes.

| Event | Value | Object |
| --- | --- | --- |
| 1 LOCK_WAIT | Requested timeout in milliseconds | Mutex address |
| 2 LOCK_ACQUIRED | Requested timeout | Mutex address |
| 3 LOCK_FAILED | Requested timeout | Mutex address |
| 4 LOCK_RELEASE | Zero; recorded before release | Mutex address |
| 5 TASK_ENTER | 1: software IRQ; 0: port API | Global USB mutex |
| 6 TASK_EXIT | Same route identifier | Global USB mutex |
| 7 TASK_SKIPPED | Same route identifier | Global USB mutex |
| 8 CDC_WRITE | Requested byte count | CDC object |
| 9 CDC_PROGRESS | Bytes remaining after a successful write | CDC object |
| 10 CDC_DONE | Bytes accepted by the stack | CDC object |
| 11 CDC_FLUSH | CDC interface index | CDC object, or null for automatic flush |

On Cortex-M, `exception == 0` means foreground code and values of 16 or above
identify external interrupts. `irq_state` is the saved PRIMASK before recording:
zero means interrupts were enabled. `caller` is a return address; resolve it
against the matching ELF, for example with `info line *0x10012345`.

`owner` is an observational SDK owner snapshot, not a synchronized proof of
ownership. The first-self-wait record latches a nonzero-timeout attempt whose
sampled owner matches the calling owner ID. Pair it with both stacks, exception
context, and acquisition/release records to establish a deadlock.

For CDC, compare the latest WRITE, PROGRESS, and DONE records and timestamps
on the same core/object. Repeated task activity with no CDC progress is
different from a core stopped at LOCK_WAIT. Bytes accepted are not proof of
host receipt; the host must verify transfer integrity separately.

TASK events cover the RP2040 port's guarded task calls only; raw `tud_task()`
calls are not labeled as TASK events. Endpoint and FIFO mutex calls reached
from either route are traced. This first pass does not capture endpoint buffer
registers, hard faults, or full call stacks in RAM.

Recording briefly masks interrupts on the current core and restores their
previous state. It uses no heap, USB output, or additional blocking mutexes.
It supports ordinary interrupt nesting, not concurrent NMI/fault logging.
It changes timing, so failure to reproduce with tracing enabled is not proof
of a fix. RP2040 SDK compilation has been checked; RP2350 still needs its own
build and hardware validation.

## Focused host check

With Python 3 and a native C compiler (`CC` defaults to `cc`), run from this
library repository:

```sh
python3 tools/test_pico_trace.py
```

The harness compiles the actual recorder with a fake Pico runtime and checks
mutex result/timeout preservation, interrupt restoration and simulated nesting,
per-core isolation, ring wraparound, retained snapshots, and freeze behavior.
It does not replace target linking or hardware tests.