#ifndef PORT_ULP_H
#define PORT_ULP_H

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the LP core program and verify it ran, using only evidence the HP core
 * can observe. `loops` is written into shared memory before the core starts. */
int c5_ulp_selftest(rt_uint32_t loops);

#ifdef __cplusplus
}
#endif

#endif /* PORT_ULP_H */
