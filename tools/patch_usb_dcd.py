"""Checked, build-local repair of the pinned Arduino-ESP32 S3 USB DCD.

The original MIT source is retained in vendor/arduino-esp32-2.0.17.
Only the build directory copy and archive may be changed.
"""
from pathlib import Path
import hashlib
import os
import shutil
import subprocess

SOURCE_URL = "https://raw.githubusercontent.com/espressif/esp32-arduino-lib-builder/62d849ae49ac9b71c408bcc35039a5e487802806/components/arduino_tinyusb/src/dcd_esp32sx.c"
SOURCE_SHA256 = "63fa1b596ce892a8e9e8e81934e48f7d0f92133b7968c0707a9a3cfc2c01e0a8"
ARCHIVE_SHA256 = "45aea2035b431e8278920806f6a4fdd59893f93a012de75c076ee3b7c7ebdb51"
OBJECT_SHA256 = "38ac2ef0fc2e6faff0e004cf51c216f3b7080a228d1f3acf46efdebc6a2752a7"
MEMBER = "dcd_esp32sx.c.obj"

HELPERS = b'''
// PaperDIY: task scheduling and ISR retirement share this register across cores.
// Keep the lock around the RMW only; never publish an event while holding it.
static portMUX_TYPE paperdiy_txfe_mux = portMUX_INITIALIZER_UNLOCKED;
static void paperdiy_txfe_set_task(uint32_t mask)
{
  portENTER_CRITICAL(&paperdiy_txfe_mux);
  USB0.dtknqr4_fifoemptymsk |= mask;
  portEXIT_CRITICAL(&paperdiy_txfe_mux);
}
static void paperdiy_txfe_clear_task(uint32_t mask)
{
  portENTER_CRITICAL(&paperdiy_txfe_mux);
  USB0.dtknqr4_fifoemptymsk &= ~mask;
  portEXIT_CRITICAL(&paperdiy_txfe_mux);
}
static void paperdiy_txfe_clear_isr(uint32_t mask)
{
  portENTER_CRITICAL_ISR(&paperdiy_txfe_mux);
  USB0.dtknqr4_fifoemptymsk &= ~mask;
  portEXIT_CRITICAL_ISR(&paperdiy_txfe_mux);
}
'''

COMPLETION = b'''      // IN XFER complete (entire xfer).
      if (USB0.in_ep_reg[n].diepint & USB_D_XFERCOMPL0_M) {
        ESP_EARLY_LOGV(TAG, "TUSB IRQ - IN XFER complete!");
        USB0.in_ep_reg[n].diepint = USB_D_XFERCOMPL0_M;
        dcd_event_xfer_complete(0, n | TUSB_DIR_IN_MASK, xfer->total_len, XFER_RESULT_SUCCESS, true);
        if (!(USB0.in_ep_reg[n].diepint & USB_D_TXFEMP0_M)) {
          ESP_EARLY_LOGE(TAG, "Complete but not empty: %u/%u", xfer->queued_len, xfer->total_len);//todo:
        }
      }
'''

RETIRED_COMPLETION = b'''      // IN XFER complete (entire xfer).
      if (USB0.in_ep_reg[n].diepint & USB_D_XFERCOMPL0_M) {
        ESP_EARLY_LOGV(TAG, "TUSB IRQ - IN XFER complete!");
        const uint16_t completed_len = xfer->total_len;
        const uint16_t completed_queued = xfer->queued_len;
        const uint32_t completed_flags = USB0.in_ep_reg[n].diepint;
        // Retire the old TXFE before another core can consume completion and
        // arm this endpoint again. TXFEMP itself is read-only, not W1C.
        paperdiy_txfe_clear_isr(1u << n);
        USB0.in_ep_reg[n].diepint = USB_D_XFERCOMPL0_M | USB_D_TIMEOUT0_M;
        if (!(completed_flags & USB_D_TXFEMP0_M)) {
          ESP_EARLY_LOGE(TAG, "Complete but not empty: %u/%u", completed_queued, completed_len);
        }
        dcd_event_xfer_complete(0, n | TUSB_DIR_IN_MASK, completed_len, XFER_RESULT_SUCCESS, true);
        // No old-transfer state or register access after event publication.
        continue;
      }
'''


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def patched_source(original):
    if sha256(original) != SOURCE_SHA256:
        raise ValueError("unsupported USB DCD source checksum")
    replacements = (
        (b'#include "freertos/xtensa_api.h"', b'#include "freertos/FreeRTOS.h"\n#include "freertos/portmacro.h"\n#include "freertos/xtensa_api.h"'),
        (b'static intr_handle_t usb_ih;\n', b'static intr_handle_t usb_ih;\n' + HELPERS),
        (b'USB0.dtknqr4_fifoemptymsk &= ~(0x7F);', b'paperdiy_txfe_clear_task(0x7Fu);'),
        (b'USB0.dtknqr4_fifoemptymsk |= (1 << epnum);', b'paperdiy_txfe_set_task(1u << epnum);'),
        (b'USB0.dtknqr4_fifoemptymsk &= ~(1 << n);', b'paperdiy_txfe_clear_isr(1u << n);'),
        (COMPLETION, RETIRED_COMPLETION),
    )
    for before, after in replacements:
        if original.count(before) != 1:
            raise ValueError("unsupported USB DCD source shape")
        original = original.replace(before, after, 1)
    return original


def write_copy(original, output):
    original, output = Path(original), Path(output)
    if original.resolve() == output.resolve():
        raise ValueError("refusing to overwrite original DCD source")
    patched = patched_source(original.read_bytes())
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != patched:
        output.write_bytes(patched)
    return sha256(patched)


def replace_archive(original, replacement_object, output, ar, environment=None):
    original, replacement_object, output = map(Path, (original, replacement_object, output))
    if output.resolve() in (original.resolve(), replacement_object.resolve()):
        raise ValueError("refusing to overwrite a build input")
    original_bytes = original.read_bytes()
    if sha256(original_bytes) != ARCHIVE_SHA256:
        raise ValueError("unsupported USB DCD archive checksum")

    def run(*args):
        return subprocess.check_output([str(ar), *map(str, args)], env=environment)

    members = run("t", original).decode().splitlines()
    if members.count(MEMBER) != 1 or replacement_object.name in members:
        raise ValueError("unsupported USB DCD archive members")
    if sha256(run("p", original, MEMBER)) != OBJECT_SHA256:
        raise ValueError("unsupported USB DCD object checksum")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    try:
        shutil.copyfile(original, temporary)
        run("d", temporary, MEMBER)
        run("rcs", temporary, replacement_object)
        expected = [member for member in members if member != MEMBER] + [replacement_object.name]
        if run("t", temporary).decode().splitlines() != expected:
            raise ValueError("USB DCD archive replacement verification failed")
        if original.read_bytes() != original_bytes:
            raise ValueError("original USB archive changed during build")
        os.replace(temporary, output)
    finally:
        if temporary.exists():
            temporary.unlink()
