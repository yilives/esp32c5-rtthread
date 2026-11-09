/* BLE bring-up for the combined C5 image.
 *
 * Lifted from c5-ble-probe/main/main.c. The controller and the NimBLE host are
 * the only BLE implementation this chip has, so the work of "porting BLE" is
 * underneath this file: the controller's tasks, queues and timers all run on the
 * FreeRTOS-API shim over RT-Thread.
 */

#include <rtthread.h>
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "port_ble.h"

#define BLE_NAME "c5-rtt"

/* ------------------------------------------------------------------ GATT -- */

/* Vendor service, 128-bit. The bytes are in NimBLE's little-endian layout, i.e.
 * BLE_UUID128_INIT takes them least-significant first. The text form is
 * 8ec90001-f315-4f60-9fb8-838830daea50 for the service and ...0002/...0003 for
 * the two characteristics. */
static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x50, 0xea, 0xda, 0x30, 0x88, 0x83, 0xb8, 0x9f,
                     0x60, 0x4f, 0x15, 0xf3, 0x01, 0x00, 0xc9, 0x8e);
static const ble_uuid128_t s_id_uuid =
    BLE_UUID128_INIT(0x50, 0xea, 0xda, 0x30, 0x88, 0x83, 0xb8, 0x9f,
                     0x60, 0x4f, 0x15, 0xf3, 0x02, 0x00, 0xc9, 0x8e);
static const ble_uuid128_t s_cnt_uuid =
    BLE_UUID128_INIT(0x50, 0xea, 0xda, 0x30, 0x88, 0x83, 0xb8, 0x9f,
                     0x60, 0x4f, 0x15, 0xf3, 0x03, 0x00, 0xc9, 0x8e);

