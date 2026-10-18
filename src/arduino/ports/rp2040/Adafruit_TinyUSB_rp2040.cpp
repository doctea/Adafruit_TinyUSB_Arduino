/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2019, hathach for Adafruit
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "tusb_option.h"

#if defined ARDUINO_ARCH_RP2040 && CFG_TUD_ENABLED

#include "Arduino.h"

// mbed old pico-sdk need to wrap with cpp
extern "C" {
#include "hardware/flash.h"
#include "pico/bootrom.h"
#include "pico/mutex.h"
}

#include "arduino/Adafruit_TinyUSB_API.h"
#include "tusb.h"

//--------------------------------------------------------------------+
// Earle Philhower and mbed specific
//--------------------------------------------------------------------+

// mbed use old pico-sdk does not have unique_id
#ifdef ARDUINO_ARCH_MBED

#define get_unique_id(_serial) flash_get_unique_id(_serial)

#else

#include "pico/unique_id.h"
#define get_unique_id(_serial)                                                 \
  pico_get_unique_board_id((pico_unique_board_id_t *)_serial);

#endif

//--------------------------------------------------------------------+
// Porting API
//--------------------------------------------------------------------+

// Big, global USB mutex, shared with all USB devices to make sure we don't
// have multiple cores updating the TUSB state in parallel
mutex_t __usb_mutex;

void TinyUSB_Port_InitDevice(uint8_t rhport) {
  mutex_init(&__usb_mutex);

  const tusb_rhport_init_t rh_init = {
      .role = TUSB_ROLE_DEVICE,
      .speed = TUSB_SPEED_FULL,
  };
  tusb_init(rhport, &rh_init);
}

void TinyUSB_Port_EnterDFU(void) {
  reset_usb_boot(0, 0);
  while (1) {
  }
}

uint8_t TinyUSB_Port_GetSerialNumber(uint8_t serial_id[16]) {
  get_unique_id(serial_id);
  return FLASH_UNIQUE_ID_SIZE_BYTES;
}

//--------------------------------------------------------------------+
// Core API
// Serialize foreground TinyUSB task calls against calls made by the core.
//--------------------------------------------------------------------+

extern "C" {

void TinyUSB_Device_Task(void) {
  if (mutex_try_enter(&__usb_mutex, NULL)) {
#if CFG_TUSB_OS == OPT_OS_PICO && CFG_TUSB_PICO_TRACE
    tusb_pico_trace_record(TUSB_PICO_TRACE_TASK_ENTER, &__usb_mutex, 0,
                           __usb_mutex.owner, (uintptr_t)__builtin_return_address(0));
#endif
    tud_task();
#if CFG_TUSB_OS == OPT_OS_PICO && CFG_TUSB_PICO_TRACE
    tusb_pico_trace_record(TUSB_PICO_TRACE_TASK_EXIT, &__usb_mutex, 0,
                           __usb_mutex.owner, (uintptr_t)__builtin_return_address(0));
#endif
    mutex_exit(&__usb_mutex);
#if CFG_TUSB_OS == OPT_OS_PICO && CFG_TUSB_PICO_TRACE
  } else {
    tusb_pico_trace_record(TUSB_PICO_TRACE_TASK_SKIPPED, &__usb_mutex, 0,
                           __usb_mutex.owner, (uintptr_t)__builtin_return_address(0));
#endif
  }
}
}

#endif
