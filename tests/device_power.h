#pragma once
#include "console.h"
#ifdef PAPERDIY_DEVICE_TESTS
// One opt-in, one-book retention check. Polling is RAM-only; completion writes
// one result blob in its own namespace, never the reader's progress namespace.
// Save POWER_ARM's nonce on the host and use POWER_REPORT <nonce> after USB
// reconnect/reset. An unqualified report identifies only the latest saved run.
#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#include <esp_rom_crc.h>
#include <cstddef>
#include <cstring>
#include "input.h"
#include "power.h"
#include "reader.h"

namespace device_power_detail {
constexpr uint32_t MAGIC = 0x50575243;
constexpr uint32_t SCHEMA = 1;
constexpr uint32_t OBSERVATION_MS = 60000;
enum Flag : uint32_t {
  BookChanged = 1, MissingBook = 2, SleepSeen = 4,
  BatterySleepSeen = 8, UnknownSourceSeen = 16
};
enum Reason : uint32_t { Explicit = 1, Timeout = 2, ExternalPower = 3 };
enum Match : uint32_t { Unchecked = 0, Matches = 1, Differs = 2 };

// Fixed-width schema. The CRC covers all bytes before resultCrc; the whole
// record is zero-filled first, including alignment bytes.
struct Record {
  uint32_t magic, schema, bytes, nonce;
  uint64_t sleepUs;
  uint32_t flags, reason, elapsedMs, observationMs;
  uint32_t sleeps, timerWakes, gpioWakes, sleepFailures;
  uint32_t frequencyChanges, frequencyFailures;
  uint32_t originalPage, originalOffset, lastPage, lastOffset;
  uint32_t originalLength, originalPages, lastLength, lastPages;
  uint32_t textCrcBefore, textCrcAfter, indexCrcBefore, indexCrcAfter;
  uint32_t textMatch, indexMatch;
  uint32_t aActions, bActions, confirmActions, shutdownActions;
  uint32_t source, sourceKnown, usbHost, lastWakeKeys, cpuMHz;
  char originalKey[16], lastKey[16];
  uint32_t resultCrc;
};
static_assert(sizeof(Record) == 192, "Schema 1 has a fixed 192-byte layout");

struct State {
  bool armed = false, haveResult = false, saved = false;
  uint32_t armedAt = 0, firstSleepAt = 0;
  String identity, key;
  IdlePowerStats before{};
  Record record{};
};

inline State& state() { static State value; return value; }

inline bool loaded() {
  return book && bookLength && bookLength <= MAX_BOOK &&
         !pageOffsets.empty() && pageOffsets.size() <= 65536 &&
         pageNumber < pageOffsets.size() && !bookIdentity.isEmpty() &&
         !bookmarkKey.isEmpty() && bookmarkKey.length() < 16;
}

inline uint32_t checksum(const void* data, size_t bytes) {
  return esp_rom_crc32_le(0, static_cast<const uint8_t*>(data), uint32_t(bytes));
}

inline uint32_t recordChecksum(const Record& record) {
  return checksum(&record, offsetof(Record, resultCrc));
}

inline void copyKey(char (&destination)[16], const String& key) {
  memset(destination, 0, sizeof(destination));
  size_t bytes = key.length() < sizeof(destination) ? key.length() : sizeof(destination) - 1;
  memcpy(destination, key.c_str(), bytes);
}

inline bool validRecord(const Record& record) {
  return record.magic == MAGIC && record.schema == SCHEMA &&
         record.bytes == sizeof(Record) && record.nonce &&
         memchr(record.originalKey, 0, sizeof(record.originalKey)) &&
         memchr(record.lastKey, 0, sizeof(record.lastKey)) &&
         record.resultCrc == recordChecksum(record);
}

inline bool readResult(Record& record) {
  Preferences storage;
  if (!storage.begin("powercheck", true)) return false;
  bool ok = storage.getBytesLength("result") == sizeof(record) &&
            storage.getBytes("result", &record, sizeof(record)) == sizeof(record);
  storage.end();
  return ok && validRecord(record);
}

inline bool writeResult(const Record& record) {
  Preferences storage;
  if (!storage.begin("powercheck", false)) return false;
  bool written = storage.putBytes("result", &record, sizeof(record)) == sizeof(record);
  Record check{};
  bool verified = written && storage.getBytesLength("result") == sizeof(check) &&
                  storage.getBytes("result", &check, sizeof(check)) == sizeof(check) &&
                  validRecord(check) && memcmp(&record, &check, sizeof(check)) == 0;
  storage.end();
  return verified;
}

inline int matchValue(uint32_t match) {
  return match == Matches ? 1 : match == Differs ? 0 : -1;
}

inline void printResult(const Record& record, bool persisted) {
  bool validBook = !(record.flags & (BookChanged | MissingBook));
  Console.printf(
      "POWER_RESULT nonce=%08x schema=%u persisted=%d reason=%u valid_book=%d "
      "book_changed=%d missing_book=%d sleep_seen=%d battery_sleep_seen=%d "
      "unknown_source_seen=%d elapsed_ms=%u observation_ms=%u sleep_us=%llu "
      "sleeps=%u timer_wakes=%u gpio_wakes=%u rejects=%u freq_changes=%u freq_fail=%u "
      "text_match=%d index_match=%d text_crc=%08x/%08x index_crc=%08x/%08x "
      "original_page=%u original_offset=%u last_page=%u last_offset=%u "
      "original_key=%s last_key=%s original_bytes=%u last_bytes=%u "
      "original_pages=%u last_pages=%u a_actions=%u b_actions=%u confirm_actions=%u "
      "shutdown_actions=%u source=%u source_known=%u usb_host=%u wake_keys=%u cpu_mhz=%u\n",
      unsigned(record.nonce), unsigned(record.schema), persisted, unsigned(record.reason),
      validBook, bool(record.flags & BookChanged), bool(record.flags & MissingBook),
      bool(record.flags & SleepSeen), bool(record.flags & BatterySleepSeen),
      bool(record.flags & UnknownSourceSeen), unsigned(record.elapsedMs),
      unsigned(record.observationMs), static_cast<unsigned long long>(record.sleepUs),
      unsigned(record.sleeps), unsigned(record.timerWakes), unsigned(record.gpioWakes),
      unsigned(record.sleepFailures), unsigned(record.frequencyChanges),
      unsigned(record.frequencyFailures), matchValue(record.textMatch),
      matchValue(record.indexMatch), unsigned(record.textCrcBefore),
      unsigned(record.textCrcAfter), unsigned(record.indexCrcBefore),
      unsigned(record.indexCrcAfter), unsigned(record.originalPage),
      unsigned(record.originalOffset), unsigned(record.lastPage),
      unsigned(record.lastOffset), record.originalKey, record.lastKey,
      unsigned(record.originalLength), unsigned(record.lastLength),
      unsigned(record.originalPages), unsigned(record.lastPages),
      unsigned(record.aActions), unsigned(record.bActions), unsigned(record.confirmActions),
      unsigned(record.shutdownActions), unsigned(record.source), unsigned(record.sourceKnown),
      unsigned(record.usbHost), unsigned(record.lastWakeKeys), unsigned(record.cpuMHz));
}

inline void observe(uint32_t now, const IdlePowerStats& stats) {
  State& test = state();
  Record& record = test.record;
  if (bookIdentity != test.identity || bookmarkKey != test.key) record.flags |= BookChanged;
  if (!loaded()) record.flags |= MissingBook;
  if (!stats.sourceKnown) record.flags |= UnknownSourceSeen;
  uint32_t sleeps = stats.sleeps - test.before.sleeps;
  if (sleeps && !(record.flags & SleepSeen)) {
    uint64_t sleptMs = (stats.sleepUs - test.before.sleepUs) / 1000;
    uint32_t elapsed = now - test.armedAt;
    uint32_t back = sleptMs < elapsed ? uint32_t(sleptMs) : elapsed;
    test.firstSleepAt = now - back;
    record.flags |= SleepSeen;
  }
  if (sleeps > record.sleeps && stats.sourceKnown && (stats.powerSource & 4) &&
      !(stats.powerSource & 3) && !stats.usbHost)
    record.flags |= BatterySleepSeen;
  record.sleeps = sleeps;
}

inline void finish(Reason reason) {
  State& test = state();
  if (!test.armed) return;
  const IdlePowerStats stats = idlePowerStats();
  uint32_t now = millis();
  observe(now, stats);
  Record& record = test.record;
  record.reason = reason;
  record.elapsedMs = now - test.armedAt;
  record.observationMs = (record.flags & SleepSeen) ? now - test.firstSleepAt : 0;
  record.sleepUs = stats.sleepUs - test.before.sleepUs;
  record.timerWakes = stats.timerWakes - test.before.timerWakes;
  record.gpioWakes = stats.gpioWakes - test.before.gpioWakes;
  record.sleepFailures = stats.sleepFailures - test.before.sleepFailures;
  record.frequencyChanges = stats.frequencyChanges - test.before.frequencyChanges;
  record.frequencyFailures = stats.frequencyFailures - test.before.frequencyFailures;
  record.source = stats.powerSource;
  record.sourceKnown = stats.sourceKnown;
  record.usbHost = stats.usbHost;
  record.lastWakeKeys = stats.lastWakeKeys;
  record.cpuMHz = getCpuFrequencyMhz();
  powerActivity();
  if (loaded()) {
    record.lastPage = pageNumber + 1;
    record.lastOffset = pageOffsets[pageNumber];
    record.lastLength = uint32_t(bookLength);
    record.lastPages = uint32_t(pageOffsets.size());
    copyKey(record.lastKey, bookmarkKey);
    // A different book is an invalid experiment, not a RAM-corruption result.
    if (!(record.flags & (BookChanged | MissingBook))) {
      record.textCrcAfter = checksum(book, bookLength);
      record.indexCrcAfter = checksum(pageOffsets.data(), pageOffsets.size() * sizeof(uint32_t));
      record.textMatch = record.originalLength == record.lastLength &&
                         record.textCrcBefore == record.textCrcAfter ? Matches : Differs;
      record.indexMatch = record.originalPages == record.lastPages &&
                          record.indexCrcBefore == record.indexCrcAfter ? Matches : Differs;
    }
  }
  record.resultCrc = recordChecksum(record);
  // Finish is one-shot even on a storage failure; ordinary polling never retries.
  test.armed = false;
  test.haveResult = true;
  test.saved = writeResult(record);
  printResult(record, test.saved);
  if (!test.saved) Console.println("POWER_RESULT_SAVE_FAILED namespace=powercheck");
  powerActivity();
}

inline void arm() {
  State& test = state();
  if (test.armed) {
    Console.printf("POWER_ARM already_armed=1 nonce=%08x\n", unsigned(test.record.nonce));
    return;
  }
  if (!loaded()) {
    Console.println("POWER_ARM failed=no_loaded_book");
    return;
  }
  powerActivity();
  memset(&test.record, 0, sizeof(test.record));
  test.record.magic = MAGIC;
  test.record.schema = SCHEMA;
  test.record.bytes = sizeof(Record);
  do { test.record.nonce = esp_random(); } while (!test.record.nonce);
  test.identity = bookIdentity;
  test.key = bookmarkKey;
  test.record.originalPage = pageNumber + 1;
  test.record.originalOffset = pageOffsets[pageNumber];
  test.record.originalLength = uint32_t(bookLength);
  test.record.originalPages = uint32_t(pageOffsets.size());
  copyKey(test.record.originalKey, bookmarkKey);
  test.record.textCrcBefore = checksum(book, bookLength);
  test.record.indexCrcBefore = checksum(pageOffsets.data(), pageOffsets.size() * sizeof(uint32_t));
  test.before = idlePowerStats();
  test.armedAt = millis();
  test.firstSleepAt = 0;
  test.haveResult = test.saved = false;
  test.armed = true;
  Console.printf("POWER_ARM nonce=%08x page=%u offset=%u key=%s bytes=%u pages=%u "
                "text_crc=%08x index_crc=%08x window_ms=%u nvs_writes=0\n",
                unsigned(test.record.nonce), unsigned(test.record.originalPage),
                unsigned(test.record.originalOffset), test.record.originalKey,
                unsigned(test.record.originalLength), unsigned(test.record.originalPages),
                unsigned(test.record.textCrcBefore), unsigned(test.record.indexCrcBefore),
                unsigned(OBSERVATION_MS));
  powerActivity();
}

inline bool parseNonce(const char* text, uint32_t& value) {
  if (strlen(text) != 8) return false;
  value = 0;
  for (unsigned i = 0; i < 8; ++i) {
    char c = text[i];
    unsigned digit = c >= '0' && c <= '9' ? unsigned(c - '0') :
                     c >= 'a' && c <= 'f' ? unsigned(c - 'a' + 10) :
                     c >= 'A' && c <= 'F' ? unsigned(c - 'A' + 10) : 16;
    if (digit > 15) return false;
    value = (value << 4) | digit;
  }
  return value != 0;
}

inline void report(uint32_t expectedNonce) {
  State& test = state();
  if (test.armed) {
    Console.printf("POWER_REPORT state=armed nonce=%08x expected=%08x expected_match=%d\n",
                  unsigned(test.record.nonce), unsigned(expectedNonce),
                  !expectedNonce || expectedNonce == test.record.nonce);
    return;
  }
  Record record{};
  bool persisted = !test.haveResult || test.saved;
  if (test.haveResult) record = test.record;
  else if (!readResult(record)) {
    Console.println("POWER_REPORT state=missing_or_invalid");
    return;
  }
  if (expectedNonce && expectedNonce != record.nonce) {
    Console.printf("POWER_REPORT state=stale expected=%08x stored=%08x\n",
                  unsigned(expectedNonce), unsigned(record.nonce));
    return;
  }
  Console.printf("POWER_REPORT state=complete nonce=%08x expected=%08x latest_only=%d\n",
                unsigned(record.nonce), unsigned(expectedNonce), !expectedNonce);
  printResult(record, persisted);
}
} // namespace device_power_detail

