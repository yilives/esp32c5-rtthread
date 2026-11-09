/* Wi-Fi station bring-up for the combined C5 image.
 *
 * Lifted from c5-wifi-probe/main/main.c once the sequence was proven there, with
 * one change: it is idempotent, because `c5all` may be run twice and esp_wifi
 * returns ESP_ERR_WIFI_INIT_STATE for a second init that a diagnostic should not
 * report as a failure.
 */

#include <rtthread.h>
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_err.h"

#include "wifi_creds.h"
#include "port_wifi.h"

#include <string.h>
#include <errno.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <fcntl.h>
#include <sys/time.h>
#include "lwip/priv/tcp_priv.h"

static volatile int s_scan_done;
static volatile int s_got_ip;
static volatile int s_connects;
static volatile int s_disconnects;
static volatile int s_reason;
static volatile int s_connected;
static volatile int s_ever_joined;
static char s_state[64] = "down";

static void wifi_events(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT)
    {
        if (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_SCAN_DONE)
        {
            if (id == WIFI_EVENT_SCAN_DONE)
            {
                s_scan_done = 1;
            }
        }
        else if (id == WIFI_EVENT_STA_CONNECTED)
        {
            s_connected = 1;
        }
        else if (id == WIFI_EVENT_STA_DISCONNECTED)
        {
            wifi_event_sta_disconnected_t *d = data;

            s_reason = d ? d->reason : -1;
            s_disconnects++;
            s_connected = 0;
        }
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        s_got_ip = 1;
        s_ever_joined = 1;
    }
}

