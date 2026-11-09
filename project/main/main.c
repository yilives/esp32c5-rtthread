/* Combined ESP32-C5 image: RT-Thread + Wi-Fi (STA) + BLE (peripheral).
 *
 * The single-target probes proved each half separately:
 *   c5-rtt-test     kernel, tick, threads, msh, pin, flash          PASS
 *   c5-wifi-probe   scan + AP record decode + association + DHCP    PASS
 *   c5-ble-probe    controller, NimBLE host, advertising            PASS
 *
 * This image is the one thing those cannot show: that Wi-Fi and BLE coexist on
 * one C5 under RT-Thread. The C5 has a single 2.4/5 GHz radio and a shared
 * PHY/coex arbiter, so "each works alone" is genuinely not the same claim.
 *
 * Commands:
 *   c5all   start everything, then report; ends with BLE advertising and the
 *           station associated so an external radio can watch both
 *   c5stat  tick, heaps, Wi-Fi state, BLE state
 */

#include <rtthread.h>
#include <finsh.h>

#include "esp_system.h"
#include "esp_heap_caps.h"

#include "port_wifi.h"
#include "port_ble.h"
#include "port_pwm.h"
#include "port_adc.h"
#include "port_spi.h"
#include "port_twai.h"
#include "port_rmt.h"
#include "port_pcnt.h"
#include "port_mcpwm.h"
#include "port_tsens.h"
#include "port_psram.h"
#include "port_res.h"
#include "port_usj.h"
#include "port_gdma.h"
#include "port_parlio.h"
#include "port_can.h"
#include "port_lp.h"
#include "port_lp_i2c.h"
#include "port_ulp.h"

static volatile rt_uint32_t counter_a;
static volatile rt_uint32_t counter_b;

static void thread_a(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        counter_a++;
        c5_ble_poll();
        rt_thread_mdelay(1000);
    }
}

static void thread_b(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        counter_b++;
        rt_thread_mdelay(1500);
    }
}

static int cmd_stat(int argc, char **argv)
{
    rt_size_t total = 0, used = 0, max_used = 0;

    RT_UNUSED(argc);
    RT_UNUSED(argv);

    rt_memory_info(&total, &used, &max_used);

    rt_kprintf("tick    : %u\n", (unsigned)rt_tick_get());
    rt_kprintf("threads : ta=%u tb=%u\n", (unsigned)counter_a, (unsigned)counter_b);
    rt_kprintf("rt-heap : total=%u used=%u max=%u\n",
               (unsigned)total, (unsigned)used, (unsigned)max_used);
    rt_kprintf("idf-heap: free=%u\n", (unsigned)esp_get_free_heap_size());
    rt_kprintf("wifi    : %s\n", c5_wifi_state());
    rt_kprintf("ble     : %s\n", c5_ble_state());

    return 0;
}
/* The description is a macro argument list, so no commas in it. */
MSH_CMD_EXPORT_ALIAS(cmd_stat, c5stat, tick and heaps and radio state);

/* `c5all` - the coexistence test, in the order a real application would do it.
 *
 * BLE first, then Wi-Fi, then BLE advertising again: that order is deliberate.
 * Bringing BLE up after Wi-Fi is the order that exercises the coexist arbiter's
 * dynamic path (radio already busy, second stack asks for time), while BLE first
 * only tests the idle path. The re-advertise at the end keeps the observer
 * window open with both stacks live.
 */
static int cmd_all(int argc, char **argv)
{
    int rc_ble, rc_wifi, rc_adv;
    int hold_s = 60;

    if (argc >= 2)
    {
        hold_s = atoi(argv[1]);
        if (hold_s < 0)
        {
            hold_s = 0;
        }
    }

    rt_kprintf("--- c5all (hold %d s) ---\n", hold_s);

    rc_ble = c5_ble_init();
    rt_kprintf("step1   : ble_init=%d\n", rc_ble);

    rc_wifi = c5_wifi_join();
    rt_kprintf("step2   : wifi_join=%d\n", rc_wifi);

    rc_adv = c5_ble_advertise_start();
    rt_kprintf("step3   : ble_advertise=%d\n", rc_adv);

    rt_kprintf("--- c5all: ble=%d wifi=%d adv=%d ---\n", rc_ble, rc_wifi, rc_adv);
    if (rc_ble == 0 && rc_wifi == 0 && rc_adv == 0)
    {
        rt_kprintf("c5all   : PASS (Wi-Fi associated, BLE advertising)\n");
        rt_kprintf("c5all   : use `c5tcp <host-ip> 3333` for a TCP round trip\n");

        if (hold_s > 0)
        {
            rt_kprintf("c5all   : hold for %d s; watch for 'c5-rtt' on another radio\n",
                       hold_s);
            rt_thread_mdelay((rt_int32_t)hold_s * 1000);
            rt_kprintf("c5all   : still alive after %d s with both stacks up\n", hold_s);
            rt_kprintf("c5all   : wifi=%s ble=%s\n", c5_wifi_state(), c5_ble_state());
        }
        return 0;
    }

    rt_kprintf("c5all   : FAIL\n");
    return -1;
}
MSH_CMD_EXPORT_ALIAS(cmd_all, c5all, bring up Wi-Fi and BLE together);

