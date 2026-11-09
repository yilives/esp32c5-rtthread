#ifndef PORT_WIFI_H
#define PORT_WIFI_H

#include <rtthread.h>
#include "port_adc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Join the AP this bring-up was developed against (5 GHz ch 149). */
int c5_wifi_join(void);

/* Join the 2.4 GHz and 5 GHz halves of the dual-band AP pair. Same credentials,
 * different SSID - the band is spelled into the name by the AP itself. */
int c5_wifi_join_alt(void);
int c5_wifi_join_b5(void);

/* Full dual-band scan, sorted by band, with the raw SSID bytes of every AP. */
int c5_wifi_bands(void);

/* SoftAP. There is one radio, so this takes the STA down: the board cannot be a
 * station and an AP on different channels at the same time. */
#define C5_AP_SSID   "c5-rtt-ap"
/* Change this. A SoftAP with a published password is not a secret, and this
 * one is a placeholder so the file is not a credential store. */
#define C5_AP_PASS   "c5-rtt-ap-password"

int c5_wifi_softap(void);
int c5_wifi_ap_down(void);

int c5_wifi_tcp(const char *host, int port, char *out, rt_size_t out_len);
int c5_wifi_tput(const char *host, int port, int seconds);
int c5_wifi_rx(const char *host, int port, int seconds);
const char *c5_wifi_state(void);

/* Diagnostics used while chasing the transmit stall (report sections 11-15). */
int c5_wifi_bstall(void);
int c5_wifi_tcpstate(void);
int c5_wifi_txstats(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_WIFI_H */