static int wifi_join_creds(const char *ssid, const char *pass)
{
    esp_err_t err;
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wc;
    esp_netif_t *sta;
    int waited;

    s_scan_done = 0;
    s_got_ip = 0;
    s_connected = 0;
    s_disconnects = 0;
    s_connects = 0;

    err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return -1;
    }
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        rt_kprintf("wifi    : netif 0x%x\n", (unsigned)err);
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        rt_kprintf("wifi    : event loop 0x%x\n", (unsigned)err);
        return -1;
    }

    sta = esp_netif_create_default_wifi_sta();
    if (sta == NULL)
    {
        rt_kprintf("wifi    : FAIL, no default STA netif\n");
        return -1;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_events, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_events, NULL);

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE)
    {
        rt_kprintf("wifi    : FAIL init 0x%x\n", (unsigned)err);
        return -1;
    }

    rt_memset(&wc, 0, sizeof(wc));
    rt_strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    rt_strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    rt_kprintf("wifi    : target ssid=\"%s\" (%d byte(s))\n",
               ssid, (int)rt_strlen(ssid));

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    rt_kprintf("wifi    : mode=0x%x\n", (unsigned)err);

    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    rt_kprintf("wifi    : config=0x%x\n", (unsigned)err);
    if (err != ESP_OK)
    {
        return -1;
    }

    err = esp_wifi_start();
    rt_kprintf("wifi    : start=0x%x\n", (unsigned)err);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE)
    {
        return -1;
    }

    /* Power save off, and this is a measurement rather than a default.
     *
     * Idle, with no TCP at all, this board answers ICMP with 41-129 ms round
     * trips and loses the first packet every run:
     *
     *   Packets: Sent = 8, Received = 7, Lost = 1 (12% loss)
     *   Minimum = 41ms, Maximum = 129ms, Average = 65ms
     *
     * on an AP 2 m away that this same host reaches in under 2 ms. That is the
     * signature of modem sleep: the radio parks between beacons and every
     * inbound frame waits for the next wake-up. Stock IDF enables WIFI_PS_MIN_MODEM
     * in the STA default config, so the port inherits it.
     *
     * It matters for the transmit investigation of 闁圭厧鍊归幃?1-13 because a sleeping
     * radio stops draining the TX path on its own schedule - which is a second,
     * independent way to starve a blocking send(). Turning it off removes that
     * variable before the remaining defect is blamed on the driver hand-off. */
    {
        esp_err_t ps = esp_wifi_set_ps(WIFI_PS_NONE);

        rt_kprintf("wifi    : set_ps(NONE)=0x%x\n", (unsigned)ps);
    }

    /* Scan first, then pin the BSSID - see the long note in
     * c5-wifi-probe/main/main.c. Without the pin, a cold connect hits
     * reason=202 (AUTH_FAIL) most of the time on this dual-band AP; with it,
     * association lands in ~2 s. */
    {
        wifi_scan_config_t sc;
        uint16_t cnt = 8;
        wifi_ap_record_t recs[8];

        rt_memset(&sc, 0, sizeof(sc));
        sc.ssid = (uint8_t *)ssid;      /* scan for the SSID we are about to join */

        s_scan_done = 0;                /* issue 4: must start at 0 per call */
        err = esp_wifi_scan_start(&sc, false);
        for (waited = 0; !s_scan_done && waited < 15000; waited += 500)
        {
            rt_thread_mdelay(500);
        }
        if (!s_scan_done)
        {
            rt_kprintf("wifi    : WARN no SCAN_DONE in 15 s, joining unpinned\n");
        }

        rt_memset(recs, 0, sizeof(recs));
        if (esp_wifi_scan_get_ap_records(&cnt, recs) == ESP_OK && cnt > 0)
        {
            int i, best = 0;

            for (i = 1; i < (int)cnt; i++)
            {
                if (recs[i].rssi > recs[best].rssi)
                {
                    best = i;
                }
            }

            rt_kprintf("wifi    : bss %02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d\n",
                       recs[best].bssid[0], recs[best].bssid[1],
                       recs[best].bssid[2], recs[best].bssid[3],
                       recs[best].bssid[4], recs[best].bssid[5],
                       (unsigned)recs[best].primary, recs[best].rssi);

            rt_memcpy(wc.sta.bssid, recs[best].bssid, 6);
            wc.sta.bssid_set = 1;
            wc.sta.channel = recs[best].primary;
            esp_wifi_set_config(WIFI_IF_STA, &wc);
        }
        else
        {
            rt_kprintf("wifi    : SSID not found in scan, connecting unpinned\n");
        }
    }

    err = esp_wifi_connect();
    s_connects++;
    rt_kprintf("wifi    : connect=0x%x\n", (unsigned)err);

    /* Retry on 202. Measured: the same reason code appears on stock FreeRTOS
     * (c5-stock-wifi) for the same AP, so it is the AP's 5 GHz behaviour, not
     * this port. Retrying is what an application would do. */
    for (waited = 0; !s_got_ip && waited < 30000; waited += 1000)
    {
        rt_thread_mdelay(1000);

        if (!s_got_ip && s_disconnects > 0 && s_disconnects < 3 &&
            (waited % 5000) == 0)
        {
            rt_kprintf("wifi    : retry (reason=%d)\n", s_reason);
            esp_wifi_connect();
            s_connects++;
        }
    }

    if (s_got_ip)
    {
        esp_netif_ip_info_t ip;

        if (esp_netif_get_ip_info(sta, &ip) == ESP_OK)
        {
            rt_snprintf(s_state, sizeof(s_state), "associated %u.%u.%u.%u (connects=%d)",
                        (unsigned)(ip.ip.addr & 0xff),
                        (unsigned)((ip.ip.addr >> 8) & 0xff),
                        (unsigned)((ip.ip.addr >> 16) & 0xff),
                        (unsigned)((ip.ip.addr >> 24) & 0xff),
                        s_connects);
        }
        rt_kprintf("wifi    : GOT_IP, %s\n", s_state);
        return 0;
    }

    rt_snprintf(s_state, sizeof(s_state), "down (reason=%d connects=%d)",
                s_reason, s_connects);
    rt_kprintf("wifi    : FAIL, %s\n", s_state);
    return -1;
}

/* Two entry points, one implementation.
 *
 * The band test needs to join a different AP without rebuilding, and the
 * existing `c5all` must keep working unchanged, so the credentials became a
 * parameter with two thin wrappers rather than a global that both paths mutate.
 * A global would have made `c5all` order-dependent on whether a band test had
 * run before it - exactly the kind of hidden state that makes a diagnostic
 * report the wrong thing. */
int c5_wifi_join(void)
{
    return wifi_join_creds(WIFI_TEST_SSID, WIFI_TEST_PASS);
}

int c5_wifi_join_alt(void)
{
    return wifi_join_creds(WIFI_B24_SSID, WIFI_B24_PASS);
}

int c5_wifi_join_b5(void)
{
    return wifi_join_creds(WIFI_B5_SSID, WIFI_B5_PASS);
}

/* `c5bands` - which bands can this board actually see and join?
 *
 * The question "does 2.4 GHz work as well as 5 GHz" cannot be answered by
 * looking at one connection: a station that associates on channel 149 has
 * proved nothing about channel 6, because the two bands use different PHY modes,
 * different regulatory limits and different calibration tables.
 *
 * So this does the receive half in one command: a scan with no SSID filter walks
 * every channel of both bands, and every record carries a channel number, so
 * sorting the records by channel answers "can the radio receive on 2.4 GHz"
 * directly. Joining per band is done by the caller (`c5join2`), because it needs
 * an AP that exists in each band.
 */
