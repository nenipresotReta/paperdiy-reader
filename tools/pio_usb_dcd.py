"""POST PlatformIO hook: substitute one checked, build-local USB SDK archive."""
from pathlib import Path
import importlib.util

Import("env")  # supplied by SCons, after framework flags/libraries are populated

project = Path(env.subst("$PROJECT_DIR"))
script = project / "tools" / "patch_usb_dcd.py"
spec = importlib.util.spec_from_file_location("paperdiy_usb_dcd_patch", script)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
if env.BoardConfig().get("build.mcu") != "esp32s3":
    raise RuntimeError("PaperDIY USB DCD patch only supports the reviewed ESP32-S3 SDK")
framework = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
if not framework:
    raise RuntimeError("PaperDIY USB DCD patch requires Arduino-ESP32")
original = Path(framework) / "tools/sdk/esp32s3/lib/libarduino_tinyusb.a"
# Check every configure, even when the local archive is already up to date.
if patch.sha256(original.read_bytes()) != patch.ARCHIVE_SHA256:
    raise RuntimeError("unsupported USB DCD SDK archive checksum")
source = project / "tools/vendor/arduino-esp32-2.0.17/dcd_esp32sx.c"
folder = Path(env.subst("$BUILD_DIR")) / "patched-usb"
output = folder / "dcd_esp32sx.c"
digest = patch.write_copy(source, output)
print("PaperDIY USB DCD patch: source=" + patch.SOURCE_SHA256 + " patched=" + digest)
obj = env.Object(target=str(folder / "paperdiy_dcd.o"), source=str(output))[0]
middleware = project / "tools" / "pio_usb_dcd.py"
env.Depends(obj, [str(source), str(script), str(middleware)])


def build_archive(target, source, env):
    ar = env.WhereIs(env.subst("$AR"))
    if not ar:
        raise RuntimeError("USB DCD patch requires the configured SDK archiver")
    patch.replace_archive(original, str(obj), str(target[0]), ar, env.get("ENV"))


archive = env.Command(str(folder / "libarduino_tinyusb_paperdiy.a"),
                      [str(original), obj, str(script), str(middleware)], build_archive)[0]
libraries = list(env.get("LIBS", []))
indices = [i for i, value in enumerate(libraries)
           if str(value) in ("-larduino_tinyusb", "arduino_tinyusb")]
if len(indices) != 1:
    raise RuntimeError("USB DCD patch must substitute exactly one SDK library")
libraries[indices[0]] = archive
env.Replace(LIBS=libraries)
env.Depends("$BUILD_DIR/${PROGNAME}.elf", archive)
