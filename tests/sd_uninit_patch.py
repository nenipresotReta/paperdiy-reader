#!/usr/bin/env python3
"""Offline production-patch checks; optional --framework checks the pinned TU."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("patch_sd_uninit", ROOT / "tools/patch_sd_uninit.py")
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)

class PatchTests(unittest.TestCase):
    def test_checksum_and_shape_fail_closed(self):
        for source in (b"", patch.FUNCTION, b"x" + patch.FUNCTION):
            with self.assertRaises(ValueError):
                patch.patched_source(source)
        for source in (b"", patch.FUNCTION + patch.FUNCTION, patch.FUNCTION.replace(b"free(card);", b"free(card); ")):
            with self.assertRaises(ValueError):
                patch.rewrite_verified_shape(source)
        self.assertEqual(patch.rewrite_verified_shape(patch.FUNCTION), patch.REPLACEMENT)

    def test_compiled_lifetime_and_invalid_drive(self):
        compiler = shutil.which("clang++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "C++ compiler required")
        prefix = r'''
#include <cassert>
#include <cstdlib>
#include <cstring>
using uint8_t=unsigned char;
using esp_err_t=int;
constexpr int FF_VOLUMES=2,ESP_OK=0,GO_IDLE_STATE=0;
struct FakeSPI {bool locked=false;void beginTransaction(){assert(!locked);locked=true;}void endTransaction(){assert(locked);locked=false;}} spi;
struct ardu_sdcard_t {FakeSPI* spi;char* base_path;};
ardu_sdcard_t* s_cards[FF_VOLUMES]={};
bool registered=true;int unregisterResult=0;unsigned freed=0;
struct AcquireSPI {ardu_sdcard_t* card;AcquireSPI(ardu_sdcard_t* c):card(c){card->spi->beginTransaction();}~AcquireSPI(){card->spi->endTransaction();}};
void sdTransaction(uint8_t,int,int,void*){assert(spi.locked);}
void ff_diskio_register(uint8_t,void*){assert(spi.locked);registered=false;}
int esp_vfs_fat_unregister_path(char*){assert(spi.locked);return unregisterResult;}
// This boundary can prove ordering without depending on allocator reuse:
void checkedCardFree(ardu_sdcard_t* p){assert(!spi.locked);std::free(p);++freed;}
void checkedFree(char* p){std::free(p);++freed;}
void checkedFree(ardu_sdcard_t* p){checkedCardFree(p);}
#define free(p) checkedFree(p)
'''
        suffix = r'''
#undef free
int main(){
 for(int failing=0;failing<2;++failing){
  auto* c=static_cast<ardu_sdcard_t*>(std::malloc(sizeof(ardu_sdcard_t)));
  c->spi=&spi;c->base_path=failing?static_cast<char*>(std::malloc(8)):nullptr;s_cards[0]=c;
  unregisterResult=failing?19:0;registered=true;
  assert(sdcard_uninit(0)==unregisterResult);assert(!spi.locked&&!registered&&!s_cards[0]);
 }
 assert(freed==3);assert(sdcard_uninit(1)==1);assert(sdcard_uninit(255)==1);
}
'''
        with tempfile.TemporaryDirectory(prefix="paperdiy-sd-patch-") as temporary:
            folder = Path(temporary)
            for name, function in (("before", patch.FUNCTION), ("after", patch.REPLACEMENT)):
                source = folder / (name + ".cpp")
                source.write_text(prefix + function.decode() + suffix)
                binary = folder / name
                subprocess.run([compiler, "-std=c++11", "-fno-exceptions", "-fsanitize=address,undefined", "-g", str(source), "-o", str(binary)], check=True)
                result = subprocess.run([str(binary)], capture_output=True)
                if name == "before":
                    self.assertNotEqual(result.returncode, 0, "baseline must expose the lifetime bug")
                else:
                    self.assertEqual(result.returncode, 0, result.stderr.decode())

    def test_installed_framework(self):
        if not FRAMEWORK:
            self.skipTest("pass --framework to verify the installed pinned SDK too")
        original = Path(FRAMEWORK) / "libraries/SD/src/sd_diskio.cpp"
        before = original.read_bytes()
        self.assertEqual(hashlib.sha256(before).hexdigest(), patch.SOURCE_SHA256)
        with tempfile.TemporaryDirectory(prefix="paperdiy-sd-copy-") as temporary:
            output = Path(temporary) / "sd_diskio.cpp"
            digest = patch.write_copy(original, output)
            self.assertEqual(output.read_bytes(), before.replace(patch.FUNCTION, patch.REPLACEMENT))
            self.assertEqual(hashlib.sha256(output.read_bytes()).hexdigest(), digest)
            stamp = output.stat().st_mtime_ns
            patch.write_copy(original, output)
            self.assertEqual(output.stat().st_mtime_ns, stamp)
            with self.assertRaises(ValueError):
                patch.write_copy(original, original)
        self.assertEqual(original.read_bytes(), before)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--framework")
    args, remaining = parser.parse_known_args()
    FRAMEWORK = args.framework
    unittest.main(argv=[__file__] + remaining)
