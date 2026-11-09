/*
 * SoftAP for the combined C5 image.
 *
 * The port has been station-only so far: `esp_wifi_set_mode(WIFI_MODE_STA)` was
 * the only mode ever programmed, so "SoftAP works" was an assumption rather than
 * a measurement. This makes it a measurement.
 *
 * ---------------------------------------------------------------------------
 * ONE RADIO, SO THIS IS A MODE SWITCH, NOT AN ADDITION.
 *
 * The C5 has a single 802.11 radio. Starting the AP while the STA is associated
 * tears the STA down; this board cannot be a station and an AP on different
 * channels at the same time. That is a hardware property, not a limitation of
 * this code, and the command says so in its own output rather than leaving the
 * user to discover it when their `c5tcp` starts failing.
 *
 * ---------------------------------------------------------------------------
 * How this is verified.
 *
 * Three things, in increasing order of strength:
 *
 *   1. The driver reports AP_START, which means the beacon timer is armed.
 *   2. The AP has an IP and a running DHCP server - read back from the netif.
 *   3. **A second device sees the SSID in a scan.** That is the only check that
 *      leaves the chip, and it is the one that distinguishes "the driver thinks
 *      it is beaconing" from "beacons are actually on the air". It is done from
 *      the host with `netsh wlan show networks`, not from this board.
 *
 * What is NOT verified here: a full association + DHCP lease to a real client.
 * Doing that from the only available host would drop its own connection to the
 * AP the rest of this port is tested against, so it is stated as untested rather
 * than risked.
 */

#include <rtthread.h>
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_err.h"

#include "port_wifi.h"

/* 2.4 GHz channel 6: the host adapter can see it while associated to a 5 GHz AP,
 * which is what makes the external check possible without moving anything. */
#define C5_AP_CHANNEL       6
#define C5_AP_MAX_STA       4

static volatile int s_ap_started;
static volatile int s_ap_clients;
static volatile int s_ap_stop;

static void c5_ap_events(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base != WIFI_EVENT)
    {
        return;
    }

    if (id == WIFI_EVENT_AP_START)
    {
        s_ap_started = 1;
    }
    else if (id == WIFI_EVENT_AP_STOP)
    {
        s_ap_stop = 1;
    }
    else if (id == WIFI_EVENT_AP_STACONNECTED)
    {
        s_ap_clients++;
    }
    else if (id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        if (s_ap_clients > 0)
        {
            s_ap_clients--;
        }
    }
}