inline bool devicePowerCommand(const String& command) {
  using namespace device_power_detail;
  if (command == "POWER_ARM") arm();
  else if (command == "POWER_FINISH") {
    if (state().armed) finish(Explicit);
    else Console.println("POWER_FINISH armed=0");
  } else if (command == "POWER_REPORT") report(0);
  else if (command.startsWith("POWER_REPORT ")) {
    uint32_t nonce;
    if (parseNonce(command.c_str() + 13, nonce)) report(nonce);
    else Console.println("POWER_REPORT failed=expected_8_hex_nonce");
  } else return false;
  return true;
}

inline void devicePowerPoll() {
  using namespace device_power_detail;
  State& test = state();
  if (!test.armed) return;
  const IdlePowerStats stats = idlePowerStats();
  uint32_t now = millis();
  observe(now, stats);
  if (!(test.record.flags & SleepSeen)) return;
  if (stats.usbHost || (stats.sourceKnown && (stats.powerSource & 3)))
    finish(ExternalPower);
  else if (uint32_t(now - test.firstSleepAt) >= OBSERVATION_MS)
    finish(Timeout);
}

inline void devicePowerAction(controls::Action action) {
  using namespace device_power_detail;
  if (!state().armed) return;
  Record& record = state().record;
  switch (action) {
    case controls::Action::Previous:
    case controls::Action::PreviousScreen: ++record.aActions; break;
    case controls::Action::Next:
    case controls::Action::NextScreen: ++record.bActions; break;
    case controls::Action::Confirm: ++record.confirmActions; break;
    case controls::Action::Shutdown: ++record.shutdownActions; break;
    default: break;
  }
}

inline void devicePowerFinish() {
  device_power_detail::finish(device_power_detail::Explicit);
}
#endif // PAPERDIY_DEVICE_TESTS
