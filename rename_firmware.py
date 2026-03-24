"""
PlatformIO pre-script: renames firmware.bin to firmware-<version>.bin
so the web flasher cache doesn't serve stale files.
Version is read from FW_VERSION in include/config.h.
"""
import re, os

Import("env")

def get_version():
    config = os.path.join(env.subst("$PROJECT_DIR"), "include", "config.h")
    try:
        with open(config) as f:
            m = re.search(r'#define\s+FW_VERSION\s+"([^"]+)"', f.read())
            if m:
                return m.group(1)
    except Exception:
        pass
    return "unknown"

version = get_version()
build_dir = env.subst("$BUILD_DIR")

def rename_after_build(source, target, env):
    src = os.path.join(build_dir, "firmware.bin")
    dst = os.path.join(build_dir, f"firmware-{version}.bin")
    if os.path.exists(src):
        import shutil
        shutil.copy2(src, dst)
        print(f"[rename] firmware-{version}.bin ready in {build_dir}")

env.AddPostAction("$BUILD_DIR/firmware.bin", rename_after_build)