int c5_wifi_softap(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wc;
    esp_netif_t *ap;
    esp_err_t err;
    int waited;

    s_ap_started = 0;
    s_ap_stop = 0;
    s_ap_clients = 0;

    err = nvs_flash_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        rt_kprintf("c5ap    : FAIL, nvs 0x%x\n", (unsigned)err);
        return -1;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        rt_kprintf("c5ap    : netif 0x%x\n", (unsigned)err);
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        rt_kprintf("c5ap    : FAIL, event loop 0x%x\n", (unsigned)err);
        return -1;
    }

    ap = esp_netif_create_default_wifi_ap();
    if (ap == NULL)
    {
        rt_kprintf("c5ap    : FAIL, no default AP netif\n");
        return -1;
    }

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, c5_ap_events, NULL);

    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE)
    {
        rt_kprintf("c5ap    : FAIL, init 0x%x\n", (unsigned)err);
        return -1;
    }

    rt_memset(&wc, 0, sizeof(wc));
    rt_strncpy((char *)wc.ap.ssid, C5_AP_SSID, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len = (uint8_t)rt_strlen(C5_AP_SSID);
    rt_strncpy((char *)wc.ap.password, C5_AP_PASS, sizeof(wc.ap.password) - 1);
    wc.ap.channel = C5_AP_CHANNEL;
    wc.ap.max_connection = C5_AP_MAX_STA;
    /* WPA2_PSK, not WPA3: an open or WPA3-only AP would make the visibility test
     * ambiguous - a device that cannot do SAE would not see it either, and then
     * "not visible" would not mean "not beaconing". */
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    rt_kprintf("c5ap    : mode=0x%x\n", (unsigned)err);

    err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    if (err != ESP_OK)
    {
        rt_kprintf("c5ap    : FAIL, config 0x%x\n", (unsigned)err);
        return -1;
    }

    err = esp_wifi_start();
    rt_kprintf("c5ap    : start=0x%x\n", (unsigned)err);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE)
    {
        return -1;
    }

    for (waited = 0; !s_ap_started && waited < 5000; waited += 100)
    {
        rt_thread_mdelay(100);
    }

    if (!s_ap_started)
    {
        rt_kprintf("c5ap    : FAIL, no AP_START event in 5 s - the beacon timer "
                   "was never armed\n");
        return -1;
    }

    /* Read the AP's own address back from the netif rather than assuming the
     * default 192.168.4.1: the DHCP server binds to whatever the netif holds, and
     * if those two ever disagree every client gets a lease it cannot use. */
    {
        esp_netif_ip_info_t ip;

        if (esp_netif_get_ip_info(ap, &ip) != ESP_OK)
        {
            rt_kprintf("c5ap    : FAIL, no IP on the AP netif\n");
            return -1;
        }

        rt_kprintf("c5ap    : AP_START ssid=\"%s\" ch=%u auth=WPA2 ip=%u.%u.%u.%u\n",
                   C5_AP_SSID, (unsigned)C5_AP_CHANNEL,
                   (unsigned)(ip.ip.addr & 0xff),
                   (unsigned)((ip.ip.addr >> 8) & 0xff),
                   (unsigned)((ip.ip.addr >> 16) & 0xff),
                   (unsigned)((ip.ip.addr >> 24) & 0xff));
    }

    rt_kprintf("c5ap    : the STA association (if any) is now down - one radio, "
               "one channel\n");
    rt_kprintf("c5ap    : check it from another device, e.g. on the host:\n");
    rt_kprintf("            netsh wlan show networks mode=bssid | findstr /i c5-rtt\n");
    rt_kprintf("c5ap    : clients=%d, max=%d\n", s_ap_clients, C5_AP_MAX_STA);
    rt_kprintf("c5ap    : READY (beaconing; no client has associated yet)\n");

    return 0;
}

int c5_wifi_ap_down(void)
{
    esp_err_t err = esp_wifi_stop();

    rt_kprintf("c5ap    : stop=0x%x\n", (unsigned)err);

    /* ESP_ERR_WIFI_NOT_INIT (0x3001) is a success here, not a failure.
     *
     * The AP shares the driver with the STA, so by the time `c5ap down` runs the
     * Wi-Fi stack may already have been torn down - or never initialised, if the
     * command is issued on a fresh boot. Reporting that as an error would make a
     * diagnostic fail on the state it is asking to reach. */
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT)
    {
        return -1;
    }

    /* De-initialise the driver too, not just stop it.
     *
     * esp_wifi_stop() leaves the driver initialised, and the STA path in
     * wifi.c calls esp_wifi_init() again on the next join - which then fails with
     * ESP_ERR_INVALID_STATE (0x103, "WiFi is initialized by esp_wifi_init").
     * Measured exactly that: `c5ap; c5ap down; c5join5` left the STA unable to
     * come back until a reboot.
     *
     * Deinit is what makes the AP/STA switch a round trip instead of a one-way
     * trip, and this is the mirror image of the "one radio" caveat: the two modes
     * cannot coexist, so handing the driver back cleanly is part of the contract.
     */
    {
        esp_err_t d = esp_wifi_deinit();

        rt_kprintf("c5ap    : deinit=0x%x\n", (unsigned)d);
        if (d != ESP_OK && d != ESP_ERR_WIFI_NOT_INIT)
        {
            return -1;
        }
    }

    rt_kprintf("c5ap    : AP is down and the driver is released; "
               "`c5join5` can bring the STA up again\n");
    return 0;
}


