# PlatformIO post-build script:
#  1. Fails the build if the app image does not fit into the OTA app partition.
#     The size shown by PlatformIO does not include the image header and padding,
#     so the size of the final .bin file is checked instead.
#  2. Creates <build_dir>/firmware-factory.bin (bootloader + partition table +
#     boot_app0 + app) that can be flashed to address 0x0 on a blank board.
Import("env")  # noqa: F821  (provided by PlatformIO)

import csv
import os
import subprocess


def _parse_size(value):
    value = value.strip().upper()
    if value.endswith("K"):
        return int(value[:-1], 0) * 1024
    if value.endswith("M"):
        return int(value[:-1], 0) * 1024 * 1024
    return int(value, 0)


def _app_partition_size(env):
    table = env.subst("$PARTITIONS_TABLE_CSV")
    with open(table, newline="") as f:
        for row in csv.reader(f):
            if not row or row[0].strip().startswith("#") or len(row) < 5:
                continue
            if row[1].strip() == "app":
                return _parse_size(row[4])
    raise RuntimeError("no app partition found in " + table)


def post_build(source, target, env):
    app_bin = target[0].get_abspath()
    build_dir = os.path.dirname(app_bin)

    app_size = os.path.getsize(app_bin)
    part_size = _app_partition_size(env)
    print("App image: %d bytes, app partition: %d bytes (%.1f %% used, %d bytes free)"
          % (app_size, part_size, 100.0 * app_size / part_size, part_size - app_size))
    if app_size > part_size:
        env.Exit("ERROR: app image does not fit into the app partition")

    chip = env.BoardConfig().get("build.mcu")
    images = []
    for offset, image in env.get("FLASH_EXTRA_IMAGES", []):
        images += [env.subst(offset), env.subst(image)]
    images += [env.subst("$ESP32_APP_OFFSET"), app_bin]

    out = os.path.join(build_dir, "firmware-factory.bin")
    cmd = [env.subst("$OBJCOPY"), "--chip", chip, "merge-bin", "-o", out] + images
    print("Creating " + out)
    subprocess.check_call(cmd)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", post_build)  # noqa: F821