/* `c5tcp <host-ip> [port]` - one TCP round trip to a listener on the host.
 *
 * Separate from c5all on purpose: it needs a peer the board can reach, which the
 * test harness starts, so it cannot be part of the self-contained sequence. */
static int cmd_tcp(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("usage: c5tcp <host-ip> [port]\n");
        return -1;
    }

    return c5_wifi_tcp(argv[1], (argc >= 3) ? atoi(argv[2]) : 3333, RT_NULL, 0);
}
MSH_CMD_EXPORT_ALIAS(cmd_tcp, c5tcp, TCP round trip to a host listener);

/* `c5iperf <host-ip> [port] [seconds]` - sustained TCP send rate. */
static int cmd_tput(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("usage: c5iperf <host-ip> [port] [seconds]\n");
        return -1;
    }

    return c5_wifi_tput(argv[1],
                        (argc >= 3) ? atoi(argv[2]) : 4444,
                        (argc >= 4) ? atoi(argv[3]) : 5);
}
MSH_CMD_EXPORT_ALIAS(cmd_tput, c5iperf, sustained TCP send rate);

/* `c5bstall` - start the blocking-send reproduction with its watchdog. */
static int cmd_bstall(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    return c5_wifi_bstall();
}
MSH_CMD_EXPORT_ALIAS(cmd_bstall, c5bstall, blocking send stall witness);

/* `c5rx <host-ip> [port] [seconds]` - sustained receive from a host source. */
static int cmd_rx(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("usage: c5rx <host-ip> [port] [seconds]\n");
        return -1;
    }

    return c5_wifi_rx(argv[1],
                      (argc >= 3) ? atoi(argv[2]) : 5555,
                      (argc >= 4) ? atoi(argv[3]) : 10);
}
MSH_CMD_EXPORT_ALIAS(cmd_rx, c5rx, sustained TCP receive rate);

/* `c5tcpstate` - check whether lwIP's timers are still running. */
static int cmd_tcpstate(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    return c5_wifi_tcpstate();
}
MSH_CMD_EXPORT_ALIAS(cmd_tcpstate, c5tcpstate, lwIP timer liveness);

/* `c5txstats` - driver-side TX accept/reject counters. */
static int cmd_txstats(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    return c5_wifi_txstats();
}
MSH_CMD_EXPORT_ALIAS(cmd_txstats, c5txstats, driver TX counters);

/* `c5join2` - join the second configured AP (the alternate band). */
static int cmd_join2(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    return c5_wifi_join_alt();
}
MSH_CMD_EXPORT_ALIAS(cmd_join2, c5join2, join the 2.4G AP);

/* `c5join5` - join the 5 GHz AP of the same pair. */
static int cmd_join5(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    return c5_wifi_join_b5();
}
MSH_CMD_EXPORT_ALIAS(cmd_join5, c5join5, join the 5G AP);

/* `c5bands` - full dual-band scan, sorted by band. */
static int cmd_bands(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    return c5_wifi_bands();
}
MSH_CMD_EXPORT_ALIAS(cmd_bands, c5bands, dual band scan summary);

/* `c5pwm [pin] [hz] [duty%]` - LEDC self test, defaults pin 3, 1 kHz, 50%. */
static int cmd_pwm(int argc, char **argv)
{
    int pin = (argc >= 2) ? atoi(argv[1]) : 3;
    int hz  = (argc >= 3) ? atoi(argv[2]) : 1000;
    int duty = (argc >= 4) ? atoi(argv[3]) : 50;

    return c5_pwm_selftest(pin, hz, duty);
}
MSH_CMD_EXPORT_ALIAS(cmd_pwm, c5pwm, LEDC PWM self test);

