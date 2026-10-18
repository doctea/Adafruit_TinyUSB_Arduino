# RP2 USB lock tracing

This instrumentation was used to diagnose RP2040 TinyUSB lockups. The port now
leaves USB hardware event capture in the DCD interrupt and runs `tud_task()`
through the foreground `TinyUSB_Device_Task()` API instead of a software IRQ.
This avoids running class transfer callbacks in an interrupt that can preempt
foreground endpoint operations. It does not change mutex timeouts or CDC write
behavior, enable USB debug printing, or assign UART pins.

## Minimal runtime fix

The RP2040 port no longer installs the software task IRQ or its shared USB IRQ
trigger. The DCD hardware interrupt still captures and queues USB events.
`TinyUSB_Device_Task()` retains its nonblocking mutex guard, including re-entry
protection, and the public `TinyUSBDevice.task()` helper uses it on RP2040 only.
Other MCU implementations of that helper are unchanged.

Applications must service the task regularly in foreground code. Arduino-Pico
also calls the guarded API from its core; cores without built-in support need
explicit polling. Long blocking application work can now delay task progress;
this service-contract change needs broader sketch/board testing before an
upstream PR. The task mutex alone does not serialize arbitrary endpoint calls
from application interrupts or the other core.

Compulidian therefore also keeps MIDI parsing in its foreground loop and queues
interrupt-produced outgoing notes for foreground transmission. Local gates and
sample triggering remain immediate. Its reliable SysEx retry path also uses
the guarded task API. The project-side queue and library-side task fix address
different captured interrupted-owner deadlocks; neither replaces the other.

Tracing is diagnostic-only and defaults to `CFG_TUSB_PICO_TRACE=0`. Keep the
recorder and hooks for future captures; disabling them does not disable the fix.
No mutex timeout reduction or recursive-mutex workaround is needed.

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

In VS Code, select a configuration whose `projectEnvName` and executable both
match this environment, and upload that same build before attaching. A normal
firmware image with trace-ELF symbols produces misleading stack and object
addresses. Confirm serial `V` reports `ENV_NAME: rpipico_16mb_usb_trace` and the
expected build time. Alternatively, start a matching GDB session from the
Compulidian directory:

```sh
pio debug -e rpipico_16mb_usb_trace --interface=gdb
```

Start debugging before reproducing the failure; normal debug startup may reset
the board. Do not restart a debug session or reset after the failure, because
the trace is ordinary RAM and does not survive a reset. Use `continue` to run
from the initial breakpoint.

## Stress and failure capture

Compulidian has survived incoming Ableton MIDI clock, outgoing MIDI notes, and
parameter streaming over CDC together, including extreme-tempo stress. This is
evidence for the tested RP2040 setup, not a guarantee for every USB entry point
or board. Keep the foreground MIDI polling, TX queue, and task fix enabled.

Remaining release checks are the same stress with tracing disabled, repeated
Start/Stop with note-off verification, USB disconnect/reconnect, and a console
whose host stops reading. Disconnected and connected-but-not-reading CDC are
different conditions; CDC writes can still block and delay foreground USB
service. RP2350 still needs a separate build and hardware test. Slow tempo
reacquisition under extreme clock changes is a separate clock-following issue;
it has not been diagnosed as another USB deadlock.

If another freeze occurs, record the host applications and which streams are
active, then capture without resetting as below.

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
| 5 TASK_ENTER | 0: port API | Global USB mutex |
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

TASK events cover the RP2040 port's guarded task calls only. Direct raw
`tud_task()` calls bypass this mutex and are not labeled as TASK events.
Endpoint and FIFO mutex calls reached from the guarded route are traced. This
first pass does not capture endpoint buffer registers, hard faults, or full
call stacks in RAM.

Recording briefly masks interrupts on the current core and restores their
previous state. It uses no heap, USB output, or additional blocking mutexes.
It supports ordinary interrupt nesting, not concurrent NMI/fault logging.
It changes timing, so failure to reproduce with tracing enabled is not proof
of a fix. RP2040 SDK compilation has been checked; RP2350 still needs its own
build and hardware validation.

## Focused host check

With Python 3 and native C/C++ compilers (`CC` defaults to `cc`, `CXX` to
`c++`), run from this library repository:

```sh
python3 tools/test_pico_trace.py
```

The harness compiles the actual recorder with a fake Pico runtime and checks
mutex result/timeout preservation, interrupt restoration and simulated nesting,
per-core isolation, ring wraparound, retained snapshots, and freeze behavior.
It also compiles the actual guarded/public task function bodies with stubbed
dependencies, checking lock contention, re-entry, task routing and polling-core
CDC flushing for RP2040, mbed RP2040 and a generic platform, with tracing on and
off. It does not replace target linking, USB hardware or scheduling tests.