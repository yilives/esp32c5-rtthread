#!/usr/bin/env python3
"""Find references to Xtensa-only symbols that are not behind an architecture guard.

Why this exists
---------------

Nine times in this port, an actively-developed debugging probe has broken the
ESP32-C5 build. The pattern is always the same:

  * someone adds `extern volatile unsigned int rt_xt_xxx[8];` and a few writes to
    shared code (components/rtthread/, components/freertos/, components/esp_libc/)
  * the symbol is defined only in libcpu/xtensa/cpuport.c, or in the S3 BSP
  * the C5 is RISC-V, so `riscv32-esp-elf-ld` reports undefined references
  * nothing fails until LINK time, several minutes into a build

Each occurrence cost a build cycle to find and a `#if defined(__XTENSA__)` guard
to fix. This script turns "the link told us" into "the source says so in one
second", so the guard can be added before the first build.

Usage
-----

    python tools/check_xtensa_guards.py            # C5 target, default paths
    python tools/check_xtensa_guards.py --root I:/RT_thread/esp-idf-rtt

Exit code 0 when clean, 1 when an unguarded reference is found. That makes it
usable as a pre-build gate.

What counts as a reference
--------------------------

A line mentioning a known Xtensa-only symbol, outside any `#if defined(__XTENSA__)`
or `#ifdef __XTENSA__` region. Declarations in libcpu/xtensa/ and the S3 BSP are
fine and are skipped: those files only ever build for Xtensa.

The guard tracking is deliberately simple - it counts nesting of `#if`/`#ifdef`/
`#ifndef` and remembers whether any enclosing level mentioned __XTENSA__. That is
enough for the shapes actually used here and avoids pulling in a C preprocessor.
"""

import argparse
import os
import re
import sys

# Symbols that exist only for Xtensa. Extended as new ones turn up; that list
# getting longer is itself the signal that this check is worth having.
XTENSA_SYMBOLS = [
    "rt_xt_br",
    "rt_xt_br_task",
    "rt_xt_sst",
    "rt_xt_lk",
    "rt_xt_lk2",
    "rt_xt_nt",
    "rt_xt_nf",
    "rt_xt_ii",
    "rt_xt_ip3",
    "rt_xt_pk",
    "rt_xt_running_count",
            "xt_rt_flash_op",
    "xt_rt_flash_ack",
]

# Files and directories where an Xtensa-only symbol is legitimate.
SKIP_PATH_PARTS = [
    os.sep + "libcpu" + os.sep + "xtensa",
    os.sep + "bsp" + os.sep + "ESP" + os.sep + "ESP32_S3",
    os.sep + "FreeRTOS-Kernel-SMP" + os.sep + "portable" + os.sep + "xtensa",
    os.sep + "FreeRTOS-Kernel" + os.sep + "portable" + os.sep + "xtensa",
]

SRC_EXT = (".c", ".h", ".cpp", ".hpp", ".cc")

SYMBOL_RE = re.compile(r"\b(" + "|".join(re.escape(s) for s in XTENSA_SYMBOLS) + r")\b")
GUARD_OPEN_RE = re.compile(r"^\s*#\s*(if|ifdef|ifndef)\b(.*)$")
GUARD_ELSE_RE = re.compile(r"^\s*#\s*(else|elif)\b(.*)$")
GUARD_END_RE = re.compile(r"^\s*#\s*endif\b")


def is_xtensa_condition(text):
    return "__XTENSA__" in text or "CONFIG_IDF_TARGET_ARCH_XTENSA" in text


def scan_file(path):
    """Return a list of (lineno, symbol, line) for unguarded references."""
    hits = []

    # stack of booleans: "was this level's condition an __XTENSA__ test?"
    guard_stack = []
    # stack of booleans: "is this level currently in an Xtensa-only branch?"
    active_stack = []

    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.readlines()
    except OSError:
        return hits

    for lineno, line in enumerate(lines, 1):
        m = GUARD_OPEN_RE.match(line)
        if m:
            xt = is_xtensa_condition(m.group(2))
            guard_stack.append(xt)
            active_stack.append(xt)
            continue

        if GUARD_ELSE_RE.match(line):
            if guard_stack:
                guard_stack[-1] = guard_stack[-1] or is_xtensa_condition(
                    GUARD_ELSE_RE.match(line).group(2))
                active_stack[-1] = guard_stack[-1]
            continue

        if GUARD_END_RE.match(line):
            if guard_stack:
                guard_stack.pop()
                active_stack.pop()
            continue

        if any(active_stack):
            continue

        # Ignore commentaries: a symbol named in prose is documentation, not a
        # reference, and three of the reports this tool was written to catch are
        # now explanatory comments that mention the very symbol they explain.
        stripped = line.lstrip()
        if stripped.startswith(("/*", "*", "//")):
            continue

        s = SYMBOL_RE.search(line)
        if s:
            hits.append((lineno, s.group(1), line.rstrip()))

    return hits


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=r"I:\RT_thread\esp-idf-rtt",
                    help="IDF tree to scan (default: %(default)s)")
    ap.add_argument("--quiet", action="store_true",
                    help="only print the summary line")
    args = ap.parse_args()

    if not os.path.isdir(args.root):
        print("root not found: {}".format(args.root))
        return 2

    bad = 0
    scanned = 0

    for dirpath, dirnames, filenames in os.walk(args.root):
        # Skip build output and third-party kernels wholesale.
        dirnames[:] = [d for d in dirnames
                       if d not in ("build", ".git", "managed_components")]

        for name in filenames:
            if not name.endswith(SRC_EXT):
                continue

            path = os.path.join(dirpath, name)
            if any(part in path for part in SKIP_PATH_PARTS):
                continue

            scanned += 1
            for lineno, symbol, line in scan_file(path):
                if not args.quiet:
                    rel = os.path.relpath(path, args.root)
                    print("{}:{}: {} -> {}".format(rel, lineno, symbol, line.strip()[:90]))
                bad += 1

    if bad:
        print("\n{} unguarded Xtensa-only reference(s) in {} file(s) scanned."
              .format(bad, scanned))
        print("RISC-V targets (ESP32-C5) will fail to link. Wrap each in")
        print("    #if defined(__XTENSA__)  ...  #endif")
        print("or, when the question is architectural rather than a probe, put a")
        print("portable accessor next to its users (see ft_port_running_count()).")
        return 1

    print("clean: no unguarded Xtensa-only references in {} file(s) scanned."
          .format(scanned))
    return 0


if __name__ == "__main__":
    sys.exit(main())




