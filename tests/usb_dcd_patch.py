#!/usr/bin/env python3
"""Offline checks of the exact DCD patch, races and archive substitution."""
import argparse
import importlib.util
import itertools
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("patch_usb_dcd", ROOT / "tools/patch_usb_dcd.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
ORIGINAL = ROOT / "tools/vendor/arduino-esp32-2.0.17/dcd_esp32sx.c"
FRAMEWORK = AR = None


class PatchTests(unittest.TestCase):
    def test_exact_source_and_fail_closed(self):
        source = ORIGINAL.read_bytes()
        changed = patch.patched_source(source)
        self.assertEqual(patch.sha256(source), patch.SOURCE_SHA256)
        self.assertEqual(changed.count(b"USB0.dtknqr4_fifoemptymsk"), 3)
        self.assertIn(b"paperdiy_txfe_clear_task(0x7Fu);", changed)
        self.assertIn(b"paperdiy_txfe_set_task(1u << epnum);", changed)
        self.assertEqual(changed.count(b"paperdiy_txfe_clear_isr(1u << n);"), 2)
        for invalid in (b"", source + b"\n", source.replace(b"(1 << epnum)", b"(1u << epnum)")):
            with self.assertRaises(ValueError):
                patch.patched_source(invalid)
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "patched.c"
            patch.write_copy(ORIGINAL, output)
            stamp = output.stat().st_mtime_ns
            patch.write_copy(ORIGINAL, output)
            self.assertEqual(output.stat().st_mtime_ns, stamp)
            self.assertEqual(output.read_bytes(), changed)
            with self.assertRaises(ValueError):
                patch.write_copy(ORIGINAL, ORIGINAL)

    def test_lost_mask_update_interleavings(self):
        # ISR retires EP1 while the application arms EP4. Each volatile RMW
        # consists of a read and a write; cross-core local IRQ disable is not a lock.
        results = []
        for order in itertools.permutations(("ir", "iw", "tr", "tw")):
            if order.index("ir") > order.index("iw") or order.index("tr") > order.index("tw"):
                continue
            mask, saved = 2, {}
            for step in order:
                if step[1] == "r":
                    saved[step[0]] = mask
                else:
                    mask = saved[step[0]] & ~2 if step[0] == "i" else saved[step[0]] | 16
            results.append(mask)
        self.assertIn(0, results, "baseline must expose lost EP4 enable")
        self.assertIn(18, results, "baseline must expose resurrected EP1 enable")
        # Shared lock makes each RMW indivisible, for either scheduling order.
        for order in (("i", "t"), ("t", "i")):
            mask = 2
            for operation in order:
                mask = mask & ~2 if operation == "i" else mask | 16
            self.assertEqual(mask, 16)
        # RMW locking alone cannot save a new same-endpoint transfer if the old
        # completion is published before retirement.
        mask = 16
        mask |= 16  # completion consumer arms the next EP4 transfer
        mask &= ~16  # old ISR retirement, even under a lock
        self.assertEqual(mask, 0)

    def test_compiled_production_helpers_and_completion(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler)
        prefix = r'''
#include <cassert>
#include <cstdint>
#define portMUX_TYPE int
#define portMUX_INITIALIZER_UNLOCKED 0
bool locked=false, isr=false; unsigned enters=0;
void enter(bool expected){assert(isr==expected);assert(!locked);locked=true;++enters;}
void leave(bool expected){assert(isr==expected);assert(locked);locked=false;}
#define portENTER_CRITICAL(p) enter(false)
#define portEXIT_CRITICAL(p) leave(false)
#define portENTER_CRITICAL_ISR(p) enter(true)
#define portEXIT_CRITICAL_ISR(p) leave(true)
#define ESP_EARLY_LOGV(...) ((void)0)
#define ESP_EARLY_LOGE(...) ((void)0)
constexpr unsigned USB_D_XFERCOMPL0_M=1, USB_D_TIMEOUT0_M=8, USB_D_TXFEMP0_M=128;
constexpr unsigned TUSB_DIR_IN_MASK=128, XFER_RESULT_SUCCESS=0;
// TXFEMP is read-only: writing its bit cannot clear it.
struct Flags {unsigned value=0; operator unsigned()const{return value;}
 Flags& operator=(unsigned bits){value &= ~(bits & ~USB_D_TXFEMP0_M);return *this;}};
struct Endpoint {Flags diepint;};
struct Device {unsigned dtknqr4_fifoemptymsk=0;Endpoint in_ep_reg[1];} USB0;
struct Transfer {uint16_t total_len=64,queued_len=64;} transfer;
bool published=false;unsigned transmitCalls=0;
void dcd_event_xfer_complete(int,unsigned,uint16_t length,int,bool){
 assert(!locked);assert(isr);assert(length==64);
 assert(!(USB0.dtknqr4_fifoemptymsk&1)); // retire BEFORE publication
 assert(!(unsigned(USB0.in_ep_reg[0].diepint)&(USB_D_TIMEOUT0_M|USB_D_XFERCOMPL0_M)));
 published=true;
 // Model another core consuming completion immediately and arming EP0 again.
 isr=false;
'''
        event_end = r'''
 isr=true;transfer.total_len=32;transfer.queued_len=0;
}
void transmit_packet(Transfer*,Endpoint*,unsigned){++transmitCalls;assert(!published);}
void complete(){for(unsigned n=0;n<1;++n){Transfer* xfer=&transfer;
'''
        # Keep the old FIFO branch too: TXFE remains asserted after W1C writes.
        fifo = r'''
 if(USB0.in_ep_reg[n].diepint&USB_D_TXFEMP0_M){
  USB0.in_ep_reg[n].diepint=USB_D_TXFEMP0_M;
  transmit_packet(xfer,&USB0.in_ep_reg[n],n);
  if(xfer->queued_len==xfer->total_len)paperdiy_txfe_clear_isr(1u<<n);
 }
}}
int main(){
 USB0.dtknqr4_fifoemptymsk=127;paperdiy_txfe_clear_task(127);assert(!USB0.dtknqr4_fifoemptymsk);
 paperdiy_txfe_set_task(1);assert(USB0.dtknqr4_fifoemptymsk==1);
 USB0.in_ep_reg[0].diepint.value=USB_D_XFERCOMPL0_M|USB_D_TXFEMP0_M|USB_D_TIMEOUT0_M;
 isr=true;complete();assert(published&&!locked&&enters==4);
 assert(USB0.dtknqr4_fifoemptymsk==1);assert(transmitCalls==0);
 assert(unsigned(USB0.in_ep_reg[0].diepint)==USB_D_TXFEMP0_M);
}
'''
        # Declare helpers before the event hook; put their definitions after globals.
        declaration = "void paperdiy_txfe_set_task(uint32_t);\n"
        helper = patch.HELPERS.decode().replace("static void paperdiy_txfe_set_task", "void paperdiy_txfe_set_task")
        for name, completion in (("before", patch.COMPLETION), ("after", patch.RETIRED_COMPLETION)):
            with tempfile.TemporaryDirectory() as temporary:
                source = Path(temporary) / "race.cpp"
                # Helpers precede the event function, avoiding any mock replacement
                # of the actual production locking/retirement statements.
                split = prefix.index("void dcd_event_xfer_complete")
                source.write_text(prefix[:split] + helper + prefix[split:] +
                                  "paperdiy_txfe_set_task(1);\n" + event_end + completion.decode() + fifo)
                binary = Path(temporary) / "race"
                subprocess.run([compiler, "-std=c++11", "-fsanitize=address,undefined", str(source), "-o", str(binary)], check=True)
                result = subprocess.run([str(binary)], capture_output=True)
                if name == "before":
                    self.assertNotEqual(result.returncode, 0, "baseline must expose premature publication")
                else:
                    self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_pinned_archive_substitution(self):
        if not FRAMEWORK or not AR:
            self.skipTest("pass --framework and --ar for installed archive checks")
        original = Path(FRAMEWORK) / "tools/sdk/esp32s3/lib/libarduino_tinyusb.a"
        before = original.read_bytes()
        with tempfile.TemporaryDirectory() as temporary:
            folder = Path(temporary)
            replacement = folder / "paperdiy_dcd.o"
            # An ar member is opaque; the independent SDK build checks its code.
            replacement.write_bytes(b"offline replacement member")
            output = folder / "patched.a"
            patch.replace_archive(original, replacement, output, AR)
            members = subprocess.check_output([AR, "t", str(output)]).decode().splitlines()
            self.assertNotIn(patch.MEMBER, members)
            self.assertEqual(members.count(replacement.name), 1)
            self.assertEqual(subprocess.check_output([AR, "p", str(output), replacement.name]), replacement.read_bytes())
            with self.assertRaises(ValueError):
                patch.replace_archive(original, replacement, original, AR)
            wrong = folder / "wrong.a"
            wrong.write_bytes(before+b"\n")
            with self.assertRaises(ValueError):
                patch.replace_archive(wrong, replacement, output, AR)
        self.assertEqual(original.read_bytes(), before)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--framework")
    parser.add_argument("--ar")
    args, remaining = parser.parse_known_args()
    FRAMEWORK, AR = args.framework, args.ar
    unittest.main(argv=[__file__]+remaining)
