# Power behavior

[English](power.md) | [简体中文](power.zh-CN.md)

This document describes the implemented idle policy and its technical limits. Source review and host tests cannot establish electrical current, battery life or physical wake reliability. Those require controlled measurements with synthetic documents and stated conditions.

## Policy

After one second without activity, lower the CPU from 240 MHz to 80 MHz.
USB book transfer is disabled by default; `PAPERDIY_EXPERIMENTAL_USB_MSC=1` is
reserved for private experimental builds. When enabled, exclusive MSC ownership
is the exception: enter at 240 MHz and retain
that clock until explicit host eject and successful raw-driver release,
including exit-requested and fault states. Raw SD callbacks run in the USB task,
so application byte-delta polling cannot safely decide when to downclock.
This conservative choice avoids clock transitions during raw I/O and gives
predictable first-burst latency at the cost of higher connected-idle CPU power.
This clock policy alone does not establish USB reliability or quantify power
cost.

On battery power, also enter manual light sleep with a one-second timer fallback.
On USB or other external power, only lower the CPU frequency; keep the processor
awake so USB enumeration, serial access, and firmware upload remain available.
An active AP or STA also blocks manual light sleep, which would suspend Wi-Fi;
idle CPU throttling remains enabled. Leaving settings disables the radio and
restores the ordinary battery sleep policy.

N runs its TLS listener only while explicitly selected. Leaving N closes the
listener and connections, discards unfinished reception, preserves any
publication intent whose result is uncertain, and requests Wi-Fi shutdown.
If shutdown fails or the SDK still reports an active radio, the sleep gate
remains blocked. An offline sender queue never wakes or enables the reader.
These lifecycle checks do not establish a measured radio-off current.

Restore 240 MHz before processing input, loading a book, or refreshing the screen.

Only sleep at the application loop boundary: no loading, display update, storage
operation, pending card debounce, held button, chord, or input release guard.
Wait for display work to finish before entering sleep. Timer wakeups service
power/button/card state without drawing a new scene, then allow another sleep.
Check every wake-source setup result and the sleep return value; a rejected
sleep must return to normal input processing rather than cause a busy retry loop.

| Input | Wake condition |
| --- | --- |
| A, GPIO4 | Low level |
| B, GPIO3 | Low level |
| Card detect, GPIO48 | Opposite of the stable card level: high for removal, low for insertion |
| Timer | One second; checks external power and the PMIC button event |

Use `gpio_wakeup_enable()` and `esp_sleep_enable_gpio_wakeup()` for the three
GPIOs. Card detection still uses the application debounce; remain awake while
a card transition is pending. Keep ordinary button processing after wakeup:
do not reset it or call `discardUntilReleased()`, which would consume the press
that woke the reader. Do not sleep while a long press is being timed.

The pinned M5Unified driver reads the power-button event from M5PM1 register
`0x42`. It does not configure a PaperDIY `_wakeupPin`, so its default
`Power.lightSleep()` wakeup configuration is insufficient. The timer provides
PMIC event polling; it does not change the power button's independent hardware
reset or power-off behavior.

There is no automatic shutdown. Light sleep preserves the loaded book, page
index, framebuffer, and navigation state. It must not restart the application,
remount an unchanged SD card, or reload a book after each pause.

### Card interrupts across sleep

Awake card detection uses a GPIO48 edge interrupt to revoke source-validation
proofs even while a long synchronous import blocks the application loop.
On the pinned ESP-IDF 4.4.7 ESP32-S3 backend, `gpio_ll_wakeup_enable()` writes
the same `pin.int_type` field used by that interrupt; disabling wakeup only
clears `wakeup_enable`. Leaving the wake level installed would turn the awake
card ISR into a continuously retriggering level interrupt.

The application-task `card_sleep::Window` disables the card interrupt before
arming the opposite wake level. Normal wake, cancellation, setup failure and
sleep failure all disable wakeup, restore `GPIO_INTR_ANYEDGE`, then re-enable
the interrupt. If restoring edge mode fails, the interrupt stays disabled and
the storage lease is revoked. A changed physical level requests the usual card
debounce; ordinary timer/key wakes do not synthesize insertion events.