int c5_wifi_bands(void)
{
    esp_err_t err;
    wifi_scan_config_t sc;
    int waited;
    int n24 = 0, n5 = 0, nother = 0;
    int i;

    s_scan_done = 0;

    rt_memset(&sc, 0, sizeof(sc));      /* no SSID filter: full dual-band walk */

    err = esp_wifi_scan_start(&sc, false);
    rt_kprintf("bands   : scan=0x%x (full walk, both bands)\n", (unsigned)err);
    if (err != ESP_OK && err != ESP_ERR_WIFI_STATE)
    {
        return -1;
    }

    /* The full walk takes ~12 s on this site (measured: 12197 ms on stock
     * FreeRTOS, report 搂8.6), so 30 s is generous without hiding a hang. */
    for (waited = 0; !s_scan_done && waited < 30000; waited += 500)
    {
        rt_thread_mdelay(500);
    }

    if (!s_scan_done)
    {
        rt_kprintf("bands   : FAIL, no SCAN_DONE in 30 s\n");
        return -1;
    }

    {
        uint16_t cnt = 48;
        static wifi_ap_record_t recs[48];

        rt_memset(recs, 0, sizeof(recs));

        if (esp_wifi_scan_get_ap_records(&cnt, recs) != ESP_OK)
        {
            rt_kprintf("bands   : FAIL, cannot read records\n");
            return -1;
        }

        rt_kprintf("bands   : %u access point(s) heard\n", (unsigned)cnt);

        for (i = 0; i < (int)cnt; i++)
        {
            unsigned ch = recs[i].primary;

            if (ch >= 1 && ch <= 14)
            {
                n24++;
            }
            else if (ch >= 32)
            {
                n5++;
            }
            else
            {
                nother++;
            }
        }

        rt_kprintf("bands   : 2.4 GHz=%d, 5 GHz=%d, other=%d\n",
                   n24, n5, nother);

        /* Hex-dump the SSID of every record on this run.
         *
         * SSID matching is a byte comparison, and a name typed into a source file
         * can differ from what the AP broadcasts in ways no console can show: a
         * trailing space, a full-width character, a different normalisation. msh
         * cannot print UTF-8 reliably, so printing the name is not evidence.
         * Printing the bytes is, and it costs one line per AP. */
        for (i = 0; i < (int)cnt; i++)
        {
            int j;
            int len = 0;

            while (len < 32 && recs[i].ssid[len] != 0)
            {
                len++;
            }

            if (len == 0)
            {
                continue;               /* hidden SSID */
            }

            rt_kprintf("bands   : ssid[%d] ch=%u rssi=%d len=%d :",
                       i, recs[i].primary, recs[i].rssi, len);
            for (j = 0; j < len; j++)
            {
                rt_kprintf(" %02x", recs[i].ssid[j]);
            }
            rt_kprintf("\n");
        }

        /* Print the strongest few of each band: a count alone can be satisfied by
         * a cached list, whereas a BSSID plus channel plus RSSI can only come
         * from a working receive chain on that channel. */
        for (i = 0; i < (int)cnt; i++)
        {
            unsigned ch = recs[i].primary;

            if (ch >= 1 && ch <= 14 && recs[i].rssi > -60)
            {
                rt_kprintf("bands   : 2.4G %02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d\n",
                           recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                           recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5],
                           ch, recs[i].rssi);
            }
        }

        for (i = 0; i < (int)cnt; i++)
        {
            unsigned ch = recs[i].primary;

            if (ch >= 32 && recs[i].rssi > -60)
            {
                rt_kprintf("bands   : 5G   %02x:%02x:%02x:%02x:%02x:%02x ch=%u rssi=%d\n",
                           recs[i].bssid[0], recs[i].bssid[1], recs[i].bssid[2],
                           recs[i].bssid[3], recs[i].bssid[4], recs[i].bssid[5],
                           ch, recs[i].rssi);
            }
        }

        /* A previous join may have pinned a BSSID. Clear it, or the next scan and
         * the next association both inherit the old channel and this test
         * measures the previous test instead of the current one. */
        {
            wifi_config_t wc;

            rt_memset(&wc, 0, sizeof(wc));
            esp_wifi_get_config(WIFI_IF_STA, &wc);
            wc.sta.bssid_set = 0;
            wc.sta.channel = 0;
            esp_wifi_set_config(WIFI_IF_STA, &wc);
        }

        return 0;
    }
}

