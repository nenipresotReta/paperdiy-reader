"""PRE PlatformIO middleware: replace only the reviewed SD implementation TU."""
from pathlib import Path
import importlib.util

Import("env")  # supplied by SCons

script = Path(env.subst("$PROJECT_DIR")) / "tools" / "patch_sd_uninit.py"
spec = importlib.util.spec_from_file_location("paperdiy_sd_uninit_patch", script)
patch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(patch)
framework = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
if not framework:
    raise RuntimeError("PaperDIY SD patch requires the Arduino-ESP32 framework")
original = Path(framework) / "libraries" / "SD" / "src" / "sd_diskio.cpp"
output = Path(env.subst("$BUILD_DIR")) / "patched-sdk" / "sd_diskio.cpp"
# Validate on every configure, including incremental builds. Never accept an
# unknown source or silently fall back to the known unsafe upstream function.
digest = patch.write_copy(original, output)
print("PaperDIY SD lifetime patch: source=" + patch.SOURCE_SHA256 + " patched=" + digest)
replacements = {"count": 0}


def replace_sd_source(build_env, node):
    if Path(node.srcnode().get_abspath()).resolve() != original.resolve():
        return node
    replacements["count"] += 1
    obj = build_env.Object(
        target=str(output) + ".o", source=str(output),
        CPPPATH=build_env.get("CPPPATH", []) + [str(original.parent)],
    )[0]
    build_env.Depends(obj, [str(original), str(script), str(script.with_name("pio_sd_uninit.py"))])
    return obj


env.AddBuildMiddleware(replace_sd_source)


def require_replacement(target, source, env):
    if replacements["count"] != 1:
        raise RuntimeError("PaperDIY SD lifetime patch did not replace exactly one framework translation unit")


env.AddPreAction("$BUILD_DIR/${PROGNAME}.elf", require_replacement)
