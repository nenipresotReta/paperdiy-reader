# Pinned USB device controller source

`dcd_esp32sx.c` is the unmodified MIT-licensed Espressif TinyUSB controller
source from esp32-arduino-lib-builder commit
`62d849ae49ac9b71c408bcc35039a5e487802806`:

[Upstream source](https://github.com/espressif/esp32-arduino-lib-builder/blob/62d849ae49ac9b71c408bcc35039a5e487802806/components/arduino_tinyusb/src/dcd_esp32sx.c).

Source SHA-256:
`63fa1b596ce892a8e9e8e81934e48f7d0f92133b7968c0707a9a3cfc2c01e0a8`.
The copyright and complete license remain in the source.

The source was checked against the Arduino-ESP32 2.0.17 / ESP-IDF 4.4.7
ESP32-S3 archive used by this project: all 30 nonempty text/literal sections
and 414 code/constant relocation entries matched a rebuild with its pinned
compiler and flags. This identifies the historical source even though later
lib-builder versions contain a different controller implementation.

`tools/patch_usb_dcd.py` checks the source, SDK archive and original object
identities. `tools/pio_usb_dcd.py` compiles a patched copy inside the build
directory and substitutes a copied SDK archive containing that object. It
does not modify the installed SDK. Unsupported identities stop the build.

The patch serializes all three TX FIFO-empty mask updates with a shared
task/ISR spinlock. It also retires the completed transfer's mask before
publishing completion, then skips the old transfer's remaining processing.
TXFEMP is read-only; clearing the mask retires its interrupt. The lock is
released before event publication. These changes address reachable lost
updates and same-endpoint completion/rearm ordering; offline tests and a
successful build do not establish the cause of a particular hardware stall.

Run the portable checks with `python3 tests/usb_dcd_patch.py`. Its optional
`--framework` and `--ar` arguments additionally verify archive substitution
against an installed pinned SDK without changing it.