/* `c5tcp <ip> <port>` - one TCP round trip, which is the actual end of the
 * platform chain.
 *
 * Association and DHCP prove the link and the DHCP client. They do not prove
 * that lwIP's socket layer runs on RT-Thread: a socket call goes through the
 * shim's queues (lwIP's own task waits on them) and through the netif's receive
 * path, and a broken semaphore there loses the packet without an error. One
 * connect + send + receive over a real peer is the only way to know.
 *
 * The peer is a TCP listener on the development host; the reply text is checked,
 * not just the return code, because a zero-length receive is a "success" too.
 */
int c5_wifi_tcp(const char *host, int port, char *out, rt_size_t out_len)
{
    struct sockaddr_in sa;
    int s;
    int n;
    const char *msg = "ping-from-c5";
    char buf[64];

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
    {
        rt_kprintf("tcp     : socket failed errno=%d\n", errno);
        return -1;
    }

    rt_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = inet_addr(host);

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    {
        rt_kprintf("tcp     : connect %s:%d failed errno=%d\n", host, port, errno);
        close(s);
        return -1;
    }
    rt_kprintf("tcp     : connected to %s:%d\n", host, port);

    n = send(s, msg, rt_strlen(msg), 0);
    rt_kprintf("tcp     : sent %d byte(s)\n", n);

    n = recv(s, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
    {
        rt_kprintf("tcp     : recv returned %d errno=%d\n", n, errno);
        close(s);
        return -1;
    }
    buf[n] = '\0';
    rt_kprintf("tcp     : received %d byte(s): %s\n", n, buf);

    close(s);

    if (rt_strncmp(buf, "pong-from-pc", 12) != 0)
    {
        rt_kprintf("tcp     : FAIL, reply is not the expected one\n");
        return -1;
    }

    if (out != RT_NULL && out_len > 0)
    {
        rt_snprintf(out, out_len, "tcp ok: %s", buf);
    }

    return 0;
}

const char *c5_wifi_state(void)
{
    return s_state;
}

/* `c5iperf <host> [port] [seconds]` - sustained TCP throughput, board as sender.
 *
 * A single 12-byte echo proves the data path exists. It says nothing about
 * whether the link sustains anything, and on a dual-band Wi-Fi 6 part that is
 * the number worth having: it is the first place a broken receive window, a
 * starved lwIP thread or a shim queue that loses the occasional buffer shows up
 * as a throughput figure rather than as a correctness failure.
 *
 * The board reports bytes and the rate it measured itself; the receiver on the
 * host counts independently, so a board that over-reports is caught.
 */
int c5_wifi_tput(const char *host, int port, int seconds)
{
    struct sockaddr_in sa;
    int s;
    int n;
    int sent_total = 0;
rt_tick_t t0, t1;
    static char buf[1024];
    int i;
    int blocks = 0;

    for (i = 0; i < (int)sizeof(buf); i++)
    {
        buf[i] = (char)('A' + (i % 26));
    }

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
    {
        rt_kprintf("tput    : socket failed errno=%d\n", errno);
        return -1;
    }

    rt_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = inet_addr(host);

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    {
        rt_kprintf("tput    : connect %s:%d failed errno=%d\n", host, port, errno);
        close(s);
        return -1;
    }

    rt_kprintf("tput    : sending for %d s ...\n", seconds);

    t0 = rt_tick_get();
    t1 = t0;

    /* Non-blocking send, and the reason is a measured hang.
     *
     * This loop used to call send() blocking. Measured: ~22 KB went out, the
     * TCP window closed, and send() then never returned - the shell stopped
     * answering, `list thread` afterwards showed the tcpip thread in a healthy
     * suspend with a tick to spare, and the board was alive but unreachable,
     * while the host's sink sat at 22528 bytes for the rest of the run.
     *
     * A blocking send from the shell thread waits on a lwIP queue, and draining
     * that queue needs the tcpip thread to run; something in that hand-off never
     * completes here. Non-blocking send makes the same measurement without
     * depending on it: the socket reports EWOULDBLOCK when the window is full,
     * this loop yields for one tick, and the throughput figure that comes out is
     * the honest one under whatever the real constraint is.
     *
     * That the blocking path stalls is a genuine finding, not a workaround - it
     * is recorded in the porting report rather than hidden by this change. */
    {
        int flags = fcntl(s, F_GETFL, 0);

        if (flags >= 0)
        {
            fcntl(s, F_SETFL, flags | O_NONBLOCK);
        }
    }

    while ((rt_tick_t)(t1 - t0) < (rt_tick_t)(seconds * RT_TICK_PER_SECOND))
    {
        n = send(s, buf, sizeof(buf), 0);
        if (n > 0)
        {
            sent_total += n;
            t1 = rt_tick_get();
            continue;
        }

        if (n < 0 && errno == EWOULDBLOCK)
        {
            /* Yield for ONE TICK, do not spin. Both variants were measured:
             *
             *   rt_thread_mdelay(1)   204448 B in 5 s  (327 kbit/s)
             *   rt_thread_yield()        6784 B in 5 s   (10 kbit/s),
             *                            with 94904 EWOULDBLOCK returns and
             *                            the host receiver seeing 1024 bytes
             *
             * So a yield does not hand control to whatever drains the TX path,
             * while a one-tick sleep does. That is the shape of a producer that
             * has to wait on a tick-driven event rather than on a task
             * hand-off, and the 32x difference is the evidence for it.
             *
             * `blocks` stays in the output so the trade-off is visible: a high
             * count with a low rate means the send buffer is small; a low count
             * with a low rate means the wait is somewhere else. */
            blocks++;
            rt_thread_mdelay(1);
            t1 = rt_tick_get();
            continue;
        }

        rt_kprintf("tput    : send returned %d errno=%d after %d bytes\n",
                   n, errno, sent_total);
        break;
    }

    close(s);

    {
        rt_uint32_t ms = (rt_uint32_t)((t1 - t0) * 1000u / RT_TICK_PER_SECOND);
        rt_uint32_t kbps = ms ? (rt_uint32_t)(((rt_uint64_t)sent_total * 8u) / ms) : 0u;

        rt_kprintf("tput    : %d bytes in %u ms = %u kbit/s, %d window stall(s)\n",
                   sent_total, (unsigned)ms, (unsigned)kbps, blocks);

        if (sent_total <= 0)
        {
            rt_kprintf("tput    : FAIL, nothing was sent\n");
            return -1;
        }
    }

    return 0;
}

/* `c5bstall` - reproduce the blocking-send stall with a witness.
 *
 * The stall of the porting report 闂?.2 was observed only as "the shell thread
 * never comes back". That is not enough to act on: the shell thread IS the one
 * that blocks, so the board cannot report its own state while it is stuck.
 *
 * This command puts the blocking send on its own thread and keeps a
 * higher-priority watchdog alive to print what the relevant threads look like
 * while the send is stuck. It reads RT-Thread's own fields - the same ones
 * `list thread` reads - so the output is kernel state, not a guess:
 *
 *   c5bsend suspended   -> lwIP is waiting for the tcpip thread to acknowledge
 *   tcpip   ready/fault -> it can run, so the problem is the acknowledgement
 *   tcpip   suspended   -> it is blocked itself, pointing at the mbox
 */
static volatile int s_stall_stop;
static volatile int s_stall_send_done;
static volatile int s_stall_sent;

static void stall_dump_thread(const char *name)
{
    rt_thread_t t = rt_thread_find((char *)name);
    rt_uint8_t  stat;
    const char *state;

    if (t == RT_NULL)
    {
        rt_kprintf("bstall  : thread '%s' not found\n", name);
        return;
    }

    /* stat and current_priority moved into the scheduler context in RT-Thread
     * 5.x (rtdef.h -> RT_SCHED_THREAD_CTX), and reached through the accessor
     * macros that components/finsh/cmd.c's list_thread uses. Writing
     * t->stat / t->current_priority is the 4.x layout and does not compile. */
    stat = RT_SCHED_CTX(t).stat & RT_THREAD_STAT_MASK;

    if (stat == RT_THREAD_READY)
    {
        state = "ready";
    }
    else if ((stat & RT_THREAD_SUSPEND_MASK) == RT_THREAD_SUSPEND_MASK)
    {
        state = "suspend";
    }
    else if (stat == RT_THREAD_RUNNING)
    {
        state = "running";
    }
    else if (stat == RT_THREAD_CLOSE)
    {
        state = "close";
    }
    else
    {
        state = "init";
    }

    rt_kprintf("bstall  : %-8s %s prio=%d left_tick=%u err=%d sent=%d\n",
               name, state, (int)RT_SCHED_PRIV(t).current_priority,
               (unsigned)RT_SCHED_PRIV(t).remaining_tick, (int)t->error,
               (int)s_stall_sent);
}

static void stall_watchdog(void *param)
{
    int i;

    RT_UNUSED(param);

    for (i = 0; i < 10 && !s_stall_stop; i++)
    {
        rt_thread_mdelay(2000);
        rt_kprintf("bstall  : --- after %d s, send_done=%d ---\n",
                   (i + 1) * 2, s_stall_send_done);
        /* The verdict from the first witness run, measured: crit=0 and irq_off=0
         * the whole time, while the tcpip thread alternated between `ready` and
         * `suspend` and never once ran long enough to drain its mailbox. So this
         * is NOT a locked scheduler and NOT a masked interrupt - the ready
         * thread is simply not being selected often enough to make progress.
         * It is kept in the dump because a regression that reintroduces a lock
         * would show up here immediately. */
        rt_kprintf("bstall  : crit=%d irq_off=%d\n",
                   (int)rt_critical_level(),
                   (int)rt_interrupt_get_nest());
        stall_dump_thread("c5bsend");
        stall_dump_thread("tcpip");
        stall_dump_thread("tshell");
    }
}

static void stall_sender(void *param)
{
    struct sockaddr_in sa;
    int s = *(int *)param;
    int n;
    int total = 0;
    static char buf[1024];
    int i;

    for (i = 0; i < (int)sizeof(buf); i++)
    {
        buf[i] = (char)('A' + (i % 26));
    }

    rt_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(4444);
    sa.sin_addr.s_addr = inet_addr("192.168.95.53");

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    {
        rt_kprintf("bstall  : connect failed errno=%d\n", errno);
        s_stall_send_done = 1;
        s_stall_stop = 1;
        return;
    }

    rt_kprintf("bstall  : connected, 1 KB sends with SO_SNDTIMEO=2000 ms\n");

    /* A send timeout, and the reason is diagnostic rather than cosmetic.
     *
     * A bare blocking send() never returns on this port (闂?1), so the loop could
     * never tell "lwIP is waiting for window" from "lwIP has lost the wake-up".
     * With SO_SNDTIMEO the call is bounded: EAGAIN on expiry means the socket is
     * alive and simply out of window, while a call that exceeds the timeout and
     * still does not return is the lost wake-up this port is being investigated
     * for. Counting both makes the difference visible in the dump. */
    {
        struct timeval tv;

        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    while (1)
    {
        n = send(s, buf, sizeof(buf), 0);

        if (n > 0)
        {
            total += n;
            s_stall_sent = total;
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            rt_kprintf("bstall  : send timed out (window), %d bytes so far, retrying\n",
                       total);
            continue;
        }

        rt_kprintf("bstall  : send returned %d errno=%d after %d bytes\n",
                   n, errno, total);
        break;
    }

    rt_kprintf("bstall  : total %d bytes\n", total);
    s_stall_send_done = 1;
    s_stall_stop = 1;
}

int c5_wifi_bstall(void)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    rt_thread_t wd;
    rt_thread_t tx;

    s_stall_stop = 0;
    s_stall_send_done = 0;
    s_stall_sent = 0;
    if (s < 0)
    {
        rt_kprintf("bstall  : socket failed errno=%d\n", errno);
        return -1;
    }

    wd = rt_thread_create("c5bwd", stall_watchdog, RT_NULL, 2048, 5, 10);
    tx = rt_thread_create("c5bsend", stall_sender, &s, 3072, 15, 10);

    if (wd == RT_NULL || tx == RT_NULL)
    {
        rt_kprintf("bstall  : FAIL, cannot create the witness threads\n");
        close(s);
        return -1;
    }

    rt_thread_startup(wd);
    rt_thread_startup(tx);

    return 0;
}

/* `c5rx <host> [port] [seconds]` - does the RECEIVE direction sustain?
 *
 * The whole of 闂佸湱鍘ч崐褰掑箖?-11 has been about the send direction. Sixty-six KB was
 * received by the board earlier (闂?.2: the scan records, the DHCP offers, the
 * echo replies) but never under sustained load, and "receive works" and
 * "receive works at rate" are different claims - especially when every received
 * segment has to travel pbuf -> netif -> tcpip_thread -> the socket's receive
 * mailbox, which is exactly the hand-off the send path is stuck on.
 *
 * The host sends a fixed-size burst and closes; this counts what arrives and
 * how fast. No dereference of the payload beyond a checksum, so a buffer mix-up
 * cannot masquerade as a rate measurement.
 */
int c5_wifi_rx(const char *host, int port, int seconds)
{
    struct sockaddr_in sa;
    int s;
    int n;
    int total = 0;
    rt_uint32_t sum = 0;
    rt_tick_t t0, t1;
    static char buf[1024];

    s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0)
    {
        rt_kprintf("rx      : socket failed errno=%d\n", errno);
        return -1;
    }

    rt_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = inet_addr(host);

    if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) != 0)
    {
        rt_kprintf("rx      : connect %s:%d failed errno=%d\n", host, port, errno);
        close(s);
        return -1;
    }

    rt_kprintf("rx      : connected, reading for %d s ...\n", seconds);

    t0 = rt_tick_get();
    t1 = t0;

    while ((rt_tick_t)(t1 - t0) < (rt_tick_t)(seconds * RT_TICK_PER_SECOND))
    {
        n = recv(s, buf, sizeof(buf), 0);

        if (n > 0)
        {
            int i;

            total += n;
            for (i = 0; i < n; i += 97)
            {
                sum += (rt_uint8_t)buf[i];
            }
            t1 = rt_tick_get();
            continue;
        }

        if (n == 0)
        {
            rt_kprintf("rx      : peer closed after %d bytes\n", total);
            break;
        }

        rt_kprintf("rx      : recv=%d errno=%d after %d bytes\n", n, errno, total);
        break;
    }

    close(s);

    {
        rt_uint32_t ms = (rt_uint32_t)((t1 - t0) * 1000u / RT_TICK_PER_SECOND);
        rt_uint32_t kbps = ms ? (rt_uint32_t)(((rt_uint64_t)total * 8u) / ms) : 0u;

        rt_kprintf("rx      : %d bytes in %u ms = %u kbit/s (checksum 0x%x)\n",
                   total, (unsigned)ms, (unsigned)kbps, (unsigned)sum);

        if (total <= 0)
        {
            rt_kprintf("rx      : FAIL, nothing arrived\n");
            return -1;
        }
    }

    return 0;
}