static uint8_t s_id_value[4] = { 0x43, 0x35, 0x52, 0x54 };  /* "C5RT" */
static uint16_t s_cnt_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile rt_uint32_t s_notify_count;

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (attr_handle == 0)
    {
        return BLE_ATT_ERR_INVALID_HANDLE;
    }

    switch (ctxt->op)
    {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        if (ble_uuid_cmp(ctxt->chr->uuid, &s_id_uuid.u) == 0)
        {
            return os_mbuf_append(ctxt->om, s_id_value, sizeof(s_id_value)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        if (ble_uuid_cmp(ctxt->chr->uuid, &s_cnt_uuid.u) == 0)
        {
            rt_uint32_t v = s_notify_count;

            return os_mbuf_append(ctxt->om, &v, sizeof(v)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        return BLE_ATT_ERR_UNLIKELY;

    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_svc_def gatt_svr_svcs[] =
{
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                .uuid = &s_id_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid = &s_cnt_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_cnt_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

/* ------------------------------------------------------------------ state -- */

static volatile int s_synced;
static volatile int s_advertising;
static ble_addr_t s_own_addr;
static volatile int s_have_addr;
static char s_state[64] = "down";

static void on_sync(void)
{
    uint8_t addr_type;
    int rc;

    s_synced = 1;

    ble_hs_util_ensure_addr(0);

    rc = ble_hs_id_infer_auto(0, &addr_type);
    if (rc == 0)
    {
        rc = ble_hs_id_copy_addr(addr_type, s_own_addr.val, NULL);
        if (rc == 0)
        {
            s_own_addr.type = addr_type;
            s_have_addr = 1;
            rt_kprintf("ble     : own addr %02x:%02x:%02x:%02x:%02x:%02x\n",
                       s_own_addr.val[5], s_own_addr.val[4], s_own_addr.val[3],
                       s_own_addr.val[2], s_own_addr.val[1], s_own_addr.val[0]);
        }
    }
}

static void on_reset(int reason)
{
    rt_kprintf("ble     : host reset reason=%d\n", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* GAP events for the advertising procedure.
 *
 * A connect must be accepted explicitly (or the connection is dropped) and the
 * notification counter is driven from here rather than from a separate thread:
 * one thread that talks to NimBLE is simpler to reason about than two, and the
 * notify interval follows the connection event, which is the rate a subscriber
 * actually samples at.
 *
 * `adv_started` is re-armed here too: on disconnect NimBLE does NOT resume
 * advertising by itself when the procedure was given an explicit event callback.
 */
static rt_tick_t s_last_notify;

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type)
    {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0)
        {
            s_conn_handle = event->connect.conn_handle;
            rt_kprintf("ble     : CONNECTED handle=%d\n", s_conn_handle);
            rt_snprintf(s_state, sizeof(s_state), "connected handle=%d", s_conn_handle);
        }
        else
        {
            rt_kprintf("ble     : connect failed status=%d, re-advertising\n",
                       event->connect.status);
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            c5_ble_advertise_start();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        rt_kprintf("ble     : DISCONNECTED reason=%d\n", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* Advertising stopped when the connection was accepted; restart it so
         * the board is reachable again without a manual command. */
        c5_ble_advertise_start();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        rt_kprintf("ble     : adv complete reason=%d\n", event->adv_complete.reason);
        s_advertising = 0;
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        rt_kprintf("ble     : subscribe attr=%d notify=%d\n",
                   event->subscribe.attr_handle, event->subscribe.cur_notify);
        if (event->subscribe.attr_handle == s_cnt_handle)
        {
            s_last_notify = 0;      /* send one immediately on subscribe */
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        rt_kprintf("ble     : mtu=%d\n", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

/* Called from the idle hook path by main.c to push one notification per second.
 *
 * Deliberately not its own thread: it is three lines and it has to run only
 * while a subscriber exists. `main.c` calls it from the existing demo thread, so
 * the BLE path shares the same scheduling load the rest of the image has. */
void c5_ble_poll(void)
{
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || s_cnt_handle == 0)
    {
        return;
    }

    if (rt_tick_get() - s_last_notify < RT_TICK_PER_SECOND)
    {
        return;
    }

    s_last_notify = rt_tick_get();
    s_notify_count++;

    {
        struct os_mbuf *om = ble_hs_mbuf_from_flat((const void *)&s_notify_count, sizeof(s_notify_count));

        if (om != RT_NULL)
        {
            int rc = ble_gatts_notify_custom(s_conn_handle, s_cnt_handle, om);

            if (rc != 0 && s_notify_count <= 3)
            {
                rt_kprintf("ble     : notify rc=%d\n", rc);
            }
        }
    }
}

int c5_ble_init(void)
{
    int rc;
    int waited;

    s_synced = 0;
    s_have_addr = 0;

    rc = nvs_flash_init();
    if (rc != 0 && rc != 0x101 /* ESP_ERR_INVALID_STATE */)
    {
        rt_kprintf("ble     : nvs=0x%x\n", (unsigned)rc);
    }

    rc = nimble_port_init();
    if (rc != 0 && rc != 8 /* BLE_HS_EDONE: already initialised */)
    {
        rt_kprintf("ble     : FAIL nimble_port_init=%d\n", rc);
        rt_snprintf(s_state, sizeof(s_state), "init failed %d", rc);
        return -1;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* Register the GATT service HERE - before the host task starts - and not
     * after the sync callback returns.
     *
     * This is the ordering that cost a round: registering after sync printed
     * gatts_count=0 and gatts_add=0, advertised fine, accepted the connection,
     * and then the peer found ZERO characteristics and dropped the link with
     * reason=531. Cause, from ble_hs.c:815: ble_hs_start() calls
     * ble_gatts_start(), which walks the registered services and builds the
     * attribute database - and it runs when the host synchronises, i.e. before
     * on_sync() executes. Services added after that are simply not in the
     * database, and nothing reports an error because adding never failed.
     *
     * ble_svc_gap_init()/ble_svc_gatt_init() return void in this NimBLE version,
     * so they are not assigned to rc. */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    rt_kprintf("ble     : gatts_count=%d\n", rc);
    if (rc != 0)
    {
        rt_snprintf(s_state, sizeof(s_state), "gatt count failed %d", rc);
        return -1;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    rt_kprintf("ble     : gatts_add=%d\n", rc);
    if (rc != 0)
    {
        rt_snprintf(s_state, sizeof(s_state), "gatt add failed %d", rc);
        return -1;
    }

    nimble_port_freertos_init(host_task);

    for (waited = 0; !s_synced && waited < 5000; waited += 100)
    {
        rt_thread_mdelay(100);
    }

    if (!s_synced)
    {
        rt_kprintf("ble     : FAIL, host never synchronised\n");
        rt_snprintf(s_state, sizeof(s_state), "no sync");
        return -1;
    }

    rt_kprintf("ble     : synced after %d ms\n", waited);

    rt_snprintf(s_state, sizeof(s_state), "synced, gatt registered, not advertising");
    return 0;
}

int c5_ble_advertise_start(void)
{
    struct ble_gap_adv_params advp;
    struct ble_hs_adv_fields fields;
    int rc;

    if (!s_synced)
    {
        return -1;
    }

    rt_memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)BLE_NAME;
    fields.name_len = 6;
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0)
    {
        rt_kprintf("ble     : FAIL adv_set_fields=%d\n", rc);
        return -1;
    }

    rt_memset(&advp, 0, sizeof(advp));
    advp.conn_mode = BLE_GAP_CONN_MODE_UND;
    advp.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &advp, ble_gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
    {
        rt_kprintf("ble     : FAIL adv_start=%d\n", rc);
        return -1;
    }

    s_advertising = 1;

    if (s_have_addr)
    {
        rt_snprintf(s_state, sizeof(s_state),
                    "advertising %02x:%02x:%02x:%02x:%02x:%02x",
                    s_own_addr.val[5], s_own_addr.val[4], s_own_addr.val[3],
                    s_own_addr.val[2], s_own_addr.val[1], s_own_addr.val[0]);
    }
    else
    {
        rt_snprintf(s_state, sizeof(s_state), "advertising");
    }

    rt_kprintf("ble     : %s\n", s_state);
    return 0;
}

const char *c5_ble_state(void)
{
    return s_state;
}

