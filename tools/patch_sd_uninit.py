#!/usr/bin/env python3
"""Reproducible Arduino-ESP32 2.0.17 SD uninit lifetime fix, without SDK writes.

The middleware only accepts this reviewed complete upstream source. The fix
ends AcquireSPI's lifetime before freeing its card. Public build inputs contain
no SDK checkout path; PlatformIO resolves the installed framework at build time.
"""
# The reviewed SD function below is derived from Arduino-ESP32 sd_diskio.cpp:
# Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD, Apache License 2.0.
# https://www.apache.org/licenses/LICENSE-2.0
# The generated complete source retains its original copyright/license header.
import hashlib
from pathlib import Path

SOURCE_SHA256 = "0401e23009f06e3a418470a144403e89727f3d007b1d16d429dc370ce8bdc5f0"
FUNCTION = b'''uint8_t sdcard_uninit(uint8_t pdrv)
{
    ardu_sdcard_t * card = s_cards[pdrv];
    if (pdrv >= FF_VOLUMES || card == NULL) {
        return 1;
    }
    AcquireSPI lock(card);
    sdTransaction(pdrv, GO_IDLE_STATE, 0, NULL);
    ff_diskio_register(pdrv, NULL);
    s_cards[pdrv] = NULL;
    esp_err_t err = ESP_OK;
    if (card->base_path) {
        err = esp_vfs_fat_unregister_path(card->base_path);
        free(card->base_path);
    }
    free(card);
    return err;
}'''
REPLACEMENT = b'''uint8_t sdcard_uninit(uint8_t pdrv)
{
    // PaperDIY build-local lifetime fix for Arduino-ESP32 2.0.17.
    // AcquireSPI's destructor dereferences card; it must precede free(card).
    if (pdrv >= FF_VOLUMES) {
        return 1;
    }
    ardu_sdcard_t * card = s_cards[pdrv];
    if (card == NULL) {
        return 1;
    }
    esp_err_t err = ESP_OK;
    {
        AcquireSPI lock(card);
        sdTransaction(pdrv, GO_IDLE_STATE, 0, NULL);
        ff_diskio_register(pdrv, NULL);
        s_cards[pdrv] = NULL;
        if (card->base_path) {
            err = esp_vfs_fat_unregister_path(card->base_path);
            free(card->base_path);
        }
    }
    free(card);
    return err;
}'''


def rewrite_verified_shape(source: bytes) -> bytes:
    """Separate shape operation so an offline minimal C++ fixture can test it."""
    if source.count(FUNCTION) != 1:
        raise ValueError("unsupported sdcard_uninit source shape")
    return source.replace(FUNCTION, REPLACEMENT, 1)


def patched_source(source: bytes) -> bytes:
    if hashlib.sha256(source).hexdigest() != SOURCE_SHA256:
        raise ValueError("unsupported Arduino SD source checksum; review before updating the patch")
    return rewrite_verified_shape(source)


def write_copy(original: Path, output: Path) -> str:
    source = original.read_bytes()
    patched = patched_source(source)
    if original.resolve() == output.resolve():
        raise ValueError("refusing to overwrite the framework source")
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.exists() or output.read_bytes() != patched:
        output.write_bytes(patched)
    return hashlib.sha256(patched).hexdigest()
