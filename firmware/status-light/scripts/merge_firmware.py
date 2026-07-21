# PlatformIO post-build hook: merge bootloader + partition table + boot_app0 +
# app into ONE image flashable at offset 0x0, and drop it into
# ../../public/firmware/status-light.bin so the web flasher (and the 3D-FarmLab
# dashboard flasher) can serve a ready-to-flash image. ESP32-C3 layout:
# bootloader @0x0, partitions @0x8000, boot_app0 @0xe000, app @0x10000.
#
# public/firmware/manifest.json (esp-web-tools format) already points at
# status-light.bin @ offset 0, so this script only writes the binary and leaves
# the manifest untouched.
import os

Import("env")  # noqa: F821 (PlatformIO SCons construct)

OUTPUT_NAME = "status-light.bin"


def merge_firmware(source, target, env):
    build_dir = env.subst("$BUILD_DIR")
    project_dir = env.subst("$PROJECT_DIR")
    out_dir = os.path.abspath(os.path.join(project_dir, "..", "..", "public", "firmware"))
    os.makedirs(out_dir, exist_ok=True)
    out_path = os.path.join(out_dir, OUTPUT_NAME)

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    app = os.path.join(build_dir, env.subst("${PROGNAME}.bin"))
    boot_app0 = os.path.join(
        env.PioPlatform().get_package_dir("framework-arduinoespressif32"),
        "tools", "partitions", "boot_app0.bin",
    )

    cmd = (
        '"$PYTHONEXE" "$OBJCOPY" --chip esp32c3 merge_bin -o "{out}" '
        "--flash_mode dio --flash_freq 80m --flash_size 4MB "
        '0x0 "{bootloader}" 0x8000 "{partitions}" 0xe000 "{boot_app0}" 0x10000 "{app}"'
    ).format(out=out_path, bootloader=bootloader, partitions=partitions,
             boot_app0=boot_app0, app=app)
    if env.Execute(cmd):
        raise SystemExit("esptool merge_bin failed")

    print("Merged firmware written to %s" % out_path)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", merge_firmware)  # noqa: F821