Each sleep attempt also drops in-memory source fingerprints and resident-open
proofs with `sourceChanged()`. This conservative rule covers a remove/reinsert
pair while the edge ISR is disabled. The loaded page and its page table remain
usable; reopening a book after sleep requires source validation again. Keeping
validation proofs across sleep is intentionally deferred until hardware can
identify every card transition during that interval.

Host `card_sleep` tests cover both card levels, normal/cancelled/failed windows,
each GPIO adapter failure, cleanup ordering and proof invalidation. The built
card callback contains only a DRAM atomic-byte store in IRAM, with no allocation
or runtime initialization calls. The pinned Arduino GPIO dispatcher itself is
flash-resident, so this does not claim an entirely IRAM-resident GPIO service.

Diagnostic builds expose `powerDiagnosticSleep(1..1000)` for an explicitly
requested short sleep while USB is attached. It uses the same production guard
and returns success only after sleep and interrupt restoration succeed. Use it
with the display and radio idle, then check GPIO48's interrupt type, storage
lease and retained page. Normal USB/external-power sleep blocking is unchanged.
This diagnostic alone does not establish battery wake or power consumption.

## Battery icon updates

The existing library sampling interval is unchanged. Redraw only when the
18-pixel interior fill changes; percentages and charge transitions that produce
identical pixels do not request a display update. Continue tracking the latest
discharge estimate even when no refresh is needed. Initial paint and scene or
rotation changes still draw the whole icon. The [host comparison](research-2026-10.md)
verifies identical pixel traces with fewer requests; current/energy savings
have not been measured.

## USB and power-source detection

The USB build flag selects the transport. With `ARDUINO_USB_MODE=1`, `Serial`
is the USB Serial/JTAG `HWCDC` implementation and USB storage is unavailable.
With `ARDUINO_USB_MODE=0`, native TinyUSB provides CDC; MSC remains disabled
unless the private experimental compile gate is explicitly enabled. Check the selected build configuration; these are distinct
firmware variants. Power code uses `usb_transport` for both variants.

In the HWCDC variant, `HWCDC::isPlugged()` detects host SOF packets, while
`bool(Serial)`/`isCDC_Connected()` additionally depend on CDC activity. The latter
cannot protect a connected computer whose serial terminal is closed. A suspended
host or a charge-only supply may not produce SOF packets at all.
The native variant blocks sleep while TinyUSB is configured or suspended,
and throughout exclusive MSC ownership even if the bus resets or disconnects.
Only an accepted explicit host eject and successful raw-driver removal permit
local FAT remount. Idle time, cable removal and a USB bus reset do not grant
that permission. CDC automatic reboot is disabled throughout host ownership;
recovery and application shutdown must wait for safe eject. USB sector activity
is not required to retain 240 MHz while MSC owns the card. Ordinary CDC without
MSC ownership may still throttle to 80 MHz.

Before sleeping, use a checked M5PM1 register read of `PWR_SRC` (`0x04`):
bit 0 is 5VIN, bit 1 is 5VINOUT, and bit 2 is the powered battery node.
Require a successful read, a known battery source, no external source, and no
USB transport sleep block. Treat failed or ambiguous reads as a reason to stay awake.
Recheck host connection immediately before entry. If USB is attached during
sleep, the timer bounds the next power-source check to about one second.
Enumeration after USB attachment during sleep still needs device acceptance.

MSC read/write callbacks release the storage mutex and wait at least one RTOS
tick on success, failure and lock contention, allowing other tasks to run
between callbacks. This does not preempt one synchronous SD operation. A
checksum-gated PlatformIO middleware compiles a build-local Arduino SD source
copy whose `sdcard_uninit()` releases its SPI guard before freeing the card;
the shared framework is not modified. These source fixes do not identify the
cause of a particular hardware disconnect.

