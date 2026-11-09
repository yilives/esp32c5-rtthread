/*
 * ESP32-C5 LP core (ULP RISC-V) program.
 *
 * This runs on the low-power core, not on the HP core the rest of this port runs
 * on, and it is a separate build: its own toolchain (rv32imac, -Os), its own
 * linker script, its own 4 KB of RTC slow memory. It is compiled into the app
 * image as a blob and started by the HP core through ulp_lp_core_load_binary() /
 * ulp_lp_core_run().
 *
 * ---------------------------------------------------------------------------
 * What it does, and why this shape.
 *
 * The point of this program is not to compute something useful - it is to prove
 * the LP core actually executes, using only evidence the HP core can see without
 * trusting the LP core's own word for it.
 *
 * Shared variables are the mechanism: a global placed in .shared_mem has its
 * address exported to the HP app as a pointer into LP memory, so the HP core can
 * read what the LP core wrote. So:
 *
 *   shared_seq    - a counter the LP core increments; a value that keeps RISING
 *                   proves execution rather than a one-shot
 *   shared_magic  - written LAST, after setup, so the HP core can tell "the
 *                   program started" from "the program finished its setup"
 *   shared_checksum - a value computed from the loop counter in a way the HP
 *                   core can recompute and compare, so the arithmetic is checked
 *                   too and not just the counter
 *
 * The LP core has no FPU, no cache, and no RT-Thread - it is bare metal with a
 * `main()` and `ulp_lp_core_delay_us()`. Anything this program needs has to be
 * statically linked into it.
 *
 * ---------------------------------------------------------------------------
 * The one thing to know before editing this file: `int main(void)` is the entry
 * point, and returning from it halts the core. There is no scheduler and no
 * exit path to unwind.
 */

#include <stdint.h>
#include <stdbool.h>

#include "ulp_lp_core_utils.h"

/* NOT in .shared_mem, deliberately.
 *
 * .shared_mem is only CONFIG_ULP_SHARED_MEM (0x10) bytes and the ULP framework
 * already fills ALL of it with its own config struct - see
 * ulp_lp_core_memory_shared.c, which static_asserts the size matches. Any user
 * variable placed there overflows the region and the LP link fails with
 * "address 0x50001010 of ulp_c5_lp.elf section `.shared_mem' is not within
 * region `shared_mem_ram'". Measured with both four and five variables.
 *
 * Ordinary .bss is the correct home: the LP core's RAM is RTC slow memory at
 * 0x50000000, which the HP core can read directly, and esp32ulp_mapgen.py
 * exports every global symbol with its absolute address. So these need no
 * special section at all - only `volatile`, which is not decoration here: the
 * loop below would otherwise be optimised into a single store at the end and the
 * HP core would see 0 for as long as it looked. */
#define ULP_SHARED volatile

/* THERE ARE EXACTLY FOUR WORDS OF SHARED MEMORY.
 *
 * CONFIG_ULP_SHARED_MEM is 0x10 and has NO Kconfig prompt, so it cannot be
 * changed from sdkconfig - writing a bigger value there is silently ignored and
 * the LP link fails with "address ... is not within region `shared_mem_ram`".
 * Measured exactly that with five variables. So the fields are packed rather
 * than one-per-word. */
ULP_SHARED uint32_t shared_seq;         /* [0] incremented every iteration */
ULP_SHARED uint32_t shared_magic;       /* [1] 0x4C504331 once setup is done */
ULP_SHARED uint32_t shared_checksum;    /* [2] recomputable from the counter */
ULP_SHARED uint32_t shared_ctl;         /* [3] bits[23:0] loops, bit[31] done */

#define ULP_MAGIC   0x4C504331u

/* Same mixing function the HP side recomputes, so the checksum is a real
 * check on the LP core's arithmetic and not just a copy of the counter. */
static uint32_t ulp_mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

int main(void)
{
    uint32_t i;
    uint32_t acc = 0;

    /* Read the loop count FIRST, before touching shared_ctl.
     *
     * An earlier version cleared the done bit with `shared_ctl = n` before the
     * loop and then re-read it, which works - but the field is also where the
     * done flag lives, so any ordering mistake silently halves the count. The
     * value is latched into a local here and the field is only written at the
     * end. */
    uint32_t n = shared_ctl & 0x00FFFFFFu;

    /* A bounded default: the HP core sets shared_loops before starting this
     * program, but a stale or zero value must not produce a program that either
     * does nothing or never ends. */
    if (n == 0u || n > 100000u)
    {
        n = 1000u;
    }

    /* Clear the done bit while KEEPING the loop count the HP core wrote, and
     * publish the count actually being used so the HP side can tell the two
     * apart when the request was out of range. */
    shared_ctl = n;
    shared_seq = 0;

    /* Written last among the setup fields, so "magic is set" means the program
     * reached the loop and not merely that it loaded. */
    shared_magic = ULP_MAGIC;

    for (i = 0; i < n; i++)
    {
        acc += ulp_mix(i);

        /* Every iteration, not every N: the HP core polls this to decide whether
         * the core is running, and a counter that moves only at the end answers
         * that question too late. */
        shared_seq = i + 1u;
        shared_checksum = acc;
    }

    shared_ctl = n | 0x80000000u;

    /* Slot for one more deliberate delay so the HP side can observe `done`
     * before the core halts - returning from main halts it immediately. */
    ulp_lp_core_delay_us(200);

    return 0;
}



