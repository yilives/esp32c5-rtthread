#ifndef PORT_BLE_H
#define PORT_BLE_H

#ifdef __cplusplus
extern "C" {
#endif

int c5_ble_init(void);
int c5_ble_advertise_start(void);
const char *c5_ble_state(void);

/* Push one notification per second while a subscriber is connected. */
void c5_ble_poll(void);

#ifdef __cplusplus
}
#endif

#endif