/* `c5adc` - read every ADC channel and report raw codes. */
static int cmd_adc(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    return c5_adc_selftest();
}
MSH_CMD_EXPORT_ALIAS(cmd_adc, c5adc, SAR ADC self test);

/* `c5spiloop [inside]` - SPI loopback.
 *
 * With no argument the GPIO matrix is rewired so MOSI's output feeds MISO's
 * input: that exercises the whole shift path (FIFO, bit order, clock, both
 * pads) with no jumper. With `jumper` the matrix is left alone and the caller is
 * expected to have tied the two pads together, which additionally tests the pad
 * drivers. */
static int cmd_spiloop(int argc, char **argv)
{
    rt_bool_t inside = RT_TRUE;

    if (argc >= 2 && rt_strcmp(argv[1], "jumper") == 0)
    {
        inside = RT_FALSE;
    }

    return c5_spi_selftest(0, inside);
}
MSH_CMD_EXPORT_ALIAS(cmd_spiloop, c5spiloop, SPI loopback check);

/* `c5twai` - CAN controller self test in internal loopback, four bit rates. */
static int cmd_twai(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    return c5_twai_selftest();
}
MSH_CMD_EXPORT_ALIAS(cmd_twai, c5twai, CAN self test in loopback);

/* `c5rmt [half_us]` - RMT square wave measured by the RMT's own RX channel. */
static int cmd_rmt(int argc, char **argv)
{
    rt_uint32_t half = (argc >= 2) ? (rt_uint32_t)atoi(argv[1]) : 100u;

    return c5_rmt_selftest(half);
}
MSH_CMD_EXPORT_ALIAS(cmd_rmt, c5rmt, RMT loopback self test);

/* `c5pcnt [gpio] [edges]` - PCNT counts software-driven edges on its own pad. */
static int cmd_pcnt(int argc, char **argv)
{
    rt_base_t pin = (argc >= 2) ? (rt_base_t)atoi(argv[1]) : C5_PCNT_TEST_PIN;
    rt_uint32_t edges = (argc >= 3) ? (rt_uint32_t)atoi(argv[2]) : 100u;

    return c5_pcnt_selftest(pin, edges);
}
MSH_CMD_EXPORT_ALIAS(cmd_pcnt, c5pcnt, PCNT edge counting self test);

/* `c5mcpwm [freq_hz] [duty_permille]` - frequency by PCNT, duty by mcycle. */
static int cmd_mcpwm(int argc, char **argv)
{
    rt_uint32_t f = (argc >= 2) ? (rt_uint32_t)atoi(argv[1]) : 1000u;
    rt_uint32_t d = (argc >= 3) ? (rt_uint32_t)atoi(argv[2]) : 500u;

    return c5_mcpwm_selftest(f, d);
}
MSH_CMD_EXPORT_ALIAS(cmd_mcpwm, c5mcpwm, MCPWM frequency and duty self test);

/* `c5tsens [samples]` - plausibility, noise and self-heating, no reference needed. */
static int cmd_tsens(int argc, char **argv)
{
    rt_uint32_t n = (argc >= 2) ? (rt_uint32_t)atoi(argv[1]) : 16u;

    return c5_tsens_selftest(n);
}
MSH_CMD_EXPORT_ALIAS(cmd_tsens, c5tsens, temperature sensor self test);

/* `c5psram [test [kb]]` - PSRAM state, and a read/write check with `test`. */
static int cmd_psram(int argc, char **argv)
{
    rt_uint32_t kb = 0u;

    if (argc >= 2 && rt_strcmp(argv[1], "test") == 0)
    {
        kb = (argc >= 3) ? (rt_uint32_t)atoi(argv[2]) : 64u;
    }
    else if (argc >= 2)
    {
        kb = (rt_uint32_t)atoi(argv[1]);
    }

    return c5_psram_selftest(kb);
}
MSH_CMD_EXPORT_ALIAS(cmd_psram, c5psram, PSRAM state and read/write check);

/* `c5ulp [loops]` - start the LP core program and watch its shared counters. */
static int cmd_ulp(int argc, char **argv)
{
    rt_uint32_t loops = (argc >= 2) ? (rt_uint32_t)atoi(argv[1]) : 2000u;

    return c5_ulp_selftest(loops);
}
MSH_CMD_EXPORT_ALIAS(cmd_ulp, c5ulp, LP core (ULP RISC-V) self test);