/* `c5tcpstate` - is TCP's own timer still running during the stall?
 *
 * 闁?4.3 narrowed the transmit stall to one connection whose pending write is
 * never resumed, while ICMP and new TCP connections still work. That leaves one
 * question that decides where to look next:
 *
 *   If TCP timers are still firing, the connection is being serviced by the
 *   stack and the bug is purely in the resume path.
 *   If TCP timers have stopped, every connection is frozen internally and
 *   "new connections still work" is only true because they are short-lived.
 *
 * What a frozen stack would look like, and why it is plausible here: lwIP's
 * tcpip thread drives its timers FROM the mailbox timeout -
 * tcpip_timeouts_mbox_fetch() calls sys_arch_mbox_fetch(..., sleeptime) and runs
 * sys_check_timeouts() only when that fetch reports SYS_ARCH_TIMEOUT
 * (tcpip.c:83-107). sys_arch_mbox_fetch() computes
 * `timeout_ticks = timeout / portTICK_PERIOD_MS` (sys_arch.c:325). With
 * portTICK_PERIOD_MS == 1 and CONFIG_FREERTOS_HZ == 1000 the division is exact,
 * so a sleeptime of 1..249 ms becomes 1..249 ticks and works. Any configuration
 * where portTICK_PERIOD_MS > 1 would round a 1 ms sleep down to 0 ticks, which
 * xQueueReceive turns into a non-blocking poll that always reports EMPTY - and
 * the loop would spin without ever calling sys_check_timeouts().
 *
 * Normal tick counting already proves whether sys_check_timeouts() runs, because
 * sys_check_timeouts() advances lwIP's tcp_ticks. So this command reads tcp_ticks
 * twice, two seconds apart, and compares it with the wall clock. No new
 * instrumentation in lwIP is needed and nothing has to be recompiled to try it.
 */
