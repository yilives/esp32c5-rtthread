/*
 * ESP32-C5 LP core (ULP RISC-V): starting it from the HP core and proving it ran.
 *
 * The LP core is a second, much smaller RISC-V core with its own 4 KB of RTC slow
 * memory. It stays alive in deep sleep; the HP core starts it, it runs a program
 * out of LP memory, and the two communicate through variables placed in a
 * .shared_mem region whose addresses both sides agree on.
 *
 * ---------------------------------------------------------------------------
 * How this is verified, and what "verified" means for a second core.
 *
 * The HP core cannot watch the LP core execute, so the test has to be built out
 * of things only the LP core could have done:
 *
 *   1. A magic word the LP program writes AFTER its setup. Seeing it means the
 *      program reached its main loop, not merely that a binary was loaded.
 *   2. A counter that keeps rising. A one-shot write could come from a stale
 *      memory value left over from a previous run; a value that climbs while the
 *      HP core polls it cannot.
 *   3. A checksum the HP core recomputes independently from the same loop count.
 *      That checks the LP core's arithmetic rather than just its liveness.
 *
 * All three are read from the exported shared-memory pointers, which are plain
 * HP-side addresses - no mailbox, no interrupt, nothing the LP core has to
 * cooperate with to be observed.
 *
 * What it does NOT verify: deep-sleep behaviour (the LP core is started here
 * while the HP core is awake), the LP core's own peripherals (LP UART, LP I2C,
 * LP ADC), and anything about its power consumption.
 */

#include <rthw.h>
#include <rtthread.h>

#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if defined(CONFIG_ULP_COPROC_ENABLED) && defined(CONFIG_ULP_COPROC_TYPE_LP_CORE)

#include "esp_err.h"
#include "ulp_lp_core.h"
/* ulp_lp_core_utils.h is an LP-core-side header: it declares functions that only
 * exist inside the LP core's own program image (ulp_lp_core_delay_us and friends
 * are compiled into the ULP blob, not into the HP app). Including it here fails -
 * "No such file or directory" - because the ULP component only puts
 * lp_core/include on the include path for the LP build, not for HP components.
 *
 * The HP side only needs ulp_lp_core.h, which is what ulp_lp_core_run() and
 * ulp_lp_core_load_binary() come from. */

/* The binary, embedded by ulp_add_project() via target_add_binary_data(). */
extern const uint8_t ulp_c5_lp_bin_start[] asm("_binary_ulp_c5_lp_bin_start");
extern const uint8_t ulp_c5_lp_bin_end[]   asm("_binary_ulp_c5_lp_bin_end");

/* The shared variables come from ulp_c5_lp.h, which ulp_add_project() generates
 * inside this component's build directory - which is exactly why this file lives
 * in main/ and not in the rtthread component. The header declares them as
 * extern uint32_t and the generated .ld defines each one at its absolute address
 * inside LP memory, so these accesses go straight into the LP core's RAM.
 *
 * Two details that cost a build each:
 *
 *   - The generated names are PREFIXED (ulp_shared_seq), not the bare globals
 *     from the LP source. "did you mean 'ulp_shared_seq'" is the compiler being
 *     helpful about a real mismatch.
 *   - The loop count and the done flag share one word because .shared_mem is a
 *     fixed 0x10 bytes that the ULP framework already fills with its own config
 *     struct - user variables have to live in ordinary .bss, of which there is
 *     plenty. */
#include "ulp_c5_lp.h"

#define ULP_SEQ         ulp_shared_seq
#define ULP_MAGIC_VAR   ulp_shared_magic
#define ULP_CHECKSUM    ulp_shared_checksum
#define ULP_CTL         ulp_shared_ctl

#define ULP_CTL_LOOPS_MASK  0x00FFFFFFu
#define ULP_CTL_DONE_BIT    0x80000000u

#define ULP_MAGIC   0x4C504331u

/* Must match ulp_mix() in main/ulp/main.c. Duplicated deliberately: if the two
 * sides shared one implementation there would be nothing to check. */