Application console output is best effort. The native CDC adapter makes finite
TinyUSB FIFO attempts rather than entering the pinned SDK's unbounded
full-buffer `USBCDC::write()` loop. It yields while waiting, with a 100 ms
no-progress limit, a two-second individual-write limit and a ten-second total
bulk-frame limit for `SCREEN` and `PAGE_TEXT`. Long-running imports and other
diagnostic computations do not consume a frame deadline. A failed bulk frame
stops waiting on subsequent rows;
`SCREEN_DONE` is emitted only for a complete capture, with `SCREEN_ABORTED`
attempted after an interrupted one. A host draining within those deadlines receives
the full frame. Dropping logs does not eject storage or prove synchronization.
RX, serial initialization and the ownership-dependent reboot guard keep their
existing paths. CDC reopen behavior requires separate physical validation.

Do not infer that USB is absent from `isCharging()`: charging can stop while
the cable remains connected. Do not use a zero returned by `getPowerSource()`
or `getVBUSVoltage()` as a successful absence check; these convenience methods
also return zero on communication failure.

## Retained state and clocks

The source review uses PlatformIO `espressif32@6.12.0`, Arduino-ESP32 2.0.17
(`framework-arduinoespressif32` package `3.20017.241212+sha.dcc1105b`), and ESP-IDF **4.4.7**,
with the `dio_opi` SDK configuration:

- `CONFIG_PM_ENABLE` is off and FreeRTOS tickless idle is not enabled. The
  policy therefore uses explicit light sleep, not automatic PM configuration.
- OPI PSRAM runs at 80 MHz. PSRAM/flash leakage workarounds and the MSPI
  pull-up workaround are enabled.
- `CONFIG_FREERTOS_HZ=1000`; the configured task watchdog timeout is five seconds.
- Arduino `millis()` uses `esp_timer_get_time()`. Light-sleep wakeup advances
  this timer using RTC elapsed time. Periodic `esp_timer` callbacks can catch up
  after sleep; future timers should use `skip_unhandled_events` where suitable.
- The supported 80/240 MHz CPU settings use the same 480 MHz PLL with different
  CPU dividers. APB remains at 80 MHz; this change does not reconfigure the
  80 MHz MSPI clock. Use `setCpuFrequencyMhz()` and check its return value.
  Do not switch clocks during display or storage work, or use frequencies below
  80 MHz without a separate review.

Keep PSRAM, flash, the SD card, and the ESP32 supply powered. Do not force
`ESP_PD_DOMAIN_VDDSDIO` off, disable the PMIC DCDC rail, or globally isolate/hold
GPIOs. Keep the SD bus idle with CS high. No SD shutdown/reinitialization belongs
in this idle path.

Do not call `M5.Display.wakeup()` on CPU wake: CPU light sleep is not a display
reinitialization. Preserve the existing framebuffer and refresh policy, with
panel work owned by the display driver.

M5PM1 I2C idle sleep remains disabled. Its first I2C transaction after waking
may be unsuccessful; enabling that mode would require a separate review of
all register reads/writes and power-rail retention.

## Sources

The following are source evidence, not measurements on this reader:

- ESP-IDF 4.4.7: [sleep modes](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/sleep_modes.html),
  [timer behavior](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-reference/system/esp_timer.html),
  and [USB Serial/JTAG limitations](https://docs.espressif.com/projects/esp-idf/en/v4.4.7/esp32s3/api-guides/usb-serial-jtag-console.html).
- ESP-IDF 4.4.7 implementations:
  [`rtc_clk.c`](https://github.com/espressif/esp-idf/blob/v4.4.7/components/esp_hw_support/port/esp32s3/rtc_clk.c),
  [`sleep_modes.c`](https://github.com/espressif/esp-idf/blob/v4.4.7/components/esp_hw_support/sleep_modes.c),
  and [`sleep_gpio.c`](https://github.com/espressif/esp-idf/blob/v4.4.7/components/esp_hw_support/sleep_gpio.c).
- Arduino-ESP32 2.0.17:
  [`HWCDC.cpp`](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/HWCDC.cpp),
  [`esp32-hal-cpu.c`](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/esp32-hal-cpu.c),
  and [`esp32-hal-misc.c`](https://github.com/espressif/arduino-esp32/blob/2.0.17/cores/esp32/esp32-hal-misc.c).
- M5Unified at `e125e73f28b6b83e621e48df432af426bb2ff935`:
  [`M5Unified.inl`](https://github.com/m5stack/M5Unified/blob/e125e73f28b6b83e621e48df432af426bb2ff935/src/M5Unified.inl),
  [`Power_Class.inl`](https://github.com/m5stack/M5Unified/blob/e125e73f28b6b83e621e48df432af426bb2ff935/src/utility/Power_Class.inl),
  and [`M5PM1_Class.inl`](https://github.com/m5stack/M5Unified/blob/e125e73f28b6b83e621e48df432af426bb2ff935/src/utility/power/M5PM1_Class.inl).
- M5Stack's [PaperDIY power circuitry and PMIC behavior](https://docs.m5stack.com/en/arduino/paperdiy/m5pm1).
  These online hardware notes may change; SDK and library claims above refer
  to the pinned versions.

## Optional battery retention diagnostic

Build with both `PAPERDIY_DIAGNOSTICS` and `PAPERDIY_DEVICE_TESTS` to enable the
one-shot telemetry in [`tests/device_power.h`](../tests/device_power.h). These
flags are disabled in the normal build. Use a loaded synthetic document with
room to turn forward and back, and keep it open during the run.

1. With USB connected, send `POWER_ARM`. Save the returned eight-digit
   hexadecimal `nonce` on the host.
2. Disconnect USB and wait at least two seconds for idle. Press and release B
   once, wait for the page and another idle interval, then press and release A
   once to return. Check that each press produces exactly one page turn.
3. Leave the device on battery for at least 60 seconds, then reconnect USB.
   Wait at least three seconds for the device and serial connection.
4. Send `POWER_REPORT <nonce>` with the nonce from step 1. Require a matching,
   completed report; `armed`, `stale`, or `missing_or_invalid` is not a pass.

The observation window starts at the first observed sleep. Completion occurs
after 60 seconds, on detected external power after sleep, or by an explicit
`POWER_FINISH`. An early external-power completion does not establish a full
60-second run. `POWER_REPORT` without a nonce returns only the latest result;
it cannot establish that the result belongs to the current experiment.

Check `valid_book=1`, `sleep_seen=1`, `battery_sleep_seen=1`, matching text and
page-index checksums, and zero frequency/sleep failures. Review an unknown-source
flag separately: `unknown_source_seen` is sticky, and `sourceKnown` starts
false, so the flag can mean either not yet sampled or an actual I2C read failure.
It is not an I2C failure count. Unknown source still blocks runtime sleep. Confirm the intended final position and A/B action counts.
The telemetry checks text and page-index CRCs; framebuffer retention and visible
refresh behavior still need their own checks.

Arming and ordinary diagnostic polling only update RAM. Completion makes one
result-write attempt in the separate NVS namespace `powercheck`, with size,
schema, CRC, and read-back validation. A failed save is reported and polling
does not retry it. Reporting reads the result; it does not write preferences.
The diagnostic does not write the reader's bookmark namespace. Normal page-turn
bookmark persistence remains active. A reset before completion loses the active
experiment, so always retain and check its nonce.

Raw reports can include book identifiers and reading positions; keep them out
of public logs. Public test reports must use synthetic documents rather than
anonymized personal reading records.

## Hardware validation requirements

- Exercise hundreds of battery sleep/wake cycles while checking the loaded
  book, page offsets, and framebuffer for corruption. Compare a PSRAM checksum
  before/after a diagnostic cycle.
- Repeat first-press checks and cover hold/repeat, staggered A+B release, and
  the two-second shutdown chord after idle with controlled physical input.
- Insert/remove SD while asleep, including contact bounce; verify the existing
  mount/error behavior without rewriting books.
- Extend the connected USB check to a closed terminal, a suspended computer,
  a charger, and a fully charged battery. Attach USB while asleep and confirm
  enumeration, diagnostics, and upload recovery.
- Verify 80/240 MHz transitions across battery wakeups, timer monotonicity,
  absence of watchdog/reset loops, and unchanged partial-refresh behavior
  after wake.
- When a current meter is available, measure battery input with a fixed card,
  page, charge state, and firmware: active, CPU-only idle, light sleep, page
  refresh, and shutdown. Record instrument, duration, and conditions before
  making current or battery-life claims.

Public benchmarks should state the reproducible fixture, tested revision,
instrument and conditions. Keep personal device-operation logs outside this
implementation guide.
