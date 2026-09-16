#!/usr/bin/env python3
"""Where this machine keeps the emulator and the licensed blobs.

Every default below is one particular Windows box, because that is the
machine the tree grew up on.  None of it belongs in a clone: the ROMs
and card images are ROOL's to distribute, the farm is machine state,
and the QEMU build is wherever it was configured.  So every path moves
with an environment variable of its own -- RISCOS_FARM, RISCOS_QEMU,
RISCOS_KERNEL, and so on -- and unset, every path is exactly what it
was.  The launcher shells read the same names directly.

    BUILD        the QEMU build directory; QEMU and QEMU_IMG default
                 inside it, so RISCOS_BUILD alone moves the emulator
    KERNEL       the stock RISC OS ROM image
    CMOS         the CMOS blob the stock image boots with
    BASE_IMAGE   the DDE card image the farm clones its discs from
    IMAGE        the stock card image a plain launch boots
    FS_ZIP       the end-user filing system, for the release
    FARM         the farm directory: one subdirectory per machine
    MINGW_BIN    the MinGW runtime, for PATH beside the emulator
    BASH         the MSYS2 bash that lists the emulator's DLLs;
                 defaults beside MINGW_BIN, in usr/bin
"""

import os

BUILD = os.environ.get("RISCOS_BUILD", r"F:\RISCOSDEV\qemu\build")
QEMU = os.environ.get(
    "RISCOS_QEMU", os.path.join(BUILD, "qemu-system-aarch64.exe"))
QEMU_IMG = os.environ.get(
    "RISCOS_QEMU_IMG", os.path.join(BUILD, "qemu-img.exe"))
KERNEL = os.environ.get("RISCOS_KERNEL", r"F:\RISCOSDEV\roms\pi\v530\RISCOS.IMG")
CMOS = os.environ.get("RISCOS_CMOS",
                      r"F:\RISCOSDEV\roms\pi\rool-cmos-unplug.bin")
BASE_IMAGE = os.environ.get("RISCOS_BASE_IMAGE",
                            r"F:\RISCOSDEV\roms\sdimg\dde_dev.img")
IMAGE = os.environ.get("RISCOS_IMAGE",
                       r"F:\RISCOSDEV\roms\sdimg\ro530-1875M.img")
FS_ZIP = os.environ.get("RISCOS_FS_ZIP",
                        r"F:\RISCOSDEV\ROS_PRIVATE\dist\end_user_fs.zip")
FARM = os.environ.get("RISCOS_FARM", r"F:\RISCOSDEV\qemu-farm")
MINGW_BIN = os.environ.get("RISCOS_MINGW_BIN",
                           r"F:\RISCOSDEV\msys64\mingw64\bin")
BASH = os.environ.get(
    "RISCOS_BASH",
    os.path.join(os.path.dirname(os.path.dirname(MINGW_BIN)),
                 "usr", "bin", "bash.exe"))