int c5_wifi_tcpstate(void)
{
    extern u32_t tcp_ticks;                 /* src/core/tcp.c */
    extern struct tcp_pcb *tcp_active_pcbs; /* src/core/tcp.c, via tcp_priv.h */
    extern u8_t tcp_active_pcbs_changed;

    u32_t t0, t1, tick0, tick1;
    rt_uint32_t wall;
    int pcbs = 0;
    struct tcp_pcb *pcb;

    tick0 = tcp_ticks;
    t0 = (u32_t)rt_tick_get();
    rt_thread_mdelay(2000);
    t1 = (u32_t)rt_tick_get();
    tick1 = tcp_ticks;

    wall = (t1 - t0) * 1000u / RT_TICK_PER_SECOND;

    for (pcb = tcp_active_pcbs; pcb != RT_NULL; pcb = pcb->next)
    {
        pcbs++;
    }

    rt_kprintf("tcpstate: tcp_ticks %u -> %u (+%u) over %u ms wall\n",
               (unsigned)tick0, (unsigned)tick1,
               (unsigned)(tick1 - tick0), (unsigned)wall);
    rt_kprintf("tcpstate: active pcbs=%d, changed=%u\n",
               pcbs, (unsigned)tcp_active_pcbs_changed);

    /* Per-connection state, which is what actually decides the question.
     *
     * `unacked` is the number of queued-but-unacknowledged segments. lwIP calls
     * sent_tcp() - the callback that resumes a blocked write - when the peer
     * acknowledges data. So a connection with unacked > 0 and snd_buf drained is
     * one whose resume depends entirely on an ACK; if the ACK arrives at the
     * driver but never reaches this callback, the connection sits here forever
     * and these numbers stop moving. That is exactly the shape 閹?4.3 described,
     * so printing them turns the hypothesis into something measurable. */
    for (pcb = tcp_active_pcbs; pcb != RT_NULL; pcb = pcb->next)
    {
        rt_kprintf("tcpstate: pcb %p st=%d snd_buf=%u snd_wnd=%u qlen=%u "
                   "unacked=%u nrtx=%u persist=%u rto=%u flags=0x%x\n",
                   (void *)pcb, (int)pcb->state,
                   (unsigned)pcb->snd_buf, (unsigned)pcb->snd_wnd,
                   (unsigned)pcb->snd_queuelen,
                   (unsigned)(pcb->unacked ? 1 : 0), (unsigned)pcb->nrtx,
                   (unsigned)pcb->persist_backoff, (unsigned)pcb->rto,
                   (unsigned)pcb->flags);
        rt_kprintf("tcpstate:      snd_nxt=%u lastack=%u wnd=%u snd_wl1=%u\n",
                   (unsigned)pcb->snd_nxt, (unsigned)pcb->lastack,
                   (unsigned)pcb->snd_wnd, (unsigned)pcb->snd_wl1);
    }

    /* TCP_TMR_INTERVAL is 250 ms here, so two seconds of wall clock should
     * advance tcp_ticks by about 8. Half of that (as measured: +4 with one
     * connection live) still means the timer runs; only zero means it does not.
     *
     * The zero case is NOT automatically a fault: CONFIG_LWIP_TIMERS_ONDEMAND=y,
     * so with no active connection lwIP legitimately arms no timers and parks on
     * an infinite mailbox wait. The first version of this command called that
     * "FROZEN" and was wrong, which is why the verdict is now gated on pcbs. */
    if (pcbs == 0)
    {
        rt_kprintf("tcpstate: no active connection, timers parked (ondemand)\n");
        return 0;
    }

    if ((tick1 - tick0) == 0)
    {
        rt_kprintf("tcpstate: FROZEN - sys_check_timeouts() is not running "
                   "while a connection is active\n");
        return -1;
    }

    rt_kprintf("tcpstate: timers alive with %d connection(s)\n", pcbs);
    return 0;
}

