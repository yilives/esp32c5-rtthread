#!/usr/bin/env bash
# Build / flash the S31 RT-Thread project through the IDF environment.
#
# Two things this script works around, both discovered by trying to call idf.py
# from a one-liner:
#   1. Git Bash exports MSYSTEM, and IDF v6.1 refuses to run when it is set.
#      The env script clears it for interactive shells; here it is unset again
#      because `bash -c` inherits the parent's environment afresh.
#   2. `idf.py` is not executable through the exported PATH on this machine -
#      which resolves to the Windows form `I:/...` and bash silently fails the
#      lookup. Invoking `python $IDF_PATH/tools/idf.py` avoids the lookup.
#
# Usage: build.sh [extra idf.py arguments]
set -uo pipefail

cd "$(dirname "$0")/.." || exit 1
source ./idf_s31_env.sh
unset MSYSTEM

echo "IDF_PATH=$IDF_PATH"
echo "gcc=$(riscv32-esp-elf-gcc --version | head -1)"
echo "python=$(python --version 2>&1)"

cd s31-rtt-test || exit 1
exec python "$IDF_PATH/tools/idf.py" -DIDF_TARGET=esp32s31 "$@"
