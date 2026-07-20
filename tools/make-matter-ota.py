#!/usr/bin/env python3
#
# Build a Matter OTA image (.ota) from the project's build output.
#
# Point it at the project root (defaults to the current directory):
#
#     python3 tools/make-matter-ota.py
#     python3 tools/make-matter-ota.py /path/to/shelly_gen4_matter_module
#
# Every field that the device matches against is read from the build:
#
#   - vendor / product ID from sdkconfig (CONFIG_DEVICE_VENDOR_ID /
#     CONFIG_DEVICE_PRODUCT_ID).
#   - software version (number + string) from main/CHIPProjectConfig.h
#     (CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION[_STRING]).
#
# It wraps build/<project>.bin with the upstream Matter ota_image_tool.py
# and writes shelly-gen4-matter-module-v<version>.ota next to the build.
#
# Run it in the same esp-matter environment you build in. ota_image_tool.py
# is located in the connectedhomeip checkout via $ESP_MATTER_PATH (or set
# $OTA_IMAGE_TOOL to its path directly).
#
import json, os, re, subprocess, sys


def sdkconfig_int(sdkconfig, key):
    m = re.search(rf"^{re.escape(key)}=(.+)$", open(sdkconfig).read(), re.M)
    if not m:
        sys.exit(f"{key} not set in {sdkconfig} -- run `idf.py build` first")
    return int(m.group(1).strip(), 0)


def define(header, key):
    m = re.search(rf"^#define\s+{re.escape(key)}\s+(.+)$", open(header).read(), re.M)
    if not m:
        sys.exit(f"{key} not found in {header}")
    return m.group(1).strip()


def find_ota_image_tool():
    override = os.environ.get("OTA_IMAGE_TOOL")
    if override and os.path.isfile(override):
        return override
    esp_matter = os.environ.get("ESP_MATTER_PATH")
    if esp_matter:
        cand = os.path.join(esp_matter, "connectedhomeip", "connectedhomeip",
                            "src", "app", "ota_image_tool.py")
        if os.path.isfile(cand):
            return cand
    sys.exit("could not find ota_image_tool.py -- set ESP_MATTER_PATH (or OTA_IMAGE_TOOL) "
             "to the esp-matter checkout you build with")


def main():
    project_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    build = os.path.join(project_dir, "build")
    desc = os.path.join(build, "project_description.json")
    if not os.path.isfile(desc):
        sys.exit(f"no build found at {build} -- run `idf.py build` in {project_dir} first")
    project_name = json.load(open(desc))["project_name"]

    sdkconfig = os.path.join(project_dir, "sdkconfig")
    header = os.path.join(project_dir, "main", "CHIPProjectConfig.h")
    app_bin = os.path.join(build, f"{project_name}.bin")
    for p in (sdkconfig, header, app_bin):
        if not os.path.isfile(p):
            sys.exit(f"missing build output: {p}")

    vid = sdkconfig_int(sdkconfig, "CONFIG_DEVICE_VENDOR_ID")
    pid = sdkconfig_int(sdkconfig, "CONFIG_DEVICE_PRODUCT_ID")
    version = int(define(header, "CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION").split()[0], 0)
    version_str = define(header, "CHIP_DEVICE_CONFIG_DEVICE_SOFTWARE_VERSION_STRING").split('"')[1]

    out = os.path.join(project_dir, f"shelly-gen4-matter-module-v{version_str}.ota")

    cmd = [sys.executable, find_ota_image_tool(), "create",
           "-v", f"0x{vid:04X}", "-p", f"0x{pid:04X}",
           "-vn", str(version), "-vs", version_str,
           "-da", "sha256", app_bin, out]
    subprocess.run(cmd, check=True)

    print(f"Created {os.path.basename(out)} ({os.path.getsize(out) / 1024 / 1024:.1f} MB)")
    print(f"  VID 0x{vid:04X}  PID 0x{pid:04X}  version {version} ({version_str})")


if __name__ == "__main__":
    main()