static rt_uint32_t c5_ulp_mix(rt_uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* Recompute the checksum the LP program should have produced. */
static rt_uint32_t c5_ulp_expect(rt_uint32_t loops)
{
    rt_uint32_t acc = 0;
    rt_uint32_t i;

    for (i = 0; i < loops; i++)
    {
        acc += c5_ulp_mix(i);
    }

    return acc;
}

int c5_ulp_selftest(rt_uint32_t loops)
{
    rt_uint32_t expect;
    rt_uint32_t observed_seq = 0;
    rt_uint32_t max_seq = 0;
    rt_uint32_t stable = 0;
    rt_uint32_t waited_ms = 0;
    rt_uint32_t i;
    esp_err_t err;

    if (loops == 0u || loops > 100000u)
    {
        loops = 2000u;
    }

    rt_kprintf("c5ulp   : LP core, %u loop(s) requested\n", (unsigned)loops);

    err = ulp_lp_core_load_binary(ulp_c5_lp_bin_start,
                                  (size_t)(ulp_c5_lp_bin_end - ulp_c5_lp_bin_start));
    if (err != ESP_OK)
    {
        rt_kprintf("c5ulp   : FAIL, load_binary returned %d\n", (int)err);
        return -1;
    }

    /* The shared variables are written AFTER load_binary, not before.
     *
     * ulp_lp_core_load_binary() does hal_memset(base, 0, CONFIG_ULP_COPROC_RESERVE_MEM)
     * on the whole LP RAM before copying the program in - "Clear LP RAM - also
     * initializes LP-side bss" in its own comment. Anything the HP core wrote
     * into LP memory beforehand is therefore wiped.
     *
     * Measured: writing loops = 2000 before the load and starting the core
     * produced seq = 1000, because the LP program found a zeroed control word,
     * fell back to its default of 1000, and reported that. The counter, the
     * magic and the done flag all looked right - only the loop count disagreed,
     * which is exactly the shape of "the handoff happened too early". */
    ULP_CTL = loops & ULP_CTL_LOOPS_MASK;
    ULP_SEQ = 0;
    ULP_MAGIC_VAR = 0;
    ULP_CHECKSUM = 0;

    {
        ulp_lp_core_cfg_t cfg = {
            .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
        };

        err = ulp_lp_core_run(&cfg);
        if (err != ESP_OK)
        {
            rt_kprintf("c5ulp   : FAIL, run returned %d\n", (int)err);
            return -1;
        }
    }

    /* Poll for the counter to move. The LP core runs at a few MHz with no cache,
     * so a couple of thousand iterations take far longer than an HP-side tick;
     * this waits up to 2 s and reports how long it took, because "slow" and
     * "not running" are different answers. */
    while (waited_ms < 2000u)
    {
        rt_uint32_t seq = ULP_SEQ;

        if (seq > max_seq)
        {
            max_seq = seq;
        }

        if (seq != observed_seq)
        {
            observed_seq = seq;
            stable = 0;
        }
        else
        {
            stable++;
        }

        if ((ULP_CTL & ULP_CTL_DONE_BIT) != 0u)
        {
            break;
        }

        rt_thread_mdelay(1);
        waited_ms++;
    }

    expect = c5_ulp_expect(loops);

    rt_kprintf("c5ulp   : magic=0x%08x (want 0x%08x), seq=%u/%u, done=%u after %u ms\n",
               (unsigned)ULP_MAGIC_VAR, (unsigned)ULP_MAGIC,
               (unsigned)ULP_SEQ, (unsigned)loops,
               ((ULP_CTL & ULP_CTL_DONE_BIT) ? 1u : 0u), (unsigned)waited_ms);
    rt_kprintf("c5ulp   : checksum=0x%08x (want 0x%08x)\n",
               (unsigned)ULP_CHECKSUM, (unsigned)expect);

    if (ULP_MAGIC_VAR != ULP_MAGIC)
    {
        rt_kprintf("c5ulp   : FAIL, the LP core did not reach its setup - the "
                   "program never ran\n");
        return -1;
    }

    if (max_seq == 0u)
    {
        rt_kprintf("c5ulp   : FAIL, the shared counter never moved - nothing "
                   "executed\n");
        return -1;
    }

    if ((ULP_CTL & ULP_CTL_DONE_BIT) == 0u)
    {
        rt_kprintf("c5ulp   : FAIL, the counter reached %u of %u and stopped "
                   "there\n", (unsigned)max_seq, (unsigned)loops);
        return -1;
    }

    if (ULP_SEQ != loops)
    {
        rt_kprintf("c5ulp   : FAIL, the loop ended at %u of %u iteration(s)\n",
                   (unsigned)ULP_SEQ, (unsigned)loops);
        return -1;
    }

    if (ULP_CHECKSUM != expect)
    {
        rt_kprintf("c5ulp   : FAIL, the checksum does not match what the LP "
                   "core's arithmetic should produce\n");
        return -1;
    }

    /* Prove it can be started again, which a core that latched into a halt state
     * would fail. This is the difference between "it ran once" and "it runs". */
    {
        rt_uint32_t second;

        /* No load_binary this time, so LP RAM is left as it is and the fields
         * can be written directly before the restart. */
        ULP_SEQ = 0;
        ULP_MAGIC_VAR = 0;
        ULP_CTL = 500u;

        err = ulp_lp_core_run(&(ulp_lp_core_cfg_t){
            .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
        });
        if (err != ESP_OK)
        {
            rt_kprintf("c5ulp   : FAIL, second run returned %d\n", (int)err);
            return -1;
        }

        second = 0;
        for (i = 0; i < 2000u && (ULP_CTL & ULP_CTL_DONE_BIT) == 0u; i++)
        {
            rt_thread_mdelay(1);
        }
        second = ULP_SEQ;

        rt_kprintf("c5ulp   : second run: seq=%u/500 done=%u\n",
                   (unsigned)second, ((ULP_CTL & ULP_CTL_DONE_BIT) ? 1u : 0u));

        if (second != 500u || (ULP_CTL & ULP_CTL_DONE_BIT) == 0u)
        {
            rt_kprintf("c5ulp   : FAIL, the LP core did not run a second time\n");
            return -1;
        }
    }

    rt_kprintf("c5ulp   : PASS (the LP core executed, its arithmetic checks out, "
               "and it restarts; deep-sleep behaviour and LP peripherals are not "
               "exercised)\n");
    return 0;
}

#endif /* CONFIG_ULP_COPROC_ENABLED && CONFIG_ULP_COPROC_TYPE_LP_CORE */