/* `c5ap [down]` - start the SoftAP, or take it down again. */
static int cmd_ap(int argc, char **argv)
{
    if (argc >= 2 && rt_strcmp(argv[1], "down") == 0)
    {
        return c5_wifi_ap_down();
    }

    return c5_wifi_softap();
}
MSH_CMD_EXPORT_ALIAS(cmd_ap, c5ap, start the SoftAP (or `c5ap down`));

/* `c5pins` - who owns which pad, and does anything clash. */
static int cmd_pins(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    return c5_pins_report();
}
MSH_CMD_EXPORT_ALIAS(cmd_pins, c5pins, pin ownership and conflict report);

/* `c5usj` - USB Serial/JTAG device-side state (no host on this board). */
static int cmd_usj(int argc, char **argv)
{
    RT_UNUSED(argc);
    RT_UNUSED(argv);

    return c5_usj_selftest();
}
MSH_CMD_EXPORT_ALIAS(cmd_usj, c5usj, USB Serial/JTAG device-side self test);

/* `c5gdma [bytes]` - memory-to-memory copy through the AHB GDMA. */
static int cmd_gdma(int argc, char **argv)
{
    rt_uint32_t n = (argc >= 2) ? (rt_uint32_t)atoi(argv[1]) : 128u;

    return c5_gdma_selftest(n);
}
MSH_CMD_EXPORT_ALIAS(cmd_gdma, c5gdma, AHB GDMA memory-to-memory self test);

/* `c5parlio [bytes] [clk_hz]` - PARLIO clocks bytes out; PCNT counts the clocks. */
static int cmd_parlio(int argc, char **argv)
{
    rt_uint32_t n = (argc >= 2) ? (rt_uint32_t)atoi(argv[1]) : 16u;
    rt_uint32_t hz = (argc >= 3) ? (rt_uint32_t)atoi(argv[2]) : 1000000u;

    return c5_parlio_selftest(n, hz);
}
MSH_CMD_EXPORT_ALIAS(cmd_parlio, c5parlio, PARLIO transmit self test);

/* `c5can` - drive TWAI through the rt_can device class, not through twai.c. */
static int cmd_can(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    return c5_can_selftest();
}
MSH_CMD_EXPORT_ALIAS(cmd_can, c5can, rt_can device-class self test);

/* `c5sleep [seconds]` - deep sleep and report what survived, on the next boot. */
static int cmd_sleep(int argc, char **argv)
{
    rt_uint32_t s = (argc >= 2) ? (rt_uint32_t)atoi(argv[1]) : 3u;

    return c5_lp_sleep_selftest(s);
}
MSH_CMD_EXPORT_ALIAS(cmd_sleep, c5sleep, deep sleep then report on next boot);

/* `c5probe [first] [last]` - drive each pad and read it back. */
static int cmd_probe(int argc, char **argv)
{
    int first = (argc >= 2) ? atoi(argv[1]) : 0;
    int last  = (argc >= 3) ? atoi(argv[2]) : 28;

    return c5_probe_pads(first, last);
}
MSH_CMD_EXPORT_ALIAS(cmd_probe, c5probe, which pads respond on this package);

/* `c5lpi2c` - the low-power I2C controller on its fixed IO2/IO3 pads. */
static int cmd_lpi2c(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    return c5_lp_i2c_selftest();
}
MSH_CMD_EXPORT_ALIAS(cmd_lpi2c, c5lpi2c, LP I2C controller self test);

int main(void)
{
    rt_thread_t tid;

    rt_kprintf("\n");
    rt_kprintf("RT-Thread %d.%d.%d on ESP32-C5 (Wi-Fi + BLE)\n",
               RT_VERSION_MAJOR, RT_VERSION_MINOR, RT_VERSION_PATCH);
    rt_kprintf("tick=%d Hz, cpus=%d, msh on UART0\n",
               RT_TICK_PER_SECOND, RT_CPUS_NR);

    /* If this boot followed a deep sleep, say what crossed it. Silent on a
     * cold boot. */
    rt_hw_lp_print_sleep_record();

    finsh_set_device("uart0");

    tid = rt_thread_create("ta", thread_a, RT_NULL, 2048, 10, 10);
    if (tid != RT_NULL)
    {
        rt_thread_startup(tid);
    }

    tid = rt_thread_create("tb", thread_b, RT_NULL, 2048, 12, 10);
    if (tid != RT_NULL)
    {
        rt_thread_startup(tid);
    }

    return 0;
}









































