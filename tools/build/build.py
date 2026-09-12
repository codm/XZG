#!/usr/bin/env python3

Import("env")

import subprocess
from subprocess import call
import shutil
import os
import time
from glob import glob
import sys
import re

sys.path.append("./tools")
from func import print_logo
from func import print_colored

VERSION_HEADER = "version.h"


def get_last_git_tag():
    command = ['git', 'describe', '--exact-match', '--tags']
    try:
        result = subprocess.run(command, capture_output=True, text=True)
    except FileNotFoundError:
        result = None
    if result is not None and result.returncode == 0:
        return result.stdout.strip().removeprefix('V')
    # Feature branches have no release tag; use the existing source version.
    with open('src/version.h', encoding='utf-8') as source:
        version = re.search(r'#define\s+VERSION\s+"([^"]+)"', source.read())
    if not version:
        raise RuntimeError('Missing firmware version')
    return version.group(1).removeprefix('V')

def after_build(source, target, env):
    time.sleep(2)
    shutil.copy(firmware_source, "bin/firmware.bin")
    for f in glob("bin/XZG*.bin"):
        os.unlink(f)

    exit_code = call(
        "python tools/build/merge_bin_esp.py --output_folder ./bin --output_name XZG.full.bin --bin_path bin/bootloader_dio_40m.bin bin/firmware.bin bin/partitions.bin --bin_address 0x1000 0x10000 0x8000",
        shell=True,
    )
    if exit_code:
        raise RuntimeError('Firmware image merge failed')

    VERSION_FILE = "src/" + VERSION_HEADER
    
    VERSION_NUMBER = get_last_git_tag()

    NEW_NAME_BASE = "bin/czc_fw_" + VERSION_NUMBER
    build_env = env['PIOENV']
    if "debug" in build_env:
        NEW_NAME_BASE += "_" + build_env
    
    NEW_NAME_FULL = NEW_NAME_BASE + ".full.bin"
    NEW_NAME_OTA = NEW_NAME_BASE + ".ota.bin"

    shutil.move("bin/XZG.full.bin", NEW_NAME_FULL)
    shutil.move("bin/firmware.bin", NEW_NAME_OTA)

    print("")
    print_colored("--------------------------------------", "yellow")
    print_colored("{} created !".format(str(NEW_NAME_FULL)), "blue")
    print_colored("{} created !".format(str(NEW_NAME_OTA)), "magenta")
    print_colored("--------------------------------------", "yellow")
    print_logo()
    print_colored("Build " + VERSION_NUMBER, "cyan")
    print("")

env.AddPostAction("buildprog", after_build)

firmware_source = os.path.join(env.subst("$BUILD_DIR"), "firmware.bin")