/* `c5txstats` - does the Wi-Fi driver accept or reject the frames lwIP hands it?
 *
 * The pcb dump in `c5tcpstate` shows the stuck connection carrying
 * TF_NAGLEMEMERR (0x80) with snd_buf=0 and four segments queued. That flag is
 * only left set when tcp_output() does not reach its clearing path, which is the
 * path taken when the link output fails. So the question is now specific: is the
 * Wi-Fi driver returning an error for these frames?
 *
 * The counters live in components/esp_wifi/src/wifi_netif.c, on the single
 * function every outgoing frame goes through (wifi_transmit ->
 * esp_wifi_internal_tx). Three outcomes are counted separately because they mean
 * different things: OK, NO_MEM (the driver's TX buffer pool is exhausted, which
 * would make this a driver back-pressure problem) and anything else (a real
 * driver error).
 */
int c5_wifi_txstats(void)
{
    extern volatile rt_uint32_t g_c5_wifi_tx_ok;
    extern volatile rt_uint32_t g_c5_wifi_tx_err_nomem;
    extern volatile rt_uint32_t g_c5_wifi_tx_err_other;

    rt_uint32_t ok = g_c5_wifi_tx_ok;
    rt_uint32_t nomem = g_c5_wifi_tx_err_nomem;
    rt_uint32_t other = g_c5_wifi_tx_err_other;

    rt_kprintf("txstats : ok=%u nomem=%u other=%u\n",
               (unsigned)ok, (unsigned)nomem, (unsigned)other);

    if (nomem || other)
    {
        rt_kprintf("txstats : the driver IS rejecting frames "
                   "(nomem=%u other=%u)\n", (unsigned)nomem, (unsigned)other);
        return -1;
    }

    rt_kprintf("txstats : every frame handed to the driver was accepted\n");
    return 0;
}











