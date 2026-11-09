# ESP32-C5 + RT-Thread bring-up report

Board: the ESP32-C5 module on **COM21**. Everything below was measured on that
board; nothing is inferred from a datasheet or from the S31 port unless it says so.

> **Rebuild note.** This file replaces an earlier ~1700-line version that was
> destroyed by a bad scripted write on 2026-11-05 (a Python `io.open(...,
> newline="\\n")` call with an illegal newline value truncated it to zero bytes;
> no backup, no git history, no shadow copy). Everything here is rebuilt from the
> session's own measurements, which are quoted verbatim. Earlier rounds are
> necessarily condensed; the later ones are close to the original text.

## 1. The result first

RT-Thread 5.4.0 runs on the ESP32-C5. The image boots from flash, the 1 kHz tick
keeps time, two threads preempt each other, msh answers on UART0 with
interrupt-driven input, the `pin` device works including per-pin interrupts, and a
flash erase/write/read cycle survives the cache-off window.

```
RT-Thread 5.4.0 on ESP32-C5
tick=1000 Hz, cpus=1, msh on UART0
msh >
```

```
msh > c5rt
tick    : 70 -> 3070, delta 3000 (expected ~3000 at 1000 Hz)
threads : ta 4 -> 7, tb 3 -> 5
heap    : total=49056 used=9424 max=13200
pin     : mode output, write high, read high
result  : PASS
msh > c5flash
flash   : 16384 KB, scratch at 0x300000
flash   : 0 mismatched byte(s), tick=6144
flash   : PASS
msh > c5pinirq
gpio: irq on line 7 (allocated, source 31)
pinirq  : 5 rising edges, 5 interrupt(s)
pinirq  : PASS
```

### 1.1 The final state, capability by capability

| Capability | Result | Evidence |
| --- | --- | --- |
| RT-Thread kernel, tick, threads, msh | PASS | §1 |
| Flash write/read across the cache-off window | PASS, 0 mismatched bytes | §1 |
| `pin` device with interrupts | PASS, 5 edges / 5 interrupts | §1 |
| Wi-Fi scan | PASS, 24-33 APs per run | §8.2 |
| Wi-Fi associate + DHCP | PASS | §8.2 |
| TCP round trip, both directions | PASS | §19 |
| TCP sustained send, 32 KB window | **19213 kbit/s** (5 GHz), **10426 kbit/s** (2.4 GHz) | §15.1 |
| TCP sustained receive | **10399 kbit/s** (5 GHz), **2077 kbit/s** (2.4 GHz) | §18.2, §15.1 |
| BLE advertising | PASS, witnessed by an external radio | §8.3 |
| BLE GATT connect / read / notify | PASS, 7 notifications, link held 40 s with 0 drops | §9.1, §19 |
| Wi-Fi + BLE simultaneously | PASS, both transferring | §9.4, §19 |
| Driver TX rejections at 19 Mbit/s | **0** | §15.1 |
| LEDC PWM (`pwm0`) with register read-back | PASS at 1 / 20k / 100 Hz | §15.4 |
| I2C master (`i2c0`) | bus driven, 112/112 addresses NACKed in 13 ms | §16 |
| SAR ADC (`adc1`) | PASS, all 6 channels convert, raw codes with real noise | §17 |
| GPSPI master (`spi0`) | **PASS**: in-chip loopback byte-identical, no jumper needed | §17k |
| TWAI / CAN (`c5twai`) | PASS in loopback at 125k/250k/500k/1M; analog layer unverified | §17b.2 |
| RMT (`c5rmt`) | **PASS**, TX+RX round trip: 19 symbols captured, mean half 4761 ticks = the request | §17o |
| PCNT (`pcnt0`) | **PASS**, exact count at 100/500/1000 edges, via RT-Thread device | §17d |
| MCPWM (`c5mcpwm`) | **PASS**, frequency + duty measured at the pad across 10 rate/duty pairs | §17e |
| Temperature sensor (`c5tsens`) | **PASS**, plausible + range register proven effective; **uncalibrated** | §17f |
| PSRAM (8 MB) | **enabled**, 1 MB read/write byte-exact, in the IDF heap | §17g |
| LP core (ULP RISC-V) | **runs**, counter + checksum exact at 100/2k/5k/20k loops, restartable | §17h |
| SoftAP (`c5ap`) | **beaconing**, seen from the host at 14:c1:9f:8a:11:39 ch=6 WPA2; no client test | §17i |
| Pin registry (`c5pins`) | **PASS**, no double claim, no test on a fixed-function pad | §17j |
| USB Serial/JTAG (`c5usj`) | **device side only**: PHY pads powered, FIFO coherent; no host, so no enumeration | §17l |
| AHB GDMA (`c5gdma`) | **PASS**: mem-to-mem copy byte-exact at 1/16/128/256 B, no overrun | §17n |
| PARLIO (`c5parlio`) | **PASS**: GDMA feeds it, the unit reads every bit out of its FIFO, PCNT sees the clock run | §17q |

Seven device drivers are registered: `uart0`, `pin`, `pwm0`, `i2c0`, `adc1` and `pcnt0` (all six verified), plus `spi0` (registered, data path unverified).

The one defect that took seven rounds to find was not in the scheduler, not in lwIP
and not in the shim: the port had reserved 128 KB of the C5's 320 KB of SRAM for
RT-Thread's heap, and the Wi-Fi driver's 32 dynamic TX buffers — ~1.6 KB each,
allocated from what was left — no longer fit. `esp_wifi_internal_tx()` returned
`ESP_ERR_NO_MEM`, lwIP latched `TF_NAGLEMEMERR` on the pcb, and a blocking `send()`
froze with no error reported anywhere. §14 has the counters that proved it; the
64 KB rebalance fixed it.

## 2. What the board actually is

| Item | Value | How |
| --- | --- | --- |
| Chip | ESP32-C5HR8, revision v1.2 | `esptool chip-id` |
| Cores | Single RISC-V core + LP core, 240 MHz | same |
| Flash | 16 MB, mfr 0x68 device 0x4018 | `esptool flash-id` |
| PSRAM | 8 MB in-package (the "R8") — **enabled and verified** (§17g) | soc_caps + measured |
| XTAL | 48 MHz, auto-detected (`CONFIG_XTAL_FREQ_AUTO`) | boot log |
| MAC | 14:c1:9f:8a:11:38 (STA), ...:3a (BLE) | `esp_wifi_get_mac` |
| ISA | `rv32imac_zicsr_zifencei_zaamo_zalrsc` | `misa` = 0x40103105 |
| FPU | **none** | §7.1 |
| Interrupts | CLIC with hardware nesting | `SOC_INT_CLIC_SUPPORTED` |
| GPIOs | 29 | `SOC_GPIO_PIN_COUNT` |

Flash layout: bootloader `0x2000`, partition table `0x8000`, app `0x10000`, 2 MB
factory partition.

## 3. Which port shape was used, and why

RT-Thread replaces the FreeRTOS kernel *inside* ESP-IDF rather than running beside
it. The Wi-Fi and BLE blobs are IDF-internal binaries that call FreeRTOS APIs
directly, so a side-by-side arrangement would mean two schedulers sharing one core
and one interrupt controller. Instead `components/rtthread` provides the kernel and
a FreeRTOS-API compatibility shim (`port/freertos/ft_*.c`), so `esp_wifi`,
`esp_netif`, `lwip`, `esp_timer`, `esp_event` and NimBLE link and run unmodified.

Options: `CONFIG_IDF_RTOS_RTTHREAD=y`, `CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM=y`,
`CONFIG_FREERTOS_UNICORE=y`.

## 4. What went wrong on the way

### 4.1 `pulse_encoder.c`: the S31 device list does not compile here

The C5 initially reused the S31 `serial_srcs` list, which compiles files whose
`RT_USING_*` class macro is off:

```
pulse_encoder.c: error: invalid use of undefined type 'struct rt_pulse_encoder_device'
pulse_encoder.c: error: 'PULSE_ENCODER_CMD_ENABLE' undeclared
```

Fixed by giving the C5 its own short `serial_srcs` list, adding a file back only
when its driver exists and its `RT_USING_*` is on.

### 4.2 `spi_flash_other_core_stop` undefined: the stubs were in the wrong branch

The weak stubs in `cache_utils.c` were inside the `#if CONFIG_FREERTOS_UNICORE`
block's `#else`. Single-core needs them too. Moved.

### 4.3 `gpio.c` compiled, and then was not in the image

`gpio.c` produced `gpio.c.obj` and the linked image contained neither
`rt_hw_pin_init` nor the `pin` command — with a clean build and no warning. Cause:
`INIT_BOARD_EXPORT()` writes a *data* entry (`.rti_fn`), nothing references the
member by symbol, so `--gc-sections` drops it. Fixed with `-u rt_hw_pin_init`. This
trap has recurred three more times (§15.4 pwm, §16 i2c, §17 adc) and is now the
standard treatment for every exported init on this target.

### 4.4 The console worked first try

`port/esp32c5/serial.c` registers `uart0` through the UART LL and carries
`rt_hw_console_output()` via the ROM putc, which is what makes the first
`rt_kprintf` before the scheduler starts work at all.

## 5. Decisions that are deliberate, not oversights

1. **No `RT_USING_SMP`** — the C5 is single-core; `startup.c` `#error`s if IDF is
   built to start multiple cores without it.
2. **No `ARCH_RISCV_FPU`** — the part has no FPU (§7.1); defining it would make
   `vectors_rtthread.S` build an FP frame for registers that do not exist.
3. **`rt_irq_nest_slot` / `rt_irq_stack_top` are non-const in `.data`**, not
   `.rodata`: the trap handler indexes them by mhartid with the flash cache off.
4. **Serial v1, not v2** — v2 needs a device model this port does not have.
5. **LL drivers, not `esp_driver_*`** — the IDF driver layer sits on FreeRTOS
   primitives, so driving the LL directly is smaller and auditable.

## 6. Reproduce

```
powershell -File tools\build.ps1 -Project c5-rtt-full build
cd c5-rtt-full\build
esptool --chip esp32c5 -p COM21 -b 460800 write-flash --flash-mode dio \
        --flash-freq 80m --flash-size 16MB \
        0x2000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
        0x10000 c5_rtt_full.bin

python tools\c5seq.py COM21 --wait 25 "c5all 0" "c5iperf <host-ip> 4444 5" "c5stat"
```

## 7. Two follow-up questions, answered from the silicon

### 7.1 "Does the C5 have an FPU?" — No, measured three ways

| Method | Result |
| --- | --- |
| `misa` CSR | `0x40103105` — the F bit is clear |
| `mstatus` | `0x00000088`, `FS` field = 0 |
| `-march` | `rv32imac_zicsr_zifencei_zaamo_zalrsc` — no `f` |
| ROM contents | `__addsf3`, `__mulsf3`, `__divsf3` present; the S31/P4 ROMs have none |
| Compiler test | `fadd.s` is emitted only with `-march=rv32imafc_...` |

### 7.2 "Is Wi-Fi running?" — originally no, then yes

The original §7.2 recorded Wi-Fi blocked inside `esp_phy_enable()` at
`locks.c:150`. That is history: §8.1 has the fix and §8.2 the first working scan.

## 8. Wi-Fi and BLE run — the measurements, and the four defects between

### 8.1 The fix that unblocked both radios

`components/esp_libc/src/locks.c` used `!xPortCanYield()` as its "am I in an ISR?"
test. On RT-Thread that predicate is false in three different states — in an ISR,
with the flash cache off, **and while RT-Thread's scheduler lock is held**. IDF
critical sections take that lock through `vPortEnterCritical() ->
rt_enter_critical()`, so every IDF critical section looked like an ISR to this
file. That is why `esp_phy_enable()` aborted: it takes a critical section, then a
libc mutex inside it, the mutex call went down the ISR path, and `locks.c:150`
fired "Tried to block on mutex from ISR".

The test now calls `xPortInIsrContext()`, which answers the question actually being
asked and already has the correct RT-Thread definition
(`rt_interrupt_get_nest() != 0`). `xPortCanYield()` itself is unchanged, which is
right for its other callers because they really do mean "can I yield".

### 8.2 Wi-Fi: scan, associate, DHCP, TCP

```
msh > c5wifi
scan    : 0x0
events  : 3, scan_done=1 after 12000 ms
aps     : 33
records : rc=0x0 got=1
ap0     : a2:d0:71:c7:0e:f5 ch=149 rssi=-17
c5wifi  : PASS (scan + decode)

msh > c5tcp 192.168.95.53 3333
tcp     : connected to 192.168.95.53:3333
tcp     : sent 12 byte(s)
tcp     : received 12 byte(s): pong-from-pc
# and on the host: server got: ping-from-c5
```

Four separate layers: the RF chain (33 APs, a real record), the link (association),
the IP layer (DHCP lease), and the socket layer.

### 8.3 BLE: advertising witnessed externally, GATT read and notified

```
# on the PC, tools/blescan.py while the board runs c5adv 25:
HIT 14:C1:9F:8A:11:3A -46 c5-rtt
RESULT MATCH

# on the PC, tools/blegatt.py:
svc 8ec90001-f315-4f60-9fb8-838830daea50
    chr 8ec90002-... props=['read']
    chr 8ec90003-... props=['notify', 'read']
read 8ec90002 = b'C5RT'
notifications: 6 [3, 4, 5, 6, 7, 8]
RESULT: PASS (notifications=6)
```

A one-radio controller **cannot hear its own advertisements**, and the first test
pretended it could. Measured: a 9 s scan heard 769 other advertisements and none of
its own address. So the proof came from outside the chip.

### 8.4 Wi-Fi and BLE together, on one C5

```
step1   : ble_init=0            (controller + host + GATT, 3 services)
step2   : wifi_join=0           (associated, 192.168.95.71)
step3   : ble_advertise=0
c5all   : PASS (Wi-Fi associated, BLE advertising)
tcp     : received 12 byte(s): pong-from-pc      <- Wi-Fi data plane, live
notifications: 6 [3, 4, 5, 6, 7, 8]              <- BLE GATT notify, live
```

### 8.5 Four more defects found on the way

**a. `esp_wifi_scan_get_ap_records(&n, &ap)` with one record and `n == 33`.** The
API takes the buffer capacity in the same word it writes the count back, so it
copied 33 records over the caller's frame. The output looked perfect, then:

```
Instruction access fault, MEPC 0x0047355e, MTVAL 0x0047355e
```

The port was innocent; the test overwrote its own stack. Fixed by passing
`want = 1`. Worth recording because the failure arrives *after* correct output.

**b. `xPortCanYield()` is not `!xPortInIsrContext()`.** §8.1.

**c. NimBLE GATT services registered after `on_sync()` are not in the database.**
`ble_hs_start()` calls `ble_gatts_start()` when the host synchronises
(`ble_hs.c:815`), before the sync callback runs. Registering afterwards printed
`gatts_count=0 gatts_add=0` — no error — advertised fine, accepted the connection,
and the peer found zero characteristics and dropped the link with reason=531.

**d. `ble_svc_gap_init()` returns void in this NimBLE version.** Assigning it to an
`int` is `invalid use of void expression`.

### 8.6 Two things that looked like port bugs and are not

**Association needs a retry, on stock FreeRTOS too.** A cold `esp_wifi_connect()`
against this dual-band AP returns `reason=202` (AUTH_FAIL) about one time in three.
The FreeRTOS control image (`c5-stock-wifi/`) reproduces it with the same
credentials, so it is the AP. The port scans for the SSID, pins the strongest BSSID
and its channel, then connects, and retries up to twice.

**The 8 s scan timeout was never enough.** The stock-FreeRTOS control measured
**12197 ms** for the same dual-band scan.

## 9. Round 2: sustained load

### 9.1 BLE holds a connection for 40 s, no drops

```
  10 s: connected=True notifications=10 last=11
  20 s: connected=True notifications=20 last=21
  30 s: connected=True notifications=30 last=31
  40 s: connected=True notifications=40 last=41
notifications=40 expected>=32 still_connected=True drops_during_run=0
RESULT: PASS
```

The first version of this script reported FAIL on a run that had done exactly the
above, because it read the disconnect counter *after* closing the client itself.

### 9.2 The blocking-send stall

`c5iperf` originally used blocking `send()`:

```
tput    : sending for 5 s ...
(no further output; the shell stops answering)
# host sink: 22528 bytes, then nothing for the remaining 60 s
```

After 20-25 KB the TCP window closes and the blocking `send()` never returns. With
a non-blocking socket the same measurement completes: 204448 bytes in 5 s =
327 kbit/s. §14 is where this was eventually explained.

### 9.3 Priority inversion ruled out by `list thread`

`tcpip` runs at RT-Thread priority 12, higher than `tshell` at 20, so it can always
run. It was not starvation.

### 9.4 Wi-Fi and BLE under simultaneous load

```
ble     : CONNECTED handle=0
tput    : sending for 5 s ...
ble     : subscribe attr=18 notify=1
tput    : 37024 bytes in 5000 ms = 59 kbit/s
notifications=20 drops_during_run=0
```

Throughput drops from 327 to 59 kbit/s while BLE is connected — what a single
shared radio should do. Both keep working, neither drops.

## 10. Round 3: the stall, diagnosed as far as it got

### 10.1 Why a witness was needed

The shell thread IS the blocked one, so the board cannot report its own state while
stuck. `c5bstall` moves the blocking `send()` onto its own thread and keeps a
higher-priority watchdog alive to dump RT-Thread's thread records every 2 s.

### 10.2 What it shows

```
bstall  : --- after 2 s, send_done=0 ---
bstall  : crit=0 irq_off=0
bstall  : c5bsend  suspend prio=15 left_tick=10 err=9 sent=12288
bstall  : tcpip    ready   prio=12 left_tick=6  err=-2 sent=12288
... repeats for 20 s with `sent` never moving past 12288 ...
```

Three facts: `crit=0` and `irq_off=0` throughout (not a locked scheduler, not
masked interrupts); the tcpip thread is at priority 12 and `ready` at times, so it
is being scheduled; yet `sent` does not advance. At this point the defect was
blamed on the lwIP-to-shim hand-off — wrongly, as rounds 4-7 eliminated.

## 11. Round 4: three hypotheses tested, and the asymmetry

### 11.1 Core locking: measured 9x worse (later withdrawn, §15.2)

```
tput    : 22624 bytes in 5000 ms = 36 kbit/s     (was 327 without it)
```

### 11.2 Bigger windows: measured worse (later withdrawn, §15.1)

```
tput    : 19456 bytes in 5000 ms = 31 kbit/s
sink    : 2464 bytes received in 60 s
```

Both of these ran while the driver was starved of heap (§14); neither tested what
it claimed to test.

### 11.3 The decisive experiment: receive sustains, send does not

```
# host: sent 401408 bytes in 1.45 s = 2211 kbit/s
rx      : 401408 bytes in 1830 ms = 1754 kbit/s (checksum 0x8674e)
```

**1.75 Mbit/s inbound, zero loss, checksummed.** The radio, the PHY and the 5 GHz
link are fine; `recv()` works at rate; the defect is in the transmit path between
lwIP and the Wi-Fi driver.

### 11.4 The transmit path waits on a tick, not on a hand-off

| back-off after EWOULDBLOCK | bytes in 5 s | rate | EWOULDBLOCK returns |
| --- | --- | --- | --- |
| `rt_thread_mdelay(1)` | 204448 | 327 kbit/s | not counted then |
| `rt_thread_yield()` | 6784 | 10 kbit/s | **94904** |
| `rt_thread_mdelay(1)`, counted | 33728 | 53 kbit/s | 4986 |

## 12. Round 5: the control works, and it convicts the port

### 12.1 The control was broken by a netif-ordering bug — found in a panic

The stock-FreeRTOS control created its default STA netif *after*
`esp_wifi_start()`:

```
stockwifi: CONNECTED (t=25043 ms)
E (25376) esp_netif_lwip: dhcp client start failed
stockwifi: got_ip=0 disconn=1 reason=202 after 45000 ms
Guru Meditation Error: Core  0 panic'ed (Instruction access fault).
MEPC    : 0x00000000  RA : 0x4208ac56
```

Association succeeds, DHCP has no netif to attach to, and the board jumps to
address zero. Two lessons: "associated but no DHCP lease" reads exactly like an AP
fault (it was blamed on the AP twice), and the netif must be created *before*
`esp_wifi_start()`.

### 12.2 Both images, same AP, same sink

```
stockwifi: [blocking] sent 5894144 bytes in 10000 ms      <- 4.7 Mbit/s
stockwifi: [nonblock] sent 137824 bytes in 5000 ms, 4978 block(s) = 220 kbit/s
# host sink: received 5894144 bytes
```

versus, on the port:

```
bstall  : send timed out (window), 6784 bytes so far, retrying
# host sink: received 1024 bytes
```

**5.89 MB versus 6.8 KB.** The port moves 0.1% of what stock FreeRTOS moves with
the same API call. This is a defect in the port, not IDF's behaviour.

### 12.3 What this round ruled out

Not IDF, not the radio (5.89 MB from the control minutes apart); not a lwIP
configuration (both experiments made it worse); not the shell thread; not
scheduler starvation.

## 13. Round 6: two more wrong predicates, and a real throughput gain

### 13.1 `is_from_isr_wrapper` had the same conflation `locks.c` had

`components/esp_wifi/esp32c5/esp_adapter.c` fed the driver's `_is_from_isr` hook
from `!xPortCanYield()` — the identical mistake as §8.1, so every driver call made
with the scheduler lock held was reported as coming from an ISR. Fixed to
`xPortInIsrContext()`. **It did not fix the transmit stall** — the blocking send
still stopped at 6784 bytes — but it is independently wrong and is left fixed.

### 13.2 Power save was on, and it was costing packets

```
before:  Packets: Sent = 8, Received = 7, Lost = 1 (12% loss)
         Minimum = 41ms, Maximum = 129ms, Average = 65ms
after:   Packets: Sent = 8, Received = 8, Lost = 0 (0% loss)
         Minimum = 13ms, Maximum = 55ms, Average = 31ms
```

on an AP this host reaches in under 2 ms. Throughput moved with it: 52 -> 86 kbit/s.
`c5_wifi_join()` now calls `esp_wifi_set_ps(WIFI_PS_NONE)` after `esp_wifi_start()`.

### 13.3 The stall is per-connection, not stack-wide

ICMP answers during the stall (2 of 6 lost, two replies in ~5 ms — faster than the
idle average), and a **second TCP connection connects and completes**:

```
3333 got: ping-from-c5
tcp     : connected to 192.168.95.53:3333
tcp     : received 12 byte(s): pong-from-pc
```

So `tcpip` is not wedged and the driver is not wedged. One specific connection's
pending write is never resumed.

## 14. Round 7: the stall is fixed — it was heap, not scheduling

### 14.1 The counter that ended the search

`wifi_netif.c`'s `wifi_transmit()` is the single function every outgoing frame
passes through:

```
before:  txstats : ok=11  nomem=0   other=0
after:   txstats : ok=25  nomem=100 other=0
```

`ESP_ERR_NO_MEM`. lwIP turns that into `ERR_MEM` (`ethernetif.c:99`),
`tcp_output_segment()` fails, `tcp_output()` returns before
`tcp_clear_flags(pcb, TF_NAGLEMEMERR)` at `tcp_out.c:1422`, and the pcb keeps
`TF_NAGLEMEMERR`. Exactly what `c5tcpstate` had been printing:

```
pcb 0x4085a810 st=4 snd_buf=0 snd_wnd=63776 qlen=4 unacked=0 nrtx=4 flags=0x80
```

`flags=0x80` is `TF_NAGLEMEMERR` (`tcp.h:259`). The dump had been pointing at the
answer since round 6 and was read as a symptom.

### 14.2 Why the driver ran out of memory

The image had reserved `CONFIG_IDF_RTTHREAD_HEAP_SIZE=131072` and
`CONFIG_IDF_RTTHREAD_INTERRUPT_STACK_SIZE=24576`, leaving the IDF heap at
**23.7 KB** with both stacks up. The driver's TX buffers are
`CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM=32`, ~1.6 KB each, allocated from that same
heap at run time. 32 of them do not fit in 23.7 KB, so under sustained load
`esp_wifi_internal_tx()` started returning `ESP_ERR_NO_MEM` — and a *blocking*
`send()` is the workload that keeps all 32 outstanding at once.

64 KB returned to the IDF heap:

```
# before:  tput :   32704 bytes =      52 kbit/s, 4987 window stall(s), nomem=100
# after:   tput : 1588800 bytes = 2542 kbit/s, 4568 window stall(s), nomem=0
# blocking path, 10 s: bstall : total 19387392 bytes (19 MB, no stall at all)
```

`idf-heap: free` went from 23.7 KB to 88.9 KB.

### 14.3 What this retroactively explains

* §9.2's stall after 12-30 KB: when 32 TX buffers have been handed out.
* §11.4's "yield is 32x worse than a 1 ms sleep".
* §11.2's "bigger window is worse": more buffers outstanding, shortage sooner.
* §13.1's `is_from_isr` fix changing nothing.

### 14.4 The fix, and how to size it

`CONFIG_IDF_RTTHREAD_HEAP_SIZE` 131072 -> 65536,
`CONFIG_IDF_RTTHREAD_INTERRUPT_STACK_SIZE` 24576 -> 16384. RT-Thread's heap now
reports `used=51472 of 65440` with both stacks up.

**The rule: the RT-Thread heap reservation is taken out of the Wi-Fi driver's
buffer pool.** Growing it past ~64 KB on a C5 with Wi-Fi and BLE both enabled
reintroduces this stall — and it presents as a hang, not as an allocation failure.

### 14.5 Two wrong verdicts of mine, recorded

1. `c5tcpstate`'s first version reported "FROZEN" on a healthy idle board —
   `CONFIG_LWIP_TIMERS_ONDEMAND=y` means lwIP legitimately arms no timers with no
   connection open.
2. `c5txstats` first read `ok=0 nomem=0` after 18 KB because the counters were only
   on `wifi_transmit()`; this target has `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` off,
   so the netif actually calls `wifi_transmit_wrap()`.

## 15. Round 8: the window was the cap, and two debts paid

### 15.1 The window, retested now that the driver is not starved

| Measurement | 5760 B window | 32768 B window |
| --- | --- | --- |
| 5 GHz send | 8864 kbit/s | **19213 kbit/s** |
| 5 GHz receive | 7200 kbit/s | **10399 kbit/s** |
| 2.4 GHz send | 4769 kbit/s | **10426 kbit/s** |
| Driver TX rejections | 0 | 0 |

```
tput    : 12008640 bytes in 5000 ms = 19213 kbit/s, 1658 window stall(s)
txstats : ok=8472 nomem=0 other=0
```

**19 Mbit/s is not a Wi-Fi 6 number.** The link is 80 MHz VHT that Windows reports
as 866 Mbit/s; the ceiling here is a single-threaded non-blocking socket loop with
a 32 KB window.

### 15.2 `LWIP_TCPIP_CORE_LOCKING`: old verdict withdrawn

| | off | on |
| --- | --- | --- |
| 5 GHz send | 19213 kbit/s | 19562 kbit/s |
| 5 GHz receive | 10399 kbit/s | 10040-12225 kbit/s |

Neutral within run-to-run noise, **not** 9x worse. Left off.

### 15.3 `bt.c:871`: the third wrong predicate, fixed

The Bluetooth controller's `is_in_isr_wrapper()` had the same
`return !xPortCanYield();`. Fixed to `xPortInIsrContext()`. BLE re-verified: 20
notifications over a 20 s hold, 0 drops, `RESULT: PASS`.

### 15.4 A second device driver: LEDC as `pwm0`

Four things had to change from the S31 file:

**a. `CONFIG_XTAL_FREQ` is 0 on this board** (`CONFIG_XTAL_FREQ_AUTO=y`):

```
c5pwm: 1000000 ns period (about 1000 Hz) is not reachable at resolution 1
```

Replaced with `esp_clk_xtal_freq()` — which returns **Hz**, not MHz; the first
attempt multiplied by 1000000 and produced a 48 GHz source.

**b. An off-by-one in the resolution search**, inherited from the S31: the loop
tested its condition after incrementing. On the S31 the 14-bit ceiling bound the
loop first; the C5 has `SOC_LEDC_TIMER_BIT_WIDTH` 20, so the bug became reachable.

**c. The LL getters return through a pointer on this chip.**

**d. `duty_r` does not read back** — 0 even 3 ms after enable while res and div are
correct. Documented rather than asserted on: the verdict rests on res and div, which
determine the frequency.

```
c5pwm   : gpio 3, 1000 Hz, 50% -> timer 0 res=15 div=375 duty=0
c5pwm   : wanted res=15 div=375 duty=16384
c5pwm   : PASS
```

Two build notes: `esp_hal_ledc` in `PRIV_REQUIRES` does not put `hal/ledc_ll.h` on
the compile line (linking `idf::esp_hal_ledc` does), and `rt_hw_pwm_init()` is
dropped by `--gc-sections` without `-u rt_hw_pwm_init`.

## 16. Round 9: I2C master as `i2c0`

One bus on IO8 (SCL) / IO9 (SDA) at 100 kHz. Four defects, three with the same
symptom:

```
i2cscan: 0 device(s) on i2c0 in 744 ms (nack=0 timeout=112 other=0)
i2cscan: FAIL, 112 address(es) timed out
```

**1. The controller clock was never enabled.** `i2c_ll_enable_bus_clock` and
`i2c_ll_enable_controller_clock` are separate gates.

**2. `int_status` is the masked view, so it was always zero.** This driver polls
and never enables an interrupt, so `int_ena` is 0. A polling driver must read
`hw->int_raw` (`*_int_raw`, not `*_int_st`).

**3. A zero-length WRITE command never retires.**

```
i2c     : timeout dump ctr=0x00000010 status=0x00000000 scl_low=240 scl_high=122
i2c     : cmd[0]=0x80003000    RESTART, done=1
i2c     : cmd[1]=0x80000901    WRITE 1 byte (address), done=1
i2c     : cmd[2]=0x80000900    WRITE 0 bytes, done=0   <- stuck here
i2c     : cmd[3]=0x00001000    STOP, done=0
i2c     : cmd[4]=0x00002000    END,  done=0
```

`done=1` on the first two also proves the pads, clock and timing were fine.

**4. The interrupt status bits sit at different positions than on the S31.** There,
bit 1 is NACK and bit 5 is timeout; on the C5 those are the TX FIFO watermark and
arbitration-lost, while NACK is bit 10 and timeouts are bits 13/14.

**Result:**

```
i2cscan: probing i2c0 at 100000 Hz (SCL=IO8 SDA=IO9)
i2cscan: idle level SCL=1 SDA=1 (both 1 = bus released)
i2cscan: 0 device(s) on i2c0 in 13 ms (nack=112 timeout=0 other=0)
```

13 ms is the wire time, and `nack=112 timeout=0` means **the controller drove the
bus and every address was clocked out and NACKed**. Without that split, `0
device(s)` would read as success even on a bus that never moved. No slave is
attached, so a real read/write is **not** verified.

## 17. Round 10: SAR ADC as `adc1`

```
c5adc   : unit 1, 6 channel(s), 12-bit, atten 12 dB, no calib
c5adc   : ch0 (GPIO1) raw=2617 then 2602, max=4095  (LP_UART pad)
c5adc   : ch1 (GPIO2) raw=2951 then 2929, max=4095  (LP_UART pad)
c5adc   : ch2 (GPIO3) raw=3060 then 3035, max=4095  (GPIO3 is the PWM self-test pin)
c5adc   : ch3 (GPIO4) raw=4095 then 4095, max=4095
c5adc   : ch4 (GPIO5) raw=2949 then 2928, max=4095
c5adc   : ch5 (GPIO6) raw=3171 then 3145, max=4095
c5adc   : PASS (all 6 channels converted)
```

The floating channels read roughly rail-adjacent at 12 dB attenuation, which is what
an unconnected pad with the input buffer disabled does; channel 3 — the pad the PWM
self test had been driving — reads full scale. Repeated runs differ in the last
digits (2617/2602, 2874/2855), which is real SAR noise.

**Two defects, both found by bisection after four wrong guesses:**

**1. `ADC_DIGI_CLK_SRC_DEFAULT` is PLL_F80M and nothing enables the PLL.** The C5
has no RTC controller, so the digital controller's clock comes from
PLL_F80M / RC_FAST / XTAL, defaulting to PLL (`clk_tree_defs.h:476`). With PLL
selected every conversion timed out; selecting **XTAL** made them complete. The PLL
is presumably not running in this image.

**2. `ADC_LL_POWER_BY_FSM` is the wrong power mode, despite the header.** The LL
header labels `BY_FSM` *"used for polling mode"* and `SW_ON` *"used for DMA mode"*,
so a polling driver reads BY_FSM as correct — and the first version used it. IDF's
own oneshot HAL, also polling, unconditionally sets **SW_ON**
(`adc_oneshot_hal.c:64`). The header comment and the working code disagree; the
code wins.

Bisected, not guessed: SW_ON + XTAL works; SW_ON + PLL fails on all six channels.
Both changes are required. Also needed: `sar_periph_ctrl_adc_oneshot_power_acquire()`
(the SAR is a shared analog block, `SOC_ADC_SHARED_POWER`) and the bus/register/
function clocks plus FSM wait states.

**A bug in my own test, worth recording because it hid the failure for a round.**
The first version printed `PASS` while every conversion had failed:
`rt_adc_read()` returns the *value* and discards the error — 0 for a grounded pin
and for a timeout alike — so the verdict has to go through `dev->ops->convert()` to
see `-RT_ETIMEOUT`. The same version left a preprocessor block unbalanced,
reported as "unterminated #if" sixty lines from the real site.

**Not claimed:** no voltage conversion. `get_vref()` returns 0 rather than a
made-up reference, because the calibration step is not run.

## 17a. Round 11: GPSPI master as `spi0` — registers only at the time (data path verified in §17k)

```
msh > list device
spi0         SPI Bus              0
adc1         ADC Device           0
i2c0         I2C Bus              0
pwm0         PWM Device           0
pin          Pin Device           0
uart0        Character Device     1
```

`port/esp32c5/spi.c` registers one bus (GPSPI2) as an `rt_spi_bus` on the C5's own
IOMUX defaults — SCLK IO6, MOSI IO7, MISO IO2, CS IO10 — so `rt_spi_transfer()`
and any RT-Thread SPI device package work against it.

**Three things differ from the S31 file, and one of them is a silent-write hazard:**

1. **One GPSPI instance, not two.** `SOC_SPI_PERIPH_NUM` is 2, but index 0 is MSPI
   (the flash controller, no signal table) and only index 1 is GPSPI2.
   `SPI_LL_GET_HW()` returns NULL for anything that is not `SPI2_HOST`, and writing
   "the second general-purpose bus" would land in the flash controller.
2. **The pad signals come from `spi_periph_signal[]`, not `gpio_sig_map.h`.** The C5
   names them `FSPICLK_OUT_IDX` / `FSPID_OUT_IDX` / `FSPIQ_OUT_IDX` /
   `FSPICS0_OUT_IDX`; the S31's `SPI2_CK_PAD_OUT_IDX` and friends do not exist here.
3. **The clock must be queried.** `spi_ll_master_set_clock()` is only exact when the
   source is APB, so the driver asks `esp_clk_apb_freq()`.

**The self test, and what it truthfully reports:**

```
c5spiloop: spi0 tx/rx at 1000000 Hz (tie MOSI to MISO for a real test)
  tx: a5 5a 01 80 ff 00 3c c3
  rx: 00 00 00 00 00 00 00 00
c5spiloop: all zeros - the master ran but nothing drove MISO; tie MOSI to MISO
           with a jumper to test the path
```

No jumper is fitted on this board, so **the data path is not verified**. What the
report can say is that the master ran a transaction to completion (a stuck
peripheral would have printed a transfer timeout from the poll loop) and that
nothing drove MISO — which is the correct reading of an open pad, not a pass.

The command distinguishes three outcomes on purpose: rx identical to tx means the
path works; rx all zero means the master clocked and the pad is open; rx anything
else means something drives MISO but the data is wrong. Reporting the middle case
as a failure is the mistake the S31 file warns about in the other direction — it
printed "PASS" for a pattern that only matched because nothing was attached.

**A software loopback was attempted and does not work.** Two variants:

* route `spid_out` straight to `spiq_in` — rx all zeros, because the GPIO matrix is
  pad-centric and nothing drives the pad;
* make the MISO pad carry `spid_out` itself — rx becomes a constant `0xff`, i.e.
  the pad output is enabled but the data does not arrive, so that pad's
  output-enable is not tied to the SPI block's own MOSI enable.

Getting it right needs the pad's OE source wired to the SPI output-enable signal,
a different matrix slot than the data signal. Not pursued: a jumper is a
one-second test and this is a diagnostic convenience. The failure is recorded in
the source so the next person does not repeat it.

**Two build traps, both measured:**

* `esp_hal_gpspi` in `PRIV_REQUIRES` does not put `hal/spi_ll.h` and friends on the
  compile line; linking `idf::esp_hal_gpspi` does — the same trap as LEDC, I2C and
  ADC.
* `spi.c.obj` compiled and the linked image contained no `rt_hw_spi_init` at all,
  so `list device` never showed `spi0`, with a clean build. The `-u` anchor fixed
  it — the fifth time this has happened in this port.

## 17b. Rounds 12-13: TWAI (CAN / CAN-FD) — working in loopback

`port/esp32c5/twai.c` implements the controller path as a plain C API
(`rt_hw_twai_init/set_bitrate/set_pins/start/stop/send/recv`) on TWAI0. As with
the S31 file this derives from, the `rt_can` device class is deliberately left for
after the hardware path is proven — a device class on top of an unproven register
sequence only makes failures harder to see.

**Round 12 failed.** Kept because the failure is what led to the fix. Measured,
four bit rates, the controller's own internal loopback (`ilbp`):

```
c5twai  : TWAI0, self-test + internal loopback, no transceiver
c5twai  : 125000 bit/s: nothing came back (-2), tx_status=0x00008886 tec=160 rec=0 err_type=2
c5twai  : 250000 bit/s: nothing came back (-2), tx_status=0x00008886 tec=160 rec=0 err_type=2
c5twai  : 500000 bit/s: nothing came back (-2), tx_status=0x00008886 tec=160 rec=0 err_type=2
c5twai  : 1000000 bit/s: nothing came back (-2), tx_status=0x00008886 tec=160 rec=0 err_type=2
c5twai  : FAIL, 4 of 4 rate(s) did not round-trip
```

**What the numbers say, and they are specific.** `tx_status = 0x00008886` decodes
as buffer 0 in state 6 (`TXT_ERR` = transmission failed) and buffers 1-3 in state
8 (empty). `tec = 160` is the transmit error counter at its failed-transmission
saturation, `rec = 0` means nothing was ever received, and `err_type = 2` is
`TWAIFD_LL_ERR_FRM_ERR` — a **form error**, not an acknowledge error (which would
be 3). That matters: a form error means the controller saw something structurally
illegal on its own RX path, so the loopback *is* delivering something and it is
malformed — not "no ACK because there is no second node".

Three hypotheses were tried and none changed the result:

1. **`twaifd_ll_enable_test_mode(true)` removed.** That sets `mode_settings.tstm`,
   which reads like the natural companion to self-test mode; IDF never calls it
   (`twai_hal_v2.c` sets stm/ilbp through `set_mode()` and stops). No change.
2. **Filter mode flipped to `afm = 1` + an explicit range filter accepting all
   standard IDs.** The S31 file uses `afm = 0` ("store everything") on the
   reasoning that `afm = 1` with no filters drops every frame. IDF's C5 HAL sets
   `afm = 1` and relies on per-filter enables. Tried the IDF shape. No change.
3. **Clock source XTAL instead of the default PLL_F80M.** This is what fixed the
   ADC (§17), and the reasoning is identical — the PLL is not enabled in this
   image unless something asks for it. No change here, which suggests the TWAI
   clock is not the problem (or that `set_clock_source` is not enough on its own).

**What is not yet known.** The form error is the thread to pull: something between
the formatted TX buffer and the looped-back RX frame is malformed. Candidates not
yet ruled out: the `twaifd_ll_mount_tx_buffer` byte layout versus
`twaifd_ll_format_frame_*`, `enable_fd_mode(true)` interacting with a `fdf = 0`
classic frame on this silicon, and the errata workaround in `set_mode()` (it sets
`rom`/`acf` together with `bmm` when `listen_only` is requested — passing false
keeps them clear, which is what this driver does).

### 17b.1 What the register dump added, and the one bug it did find

A dump of the timing, mode and probe frame turned the failure from a guess into
three facts:

```
c5twai  : xtal=48000000 Hz btr=0x0830a302 mode=0x0227021c ena=0
c5twai  : probe frame w0=0x00000004 w1=0x048c0000 (id=0x123 dlc=4)
c5twai  : pads TX=IO4 RX=IO13, idle levels tx=1 rx=1
```

Decoded against `twaifd_struct.h`'s field layout:

* `mode = 0x0227021c` → `stm=1, afm=1, fde=1, rxbam=1, rtrle=1, ilbp=1,
  ena=0`. Self-test and internal loopback are genuinely on. The pads idle
  recessive (1/1), so a floating line is not the cause.
* `probe frame w0 = 0x00000004` → `dlc=4`, everything else clear — a valid
  classic header. `w1 = 0x048c0000` → `0x123 << 18`, the identifier in the right
  field. **The frame the driver builds is well-formed.**

**The bug this found: the bit timing was 14 % fast.** `btr = 0x0830a302` decodes
to `brp=6, prop=2, ph1=6, ph2=5` — 14 quanta per bit, so
`48e6 / (6 * 14) = 571428 Hz` where 500000 was asked for.

The cause is an inherited subtraction. The S31 file writes
`prop_seg = prog/4 - 1` and `tseg_1 = prog/2 - 1` because the CTU CAN FD IP it
drives encodes each segment as *(quanta - 1)*. The C5's controller uses a plain
Synopsys-style layout — `twaifd_struct.h` documents `prop: bitpos [6:0],
measurement unit: time quanta` with no bias — so carrying the `- 1` over made
every bit short. Fixed, and now exact:

```
c5twai  : btr=0x0830a383 -> brp=6 prop=3 ph1=7 ph2=5 sjw=1 -> 16 quanta
            bitrate = 500000 Hz (asked 500000)
```

**The loopback still fails with the same form error at every rate**, so the timing
was a real defect but not *the* defect. That narrows it usefully: the frame is
well-formed, the mode bits are right, the clock is right, the timing is now right,
and the pads are idle recessive — the remaining suspect is the TX buffer handoff
itself (`twaifd_ll_mount_tx_buffer` / the `tx_command` state machine), not the
data being mounted into it.

Two more hypotheses were eliminated in this round, both with no change in result:
`enable_fd_mode(true)` → `false` (classic-only), and making the "return buffer to
Empty" command conditional instead of unconditional. The latter is a no-op either
way because the free-buffer loop always leaves `buf_idx` on an already-Empty
buffer.

### 17b.2 Round 13: the fix is a single pad, and a write-only window that hid it

Two findings closed this out, in the order they mattered.

**The TX memory window cannot be read.** The obvious way to check the handoff is
to read `txt_mem_cell[]` back after mounting and compare. That reads as zero:

```
c5twai  : txt[0] want=0x00000004 got=0x00000000
c5twai  : txt[4] want=0x44332211 got=0x00000000
c5twai  : mount verify: MISMATCH (3 word(s) differ)
```

which looks damning until the same test is pointed at an **unused** buffer:

```
c5twai  : window probe buf3: before=0x00000000 wrote=0xdeadbeef read=0x00000000 -> WRITE-ONLY
```

Writing a known pattern and reading it straight back returns zero. **The window is
write-only from the APB side**, so a read-back "mismatch" says nothing about
whether the frame landed. The mount check was measuring the bus, not the driver,
and it is removed rather than left in reporting a meaningless MISMATCH.

**The actual fix: put TX and RX on the same pad.** `TWAI0_TX_IDX` and
`TWAI0_RX_IDX` are the same matrix index on this chip (both 66), so a single pad
can carry both directions — its own input sees what its own output drives. The
S31 file documents exactly this as its way of separating "the internal loopback
bit is not taking effect" from "the frame never reaches the pin". That was the
case here: with `ilbp` set the controller still reported a form error on every
frame, and with TX and RX on IO4 the same code path works at every rate.

```
c5twai  : pads TX=IO4 RX=IO4, idle levels tx=1 rx=1
c5twai  : 125000 bit/s: id=0x123 len=4 echoed back OK
c5twai  : 250000 bit/s: id=0x124 len=4 echoed back OK
c5twai  : 500000 bit/s: id=0x125 len=4 echoed back OK
c5twai  : 1000000 bit/s: id=0x126 len=4 echoed back OK
c5twai  : PASS at all 4 rates (controller verified; the analog layer is not -
           no transceiver fitted)
```

Stable across three consecutive runs. What this verifies: bit timing at four
rates, classic frame formatting, the RX filter path, the TX buffer handoff, and
the data path in both directions. What it does **not** verify: the analog layer —
no transceiver, no differential pair, no termination, no arbitration against a
real second node, no error-frame behaviour. `ilbp` and the single-pad trick are
both internal; neither proves a wire works.

**So the driver now works, and it registers no RT-Thread device.** The `rt_can`
device class is still deliberately not wired up: the hardware path is proven for
loopback only, and a device class on top of it would advertise capability the
analog side cannot back. That is §21's business.

The old paragraph, kept for the record:

**Stated plainly: TWAI was not working for rounds 12.** The driver was written, it
compiled, registered no RT-Thread device (by design), configured timing at four
rates and reported exactly why each frame failed. That was a starting point, not a
result.

**One compile-time finding worth keeping:** `twaifd_ll_get_err_reason()` returns a
bit-field *struct* (`twai_error_flags_t`), not an integer, so casting it for
`rt_kprintf` is a compile error ("aggregate value used where an integer was
expected"). The raw `err_capt_retr_ctr_alc_ts_info.err_type` nibble is the value a
diagnostic actually wants.

Also: the C5 splits TWAI across two headers. `twai_ll.h` has only four functions
(bus clock, reset, enable clock, clock source); every frame and register operation
lives in `twaifd_ll.h`. Reading `twai_ll.h` alone suggests the driver does not
exist, and there is **no** `twaifd_ll_mem_force_power_on()` on this chip — that S31
step has no C5 equivalent.

## 17c. Round 14: RMT — transmitter proven, receiver loop left open (resolved in §17o)

`port/esp32c5/rmt.c` implements RMT TX and RX on channel 0/0 as a plain C API.
**The transmitter works; the RX self-loop does not.**

**What the transmitter measurement proves.** Four independent checks, all passing:

```
c5rmt   : pad self-check IO0: drove 1 -> read 1, drove 0 -> read 0 (pad and input path OK)
c5rmt   : pad sampling during continuous TX: 31 transition(s) in 20000 reads
                     -> RMT reaches the pad, fault is on the RX side
c5rmt   : tx status: state=0 raddr=21 txdone=1 intraw=0x00000005
c5rmt   : mem probe tx[0]=0x12999299 tx[1]=0x92991299
```

Decoding the two symbols against the requested waveform:

```
0x12999299 -> d0=4761 l0=1 d1=4761 l1=0
0x92991299 -> d0=4761 l0=0 d1=4761 l1=1
```

4761 ticks at 21 ns is 100 µs — exactly what was asked for, with the levels
alternating as intended. The symbols in RMTMEM are the correct waveform, `tx_end`
fires, and the pad was observed toggling through its own input while the channel
ran in continuous mode. So: clock, divider, symbol encoding, memory writes,
transmitter, output routing and the pad are all verified.

**What does not work: the receiver never captures the frame.**

```
c5rmt   : rx status: state=97 waddr=96 rxdone=1
c5rmt   : rx 0 symbol(s), 0 pulse(s), mean half = 0 tick(s)
```

`ch2_rx_end` is set and the write pointer advanced by exactly one past its base of
96, i.e. the receiver arm-then-stopped, capturing a single symbol and no edges.
That is the signature of the idle detector firing before the transmission starts,
not of a broken signal path — the signal path is proven above.

Five things were tried against this, none of them fixed it:

1. `idle_thres = max (0x7FFF)` instead of a multiple of the half period, so the
   gap between arming RX and starting TX cannot end the frame.
2. `rmt_ll_rx_enable_filter(false)`.
3. `rmt_ll_rx_set_mem_owner(HW)` before arming and `SW` before reading — IDF does
   exactly this handoff around its copy (`rmt_rx.c:655-658`), and without the SW
   step every read returns zero even when the receiver did capture data.
4. Reading the RX write pointer from `chmstatus[]` rather than `chnstatus[]`
   (they are different arrays: the former is TX, the latter is RX).
5. Filling the memory block completely, then adding the stop pattern.

**Five real bugs were found and fixed along the way**, each of which produced a
failure that looked like something else:

* **The output enable was inverted.** `gpio_ll_set_output_enable_ctrl()`'s second
  argument is the INVERT flag, not "enable". Passing `true` disables the pad
  exactly when the peripheral wants to drive it.
* **…and it was asking the wrong master to drive it.** `ctrl_by_periph = true`
  assumes the peripheral offers an output-enable signal. The RMT on this chip only
  provides the data line, so the pad stayed undriven while TX raised `tx_end`
  normally. `ctrl_by_periph = false` (GPIO owns the enable) is what works.
* **`state_chn` is a 3-bit field, not a flag.** Polling it for "returns to idle"
  misses a frame that completes between two reads, so every transmit burned the
  whole timeout on a transmission that had already finished. `ch0_tx_end_int_raw`
  is latched and cannot be missed.
* **The tick length was 20× wrong.** `(1000 + xtal/1000 - 1)/(xtal/1000)` gives
  1 ns at 48 MHz; the tick is 1e9/48e6 ≈ 21 ns, and with the wrong value every
  requested duration was clamped to the 15-bit maximum (32767 ticks).
* **`chmconf[]` has two entries, not four.** The RX channel index passed to every
  `rmt_ll_rx_*` accessor is the channel's position *within the RX half* (0 or 1),
  not the global channel number. Passing 2 wrote past the end of the array; the
  memory index is the separate global number (`index + 2`).

**An RMT transmission also does not end by running out of memory.** It ends at a
symbol whose `duration0` is zero — IDF says so outright (`rmt_tx.c:612`: "a RMT
word whose duration is zero means a stop pattern"). Without that symbol the
channel transmits the whole block and never raises `tx_end`; the only status bit
that appears is `ch0_tx_thr_event`, which is the "sent more than `tx_lim`"
threshold and not an end-of-frame signal.

**Stated plainly: RMT TX is verified, RMT RX is not.** The self test reported FAIL
and the driver was listed as partial. **Resolved in §17o**: the RX carrier
demodulator defaults to ON, which is what `rx 0 symbol(s)` was all along.

## 17d. Round 15: PCNT as `pcnt0` — verified

```
msh > list device
pcnt0        Miscellaneous Device 0
spi0         SPI Bus              0
adc1         ADC Device           0
i2c0         I2C Bus              0
pwm0         PWM Device           0
pin          Pin Device           0
uart0        Character Device     1
```

`port/esp32c5/pcnt.c` registers unit 0 as an `rt_pulse_encoder_device`, so the count
is reachable the RT-Thread way — `rt_device_find("pcnt0")`, `rt_device_read()`,
`PULSE_ENCODER_CMD_CLEAR_COUNT` — rather than only through a private API.

**How it is verified with no wiring.** A PCNT unit counts edges on a pad, and a
pad's input path sees whatever the pad is driven to, including by its own output
driver. So the test drives the pad high/low N times from software and compares the
counter with N. The expected value is exact, so this is pass/fail rather than
"something moved", and it is a real round trip through the input matrix, the glitch
filter and the counter:

```
c5pcnt  : unit 0 on IO13, drove 100 rising edge(s), counter reads 100
c5pcnt  : pcnt0 device path agrees (100)
c5pcnt  : PASS
```

Repeated at 100, 500 and 1000 edges — all exact. The second line matters as much as
the first: the count comes back through `rt_device_read()` as well as through the
driver's own accessor, so the registration is exercised and not just declared.

**The bug worth recording: the glitch filter is on out of reset, and it eats the
signal.** `ctrl.filter_en_un` defaults to **1** with `filter_thres_un` = **16**
(`pcnt_struct.h`). Sixteen APB cycles is a real filter — edges shorter than that are
discarded entirely — and two consecutive register stores produce a pulse far inside
that window. Measured with the reset value: the counter read **0** while every edge
provably reached the pad. Two changes fixed it together: `pcnt_ll_enable_glitch_filter
(false)`, and a 1 µs gap between edges so the pulse is wide enough for the input
synchroniser even with the filter off.

A real encoder wants the filter, with a threshold chosen for its edge rate. What the
driver must not do is silently inherit one that eats the signal — so it is disabled
here and the note explains why, rather than the default being left alone.

**Two more C5-vs-S31 differences:**

* **The pad signal indices are not contiguous per unit.** The header is
  channel-major (`PCNT_SIG_CH0_IN0..3`, then `PCNT_CTRL_CH0_IN0..3`), and per unit the
  numbers are 101/103, 105/107, 109/111, 113/115 — stride 4, not 2. Indexing that
  table arithmetically instead of reading it picks a different unit's channel.
* **The clock source is APB and only APB** — `pcnt_ll_set_clock_source()`
  `HAL_ASSERT`s anything else. That is the opposite of the ADC/CAN/RMT pattern in this
  port, where the default source had to be switched away from the disabled PLL. Three
  drivers needed that fix; this one must not get it.

**One build trap, the sixth of its kind:** `pcnt.c.obj` compiled, and the link failed
with a single undefined symbol — `rt_device_pulse_encoder_register`. The class file
`components/drivers/misc/pulse_encoder.c` was in the S31's source list but not the
C5's. Adding `RT_USING_PULSE_ENCODER` to `rtconfig.h` is not enough on its own; the
framework file has to be added too, and it only compiles once the macro is on.

## 17e. Round 16: MCPWM — verified by two independent measurements

`port/esp32c5/mcpwm.c` programs a complementary pair with dead time on
timer 0 / operator 0 and exposes it as a plain C API.

**How it is verified, and why that is worth something.** Two measurements, both
taken at the output pad, neither needing a scope or a jumper — the pad is both the
MCPWM output and a GPIO input, so the loop closes inside the chip:

1. **Frequency** — the PCNT unit from §17d counts rising edges over a window.
2. **Duty** — software polls the same pin with `mcycle` and times rise→fall→rise.

The counter doing the frequency measurement is the one verified independently in
§17d, so the two drivers check each other rather than one timestamping itself:

```
c5mcpwm : 1000 Hz 500/1000 requested, dead time 2 tick(s)
c5mcpwm : frequency 1000 Hz from 50 edge(s) in 50000 us (PCNT)
c5mcpwm : duty 499/1000 from 959932 high / 1919998 total cycle(s)
c5mcpwm : PASS
```

Matrix run, all PASS: 50 Hz/500 permille, 100 Hz/250/1000, 100 Hz/900/1000, 200 Hz/100/1000,
1 kHz/500 permille, 2 kHz/950/1000, 5 kHz/750/1000, 10 kHz/50/1000, 20 kHz/200/1000, 40 kHz/500 permille.

**What is not verified:** the analog shape of the output (rise time, overshoot) and
the dead time's actual nanoseconds. Dead time is programmed and read back from the
register, but not measured — that needs a scope, so it is stated as unmeasured
rather than claimed. `C5_MCPWM_DEAD_TICKS` is 2 ticks ≈ 42 ns, far below anything a
real half-bridge needs; it proves the programming path runs, not that it protects a
transistor.

### Three measurement bugs, all of which looked like hardware faults

This is the part worth recording. The PWM was correct on the first flash; every
failure in this round was in the *measurement*, and each one presented as a
plausible driver defect.

**1. The period was not a period.** The duty loop captured `t0` *before* waiting
for the rising edge, so the "period" it accumulated was `leftover_low + full_high`
rather than one cycle. Measured 533/1000 for a requested 500 — a 6.6 % error that
reads like a PWM defect. A period is rise→fall→rise; fixed and it reports 499.

**2. A 32-bit multiply overflowed, and only at low frequency.** `high_cycles * 1000`
is fine at 1 kHz but not at 100 Hz, where one accumulated period is ~19 200 000
cycles and the product is 4.8e9 — past 2³². Measured: `c5mcpwm 100 250` reported
**26/1000** and `c5mcpwm 100 900` reported **5/1000**, both FAIL, while the same
driver passed at every frequency above because the product fitted.

```
correct duty   = 250/1000
(high*1000) mod 2^32 / total = 26     <- exactly what was printed
```

Dividing first would have lost the resolution instead (1000-cycles-per-period
truncates to 0), so widening to 64 bits is the correct fix rather than a
workaround. Note the shape of this bug: it is invisible at the frequency used for
the first test and appears only when a value is *chosen* to be large.

**3. The measurement window was fixed, so the result was quantised.** A fixed 50 ms
window resolves to 20 Hz steps (one edge per 50 ms), so a requested 50 Hz measured
as **40 Hz** — 2 edges instead of 2.5 — and failed a 10 % check that the hardware
passed easily. The window now scales with the frequency to target 20 edges
(20000000/freq µs), clamped to [50 ms, 500 ms], putting the quantisation at 5 %
while keeping the shell responsive at high frequencies.

**Two C5-vs-S31 differences:**

* **The pad signals are named differently and shorter.** The C5 has
  `PWM0_OUT0A_IDX` / `PWM0_OUT0B_IDX`; the S31 uses
  `PWM0_CH0_A_PAD_OUT_IDX` / `PWM0_CH0_B_PAD_OUT_IDX`. The peripheral is called
  MCPWM in code and PWM in the matrix.
* **`mcpwm_ll_group_set_clock_source()` takes a `soc_module_clk_t`**
  (`SOC_MOD_CLK_XTAL`), not a `mcpwm_clock_source_t`. Same call, different type —
  the S31 signature does not compile here.

And one field name one letter apart: the C5 register struct calls the timer config
`timer_cfg0`, the S31 `timern_cfg0`, while the field inside stays `timern_period`.
The compiler catches it; the eye does not.

**Clock source is XTAL, deliberately.** The PWM frequency then does not move when
DFS changes the CPU clock, which is the one property a motor-control signal must
have. It is also the source that is actually running in this image — five drivers
in this port have now been bitten by a default that points at an unpowered PLL
(ADC, CAN, RMT, and both of MCPWM's alternatives here).

## 17f. Round 17: temperature sensor — reading, with the caveats stated

`port/esp32c5/tsens.c` brings up the on-die sensor and reports degrees Celsius.

```
c5tsens : band -10..80 C (error < 1 C), DAC offset 0, XTAL clock
c5tsens : 32 sample(s): first 15, last 15, min 15, max 15, mean 15 C
c5tsens : raw now 82, spread 0 C, drift +0 C
c5tsens : band 2 vs band 4 agree to 3 C (offsets 0 and +2; a dropped range write would show ~56 C)
c5tsens : PASS
```

**How a sensor is checked with no reference.** It cannot be checked against a
known temperature, so it is checked against itself in two ways that a stuck
register cannot fake:

1. **Plausibility** — the mean has to be a temperature a powered board can be at
   (−20…100 °C). A dead register usually reports 0, −128 or similar.
2. **The range register actually taking effect** — the same die is read in two
   bands whose DAC offsets differ by 2. If the internal-I2C range write is
   dropped, the *same* raw value is interpreted with two different offsets and the
   answers land `2 × 27.88 = 55.8 °C` apart. If the write works, the raw value
   shifts to compensate and the two agree. Measured 3–4 °C apart, every run.

That second check is the one that matters on this chip, and it is not obvious from
the data sheet: **the sensor's only configuration path is a `regi2c` write**, and
the internal I2C bridge that reaches it (`PMU_RF_PWC_REG`'s `XPD_PERIF_I2C`) is off
by default. With it off, every range write is dropped **silently** — the sensor
still reads, just always against whatever range value happened to be latched. The
driver calls `regi2c_saradc_enable()` first, which clears `PERIF_I2C_RSTB`, waits
the 1 µs the design recommends, powers the bridge, and releases the reset.

**A criterion that had to be dropped.** The first version required the 16 reads to
differ ("otherwise the value is not being re-sampled") and failed a working sensor:

```
c5tsens : 16 sample(s): first 25, last 25, min 25, max 25, mean 25 C
c5tsens : FAIL, all 16 reads were identical
```

The ADC quantises to whole degrees inside a band, so a thermally stable die
legitimately lands on one integer. A constant reading is not evidence of a fault;
a range register that never took effect *is*, so that is what gets tested
directly — by the two-band cross-check above.

**Not verified, and it matters more here than anywhere else in this port: the
value is NOT calibrated.** IDF reads a per-chip `deltaT` from eFuse and subtracts
`deltaT/10` from the result, which is what makes a reading *absolute* degrees
Celsius rather than "this sensor on this die". This driver does not apply it, so
the numbers above are uncalibrated and can be several degrees off. They are
internally consistent and they track the die, but they are not a thermometer
reading. That is stated in the command's own output rather than only in the docs.

**Three C5 specifics:**

* The sensor lives in the **SAR ADC block**, not in its own peripheral: registers
  are `APB_SARADC.saradc_apb_tsens_ctrl` / `tsens_sample`, the power switch is
  `saradc_tsens_pu`, and the range is a `regi2c` write to `I2C_SARADC_TSENS_DAC`.
* Its clock is selected in **PCR** (`tsens_clk_sel`), not in the ADC's digital
  clock register — a different register from the one §17's ADC driver touches.
* The conversion is
  `(4386·raw − 278800·dac_offset − 205200) / 10000`, with the DAC offset picked
  from a five-entry band table. This driver selects the −10…80 °C band (error
  < 1 °C), which is the one that covers a board at room temperature, and says so
  rather than pretending to cover all five.

**One more build-blocking fix, the fourth of this shape.** `ft_task.c` and
`freertos/port_systick.c` both carry someone's active debugging probes that
reference `rt_xt_br[]` / `rt_xt_br_task[]` — symbols defined only in
`libcpu/xtensa/cpuport.c`. On a RISC-V target they do not exist and the link
fails:

```
undefined reference to `rt_xt_br'
undefined reference to `rt_xt_br_task'
```

Guarded with `#if defined(__XTENSA__)`, not deleted. The other three are the
`RT_USING_SMP` guards in `ft_task.c` and `thread.c`; this is now a recurring
pattern — **debug instrumentation written for one architecture lands in shared
code and breaks the other.**

## 17g. Round 18: the 8 MB PSRAM is up

The board is a C5HR8 — 16 MB flash and **8 MB in-package PSRAM** — and the PSRAM
half had never been enabled. It is now, and it is verified.

```
c5psram : initialised=yes, reported size=8388608 byte(s)
c5psram : IDF heap: internal=152720, spiram=8388608, total=8541328 byte(s)
c5psram : wrote and read back 1048576 byte(s), crc32=0xabc4e6c2, 0 mismatch(es)
c5psram : PASS
```

**One megabyte, byte-exact, with a position-dependent pattern** — byte `i` holds
`i ^ (i >> 8)`, so no two addresses hold the same value and a stuck or shorted
address line cannot pass as a block of identical bytes. Also run at 64 KB. The
allocation is `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` and nothing else, so a
failure would mean "PSRAM cannot satisfy this" rather than being quietly served
from internal RAM.

**Three things had to be got right, and the first one silently does nothing:**

1. **The bootloader has to be rebuilt.** `CONFIG_SPIRAM` was added to `sdkconfig`
   and the build said `Project build complete` — and the boot log still had no
   PSRAM and `esp_psram_get_size()` still returned 0. The bootloader binary was
   still the 10:57 one from before the config change: the `bootloader-prefix`
   sub-build is only re-run when its stamp says so, and a config change does not
   invalidate it. Removing `build/bootloader` alone makes it worse — the stamp
   then refers to a missing cache and ninja fails with `could not load cache`.
   **Both `bootloader/` and `bootloader-prefix/` have to go.**
2. **`CONFIG_SPIRAM_IGNORE_NOTFOUND=y` hides exactly the failure being debugged.**
   With it set, a chip that does not answer is logged at DRAM level and startup
   continues; without it, `cpu_start.c` aborts. For bring-up the abort is the
   useful behaviour, so it is now off — and the absence of that abort is itself
   evidence the chip answered.
3. **The guard has to be `CONFIG_SPIRAM`, not `SOC_SPIRAM_SUPPORTED`.**
   `SOC_SPIRAM_SUPPORTED` says the *silicon* has a PSRAM controller and is 1 on
   this chip regardless of configuration; the `esp_psram` API behind it is
   compiled out when `CONFIG_SPIRAM` is off. The first version of `psram.c`
   guarded on the capability macro without including `soc_caps.h` at all, so the
   whole file compiled to nothing and the link failed with
   `undefined reference to c5_psram_selftest`.

**What is deliberately not done: RT-Thread's heap still does not use it.** The two
allocators are separate — RT-Thread's `rt_malloc()` runs on a fixed region
(`CONFIG_IDF_RTTHREAD_HEAP_SIZE`, 64 KB) and cannot see IDF's `heap_caps` heap,
which is where the 8 MB went. Bridging them is a real design decision, not a
detail: pointing a small-object allocator at an external SPI-backed region changes
the cost of every allocation in the system, and its free-list metadata would live
in the memory it manages. So PSRAM is **available and proven**, and `rt_malloc()`
still cannot reach it. That is stated here rather than implied by the word
"enabled".

**Not verified:** PSRAM under DMA, and as executable memory (XIP). Both are
opt-in (`CONFIG_SPIRAM_XIP_FROM_PSRAM` is off) and neither is needed for the
memory to be usable.

## 17h. Round 19: the LP core (ULP RISC-V) runs

The C5's second core is now up and verified. This was the last whole functional
domain in this port that had never been touched.

```
c5ulp   : LP core, 2000 loop(s) requested
c5ulp   : magic=0x4c504331 (want 0x4c504331), seq=2000/2000, done=1 after 4 ms
c5ulp   : checksum=0x85201807 (want 0x85201807)
c5ulp   : second run: seq=500/500 done=1
c5ulp   : PASS
```

Repeated at 100, 5000 and 20000 loops — counter and checksum exact every time,
1/9/31 ms respectively.

**How a second core is verified when you cannot watch it.** The HP core only sees
what the LP core leaves in memory, so the test is built from things only the LP
core could have produced:

1. **A magic word written after setup** — proves the program reached its main
   loop, not merely that a binary was loaded.
2. **A counter that keeps rising** — a stale value from a previous run could fake
   a single write; a value that climbs while the HP core polls cannot.
3. **A checksum recomputed independently** on the HP side from the same loop
   count — checks the LP core's *arithmetic*, not just its liveness.
4. **A second start** — distinguishes "it ran once" from "it runs".

**Three build/plumbing traps, each of which looked like a driver bug:**

1. **`.shared_mem` is 16 bytes and the framework already owns all of it.**
   `CONFIG_ULP_SHARED_MEM` is `0x10` with **no Kconfig prompt**, so a larger value
   written into `sdkconfig` is silently ignored — and `ulp_lp_core_memory_shared.c`
   `static_assert`s the size because its own config struct fills the region. Any
   user variable placed there overflows and the LP link fails with
   `address 0x50001010 of ulp_c5_lp.elf section '.shared_mem' is not within region
   'shared_mem_ram'`. The fix: user variables go in **ordinary `.bss`**. LP RAM is
   RTC slow memory at `0x50000000`, which the HP core reads directly, and
   `esp32ulp_mapgen.py` exports every global with its absolute address — so no
   special section is needed at all. Only `volatile` is, so the loop is not
   optimised into a single terminal store.
2. **The generated header prefixes the names.** `ulp_add_project()` runs
   `esp32ulp_mapgen.py` with `--prefix ulp_`, so `shared_seq` becomes
   `ulp_shared_seq`. The compiler's `did you mean 'ulp_shared_seq'` is a real
   mismatch, not noise.
3. **The shared variables have to be written AFTER `ulp_lp_core_load_binary()`.**
   That function does `hal_memset(base, 0, CONFIG_ULP_COPROC_RESERVE_MEM)` over the
   whole LP RAM before copying the program in — "Clear LP RAM, also initializes
   LP-side bss" in its own comment. Anything the HP core wrote into LP memory
   beforehand is wiped.

Point 3 is the interesting one, because of *how* it failed. Writing `loops = 2000`
before the load produced:

```
c5ulp   : magic=0x4c504331 (want 0x4c504331), seq=1000/2000, done=1 after 3 ms
c5ulp   : checksum=0xfb2785e0 (want 0x85201807)
c5ulp   : FAIL, the loop ended at 1000 of 2000 iteration(s)
```

The magic, the done flag and the counter all looked healthy — the LP core had
clearly run and finished cleanly. Only the loop count disagreed, because the
control word had been zeroed and the LP program fell back to its own default of
1000. **A clean run with one wrong number is the signature of a handoff that
happened too early**, and the counter still being exactly 1000 (not partial) is
what ruled out a crash.

**One architectural note worth keeping.** The ULP driver is *not* in
`components/rtthread/port/esp32c5/`. It lives in `main/ulp_selftest.c` because
`ulp_add_project()` attaches the generated shared-symbol linker script to the
component that calls it — the shared variables are only visible from there. An LP
program is a property of the **project**, not of the port. The first version put
the driver in the port layer and failed with `undefined reference to
ulp_c5_lp_shared_ctl`, which is not obviously about component scoping until you
read `project_include.cmake`.

**Not verified:** deep-sleep behaviour (the LP core is started here with the HP
core awake), and the LP core's own peripherals — LP UART, LP I2C, LP ADC, LP
touch. Also nothing about its power consumption.

**Two more Xtensa probes had to be guarded** — `rt_xt_sst` in
`freertos/port_systick.c` and `rt_xt_lk` in `esp_libc/src/locks.c`, both defined
only in `libcpu/xtensa/cpuport.c`. That is the sixth and seventh occurrence of
this shape. `port_systick.c` in particular is someone's live debugging target and
has gained a new probe each time it has been touched.

## 17i. Round 20: SoftAP — and a regression the STA had been hiding

The board now beacons as an access point, verified from **outside the chip**:

```
msh > c5ap
c5ap    : mode=0x0
c5ap    : start=0x0
c5ap    : AP_START ssid="c5-rtt-ap" ch=6 auth=WPA2 ip=192.168.4.1
c5ap    : the STA association (if any) is now down - one radio, one channel
c5ap    : clients=0, max=4
c5ap    : READY (beaconing; no client has associated yet)
```

and on the host, which is associated to a 5 GHz AP 2 m away:

```
SSID 6 : c5-rtt-ap
      Authentication          : WPA2-Personal
      Encryption              : CCMP
      BSSID 1                 : 14:c1:9f:8a:11:39
           Signal             : 95%
           Radio type         : 802.11n
           Band               : 2.4 GHz
           Channel            : 6
```

That BSSID is the board's own MAC (`14:c1:9f:8a:11:38`) plus one, on the channel
that was programmed. **This is the only check in the whole port that leaves the
chip**: every other result is the board reporting on itself. A driver that thinks
it is beaconing and a driver that is beaconing look identical from inside.

**One radio, so this is a mode switch, not an addition.** The C5 has a single
802.11 radio; starting the AP tears the STA down. The command says so in its own
output rather than leaving it to be discovered when `c5tcp` starts failing.

### The regression: a fix from an earlier round had been lost

`c5ap` did not fail — it **aborted**:

```
abort() was called at PC 0x408025db on core 0
MEPC : 0x40811b90   MCAUSE : 0x00000002
```

`addr2line` on both the PC and the stacked return addresses gave a precise chain:

```
lock_acquire_generic          esp_libc/src/locks.c:150
adc_reset_lock_acquire        esp_hw_support/port/esp32c5/sar_periph_ctrl.c:154
sar_periph_ctrl_adc_reset     esp_hw_support/port/esp32c5/sar_periph_ctrl.c:137
adc_apb_periph_claim          esp_hw_support/adc_share_hw_ctrl.c:209
temperature_sensor_power_acquire  esp_hw_support/sar_tsens_ctrl.c:49
phy_set_tsens_power           esp_phy/src/phy_override.c:93
phy_rf_init
```

So: **`esp_phy_enable()` takes the RT-Thread scheduler lock, then reaches into
libc, which lands in `lock_acquire_generic()` and aborts.**

The cause is that `locks.c` decides "am I in an ISR?" with `!xPortCanYield()`, and
that predicate is false in **three** different situations:

* genuine interrupt context,
* an RT-Thread scheduler lock (`rt_critical_level() != 0`),
* global IRQ masking.

Only the first is an ISR. The other two are a *task* that happens to hold a lock —
and taking the ISR branch from a task is fatal, because `xSemaphoreTakeFromISR()`
on a held semaphore returns false and line 150 calls `abort()`.

**This had been fixed in an earlier round and the fix was gone.** `git status`
showed `locks.c` unmodified against HEAD, i.e. the working-tree edit was lost
somewhere — the same failure mode as the report file earlier in this port, and it
only surfaced now because nothing had re-initialised the PHY since. The fix is
restored, with the reasoning written next to it this time:

```c
    /* xPortInIsrContext(), NOT !xPortCanYield(). ... */
    if (xPortInIsrContext()) {
```

**Two lessons, both worth more than the feature:**

1. **A lost edit is invisible until the path it fixes runs again.** `c5join5` had
   been passing for three rounds with the fix absent, because association only
   re-inits the PHY on a cold start and the board had been rebooting into a warm
   state. The regression was found by adding a *new* command, not by any test that
   was watching for it.
2. **`addr2line` on the stacked return addresses, not just the fault PC.** The PC
   only said `locks.c:150` with no indication of who called it; the stack gave the
   whole seven-frame chain up through `phy_rf_init` in one command.

### The AP/STA round trip had to be made a round trip

`c5ap down` first returned `0x3001` (`ESP_ERR_WIFI_NOT_INIT`) and then left the
STA unable to rejoin:

```
c5ap    : stop=0x0
c5ap    : AP is down
E (142390) wifi_init: Failed to init, WiFi is initialized by esp_wifi_init
wifi    : FAIL init 0x103
```

`esp_wifi_stop()` leaves the driver initialised, so the STA path's next
`esp_wifi_init()` fails with `ESP_ERR_INVALID_STATE` (0x103). Deinit is the missing
half:

```
c5ap    : stop=0x0
c5ap    : deinit=0x0
c5ap    : AP is down and the driver is released; `c5join5` can bring the STA up again
wifi    : GOT_IP, associated 10.0.0.195 (connects=1)
```

This is the mirror image of the "one radio" caveat: the two modes cannot coexist,
so handing the driver back cleanly is part of the contract, not an afterthought.

**Not verified:** a full association + DHCP lease from a real client. The only
available host would have to leave the 5 GHz AP the rest of this port is tested
against, so it is stated as untested rather than risked. Everything up to and
including "beacons are on the air, on the right channel, with the right security"
is verified.

## 17j. Round 21: the work is committed, and a pin registry that finds clashes

Two things this round, neither of them a new peripheral. Both came out of the same
observation: **several fixes in this tree had already been lost once** (§17i — the
libc lock fix silently reverted and stayed invisible for three rounds), and there
was no history to recover them from.

### The port is now in git

`esp-idf-rtt` had no commits of its own work: `git status` showed 71 modified
files and an untracked `components/rtthread/port/esp32c5/` with eleven drivers in
it. A single edit lost from there is unrecoverable. Two commits now exist on a
branch off `rtt-s31`:

```
312a3f0b C5 port: pin ownership registry, and two clashes it found
4352d736 C5 port: kernel, radios, and eleven device drivers
```

The working tree is clean, and the ISR-context fix that was lost is verifiably in
`HEAD`:

```
$ git show HEAD:components/esp_libc/src/locks.c | grep xPortInIsrContext
    if (xPortInIsrContext()) {
    if (xPortInIsrContext()) {
```

The board's schematic PDF was excluded rather than committed — it is the user's
file, not part of the port.

### `c5pins` — who owns which pad

A failure class had now cost two rounds and would have cost a third:

* `c5adc` parks its channel pads for analog, and **ADC1 channel 3 is GPIO4** —
  which is where the CAN self test puts TWAI0's TX and RX. Running the ADC test
  before the CAN test made CAN fail with a form error at every rate (§17d).
* The C5's **native USB D-/D+ pads are GPIO13 and GPIO14**
  (`USB_INT_PHY0_DM/DP_GPIO_NUM` in `io_mux_reg.h`). The PCNT self test was
  pointed at IO13 and MCPWM's output B at IO14. Both *worked*, because those pads
  behave as ordinary GPIO while nothing enumerates USB — which is precisely the
  kind of latent trap that springs when someone next enables the USB console.

Neither is a driver bug. Both are two things wanting one pad, found only by
running them in an unlucky order. So `port/esp32c5/resources.c` holds one table of
pad ownership, and `c5pins` reports:

```
c5pins  : 14 pad(s) in use
c5pins  : IO11  uart0 console TX             (UART0 default)
c5pins  : IO4   CAN TX and RX (one pad)      (C5_TWAI_TX_PIN)
c5pins  : IO5   PCNT test                    (C5_PCNT_TEST_PIN)
  ...
c5pins  : ADC1 channels and the drivers that share their pads:
c5pins  :   ch3 IO4  SHARED with CAN TX and RX (one pad) - reading it parks the pad for analog
c5pins  :   ch4 IO5  SHARED with PCNT test - reading it parks the pad for analog
c5pins  : PASS, no pad is claimed twice and no test sits on a fixed-function pad
```

It returns 0/-1 rather than only printing, so it can be a boot self-check. Each
table row names the `#define` it mirrors, so a driver's pin changing shows up as a
visibly stale comment rather than as a silent conflict.

**It found two real clashes on its first run**, and both are fixed:

* PCNT's test pad moved from IO13 to **IO5**.
* MCPWM output B is still *programmed* (it is half of the complementary pair, and
  the dead-time path needs it) but **no longer routed to a pad** — `-1` now means
  "no pad", which `rt_hw_mcpwm_set_pin()` already handled. Only output A is
  measured, so routing B bought nothing and cost a hardware pad.

Verified after the move: PCNT counts 100/100 on IO5, MCPWM still reports
1000 Hz and 500/1000, and running `c5adc` then `c5pcnt` in one session now passes
both — the ADC reads channel 4 (IO5) and the counter still works afterwards,
because 1 µs of settle time between the pad being parked and the counter using it
is not the same situation as CAN's differential pair.

**Eighth Xtensa probe, guarded:** `rt_xt_nt` in `ft_notify.c`. Same shape as the
previous seven.

## 17k. Round 22: the SPI data path closes — one wrong word, three symptoms

`spi0` has been registered since round 11 with its data path unverified: the
loopback read all zeros and the report said so rather than claiming the driver
worked. It works now, with no jumper:

```
c5spiloop: spi0 tx/rx at 1000000 Hz (tie MOSI to MISO for a real test)
  tx: a5 5a 01 80 ff 00 3c c3
  rx: a5 5a 01 80 ff 00 3c c3
c5spiloop: PASS, loopback identical
```

Stable across repeated runs.

### The bug was one argument, and it had already been fixed elsewhere

`gpio_ll_set_output_enable_ctrl(hw, pin, ctrl_by_periph, oen_inv)` — the second
boolean is the **INVERT** flag, not "enable". `spi.c` was passing `(true, true)`,
so the pad was disabled exactly while the peripheral drove it. The master ran a
complete transaction, `trans_done` fired, and the line stayed idle.

**That is the same mistake that cost the RMT driver a whole round in §17c**, where
it produced a different symptom: `ch0_tx_end` set, the symbols correct in memory,
and zero received. Here it produced a silent all-zeros receive. Same one-word
error, two completely different-looking failures — which is why it survived the
first fix.

The canonical form is IDF's own: `gpio_hal_matrix_out()` calls
`gpio_ll_set_output_enable_ctrl(dev, pin, true, oen_inv)` with `oen_inv` normally
false. Every driver in this port now uses that form, and the audit found a third
site:

| file | was | now | symptom |
|---|---|---|---|
| `spi.c` | `(true, true)` | `(true, false)` | loopback read all zeros |
| `rmt.c` | was `(true, true)` | `(true, false)` | tx_end fired, nothing on the pad (§17c) |
| `i2c.c` | `(true, true)` | `(true, false)` | **none** — and that is the interesting one |

`i2c.c` worked with the wrong polarity because its pads are open-drain
(`gpio_ll_od_enable`): a low is pulled down regardless of the enable, and a high is
the pull-up. So the bus worked, and would have kept working — right up until
someone made the pads push-pull or moved that code to a peripheral that needs the
enable. **A latent third instance of the same bug, invisible because the pad
technology masked it.** Fixed anyway.

### The software loopback works too, in the form the S31 file gave up on

Round 11 recorded that routing `spid_out` to the MISO pad and letting the pad
drive its own input produced a constant `0xff`, and concluded that the pad's OE
would have to be wired to the SPI block's output-enable signal — a different
matrix slot. That conclusion was wrong: the `0xff` was the same inverted enable.
With the polarity corrected, the pad drives, its own input reads the shifted data
back through `spiq_in`, and the loopback closes inside the chip:

```
if (inside)
{
    esp_rom_gpio_connect_out_signal(C5_SPI0_MISO_PIN, sig->spid_out, false, false);
    gpio_ll_set_output_enable_ctrl(&GPIO, C5_SPI0_MISO_PIN, true, false);
    gpio_ll_input_enable(&GPIO, C5_SPI0_MISO_PIN);
}
```

So `c5spiloop` now verifies the whole shift path — FIFO, bit order, clock, both
pads — with no wire, and `c5spiloop jumper` still leaves the matrix alone for a
real MOSI→MISO jumper. **What is still not verified: the pad drivers themselves**
(nothing leaves the chip in `inside` mode) and any real peripheral.

**The lesson is not "check the OE polarity".** It is that one wrong argument
produced three different symptoms in three drivers, and the first two fixes were
local — so the third site sat there working by luck. When a bug of this shape is
found, the right response is to audit every call site in the same breath, which is
what should have happened in §17c and did not.

## 17l. Round 23: USB Serial/JTAG — the device side, and what that can't prove

`port/esp32c5/usj.c` brings up the in-chip USB device controller on IO13/IO14 —
the two pads `c5pins` had just asked the other drivers to vacate (§17j). It runs,
and the command is explicit that this is only half a test:

```
c5usj   : conf0=0x00004200 phy_sel=0 pad_enable=1
c5usj   : SOF bit 0 -> 0, frame index 0 -> 0, int_raw=0x00000008
c5usj   : no host traffic - the cable is not connected to a powered host,
           which is expected here and is NOT a driver fault
c5usj   : IN FIFO writable=1
c5usj   : wrote 2 byte(s) into the IN FIFO: accepted
c5usj   : NOT verified: enumeration, CDC-ACM setup, and any byte crossing the
           cable - the USB pads are not broken out on this board
c5usj   : PASS (device side only)
```

**Why this cannot be more than a device-side test.** The C5's USB pads are not
brought out on a connector this session can reach — COM21 is the CH340 UART
bridge, and every other serial port on the host belongs to other boards (COM18 and
COM19 both answer as ESP32-S3). A USB device cannot be proven from its own side of
the cable; claiming "USB works" here would be exactly the mistake §17i called out
when a driver raising `AP_START` was confused with an AP being on the air.

So what *is* checked is everything the device can know about itself:

| check | what it rules out |
|---|---|
| `conf0.phy_sel=0`, `usb_pad_enable=1` | the PHY is on the internal source and the pads are powered — a device with the bus clock on and the pads off is invisible on the cable while every other register reads healthy |
| `sof_frame_index` static, SOF/BUS_RESET clear | the controller is running and simply has no host — this is the correct reading for an unplugged cable, and also what a dead PHY looks like, which is why it is stated rather than scored |
| FIFO writable flag readable and coherent | the register plumbing and the write path work |

**The test had to be rewritten twice, and both times for the same reason: it was
testing the host, not the device.**

1. First version: write 2 bytes, fail if the FIFO refuses. That passed once, then
   failed forever — `usb_serial_jtag_ll_txfifo_flush()` does **not** empty the
   FIFO, it sets `wr_done`, which only hands the current buffer to the link layer.
   With no host consuming anything, the FIFO fills a few bytes into the first run
   and stays full.
2. Second version: flush, wait 100 ms, then write. Still failed, for the same
   reason — waiting does not drain a FIFO nobody is reading.
3. Final: **report** the state instead of demanding an event that requires a host.
   `writable=1` → attempt the write and require it to be accepted. `writable=0` →
   say that this is the expected no-host condition. Repeated runs now behave:

```
c5usj   : IN FIFO writable=1 ... wrote 2 byte(s): accepted ... PASS
c5usj   : IN FIFO writable=0
c5usj   : the IN FIFO is already full - expected with no host draining it,
           and not a fault
c5usj   : PASS (device side only)
```

That distinction — *a test that fails on the second run is testing the wrong thing*
— is the same one behind the `c5pcnt` constant-reading episode in §17d, and worth
keeping in mind for anything that talks to an absent peer.

**One more Xtensa coupling, this time fixed rather than guarded.**
`ft_ipc.c` and `ft_task.c` both read `rt_xt_running_count`, which is defined only
in `libcpu/xtensa/cpuport.c` — so the C5 link failed. Instead of `#ifdef`-ing each
use out, the *question* was moved into `ft_core.h` as `ft_port_running_count()`,
which returns the Xtensa counter there and `rt_thread_self() != RT_NULL` on
RISC-V. Same semantics, one place, and the two call sites no longer know which
architecture they are on. That is the shape the previous seven probe guards should
have had.

**Not verified:** enumeration, CDC-ACM class handling, any byte crossing the cable,
and the console. The console deliberately stays on UART0 — switching it to a port
that cannot be reached from the host would leave the board with no console at all.

## 17m. Round 24: a pre-build gate, and two honest negatives

No new peripheral this round. Three things: a guard against the failure mode that
has cost the most build cycles in this port, and two attempts that failed and are
recorded as failures.

### The gate: `tools/check_xtensa_guards.py`

Nine times now, an actively-developed Xtensa debugging probe has broken the C5
build - `rt_xt_br`, `rt_xt_sst`, `rt_xt_lk`, `rt_xt_lk2`, `rt_xt_nt`, `rt_xt_nf`,
`rt_xt_ii`, `rt_xt_ip3`, `xt_rt_flash_op`. The symbol is defined only in
`libcpu/xtensa/cpuport.c` or the S3 BSP, the C5 is RISC-V, and the failure appears
at **link time**, several minutes into a build, as a wall of undefined references.

The script walks the IDF tree, tracks `#if`/`#ifdef` nesting well enough to know
whether a line is inside an `#if defined(__XTENSA__)` region, and reports every
reference outside one:

```
components/rtthread/port/freertos/ft_notify.c:179: rt_xt_nf -> { extern volatile unsigned int rt_xt_nf[8];
```

It is wired into `build.ps1` as a pre-build gate - about one second, and it names
the file and line before the first object is compiled:

```
clean: no unguarded Xtensa-only references in 15749 file(s) scanned.
Project build complete.
```

`-SkipGuardCheck` bypasses it. **The checker was itself verified both ways**: a
deliberately inserted `rt_xt_canary` reference made it fail and made `build.ps1`
refuse to build; removing the canary made both pass. A tool that has only ever
said "clean" is a tool that has never been tested.

One detail worth recording: the first version flagged three *comments* that
explained the symbols they were about. It now skips comment lines, because
documentation is not a reference and a checker that cries wolf gets ignored.

### Negative 1: RMT RX still does not work, and four attempts failed

This has been open since §17c. Four more things were tried this round, and every
one of them either changed nothing or made it worse:

| attempt | result |
|---|---|
| `rmt_ll_rx_reset_channels_clock_div()` before enabling RX (IDF does this in its channel reset) | no change: `rx_en=1`, frame ends, 0 symbols |
| print `chmconf.conf1` before and after the enable | `conf1 = 0x000001e8 -> 0x000001e9`, i.e. `rx_en_chm` did latch - so the problem is not the enable |
| remove the `printf` between arming RX and starting TX | no change. The prints took ~1 ms each and `idle_thres` at its 15-bit maximum is only 688 us, so this *was* a real race - fixing it just was not sufficient |
| reverse the order: run TX continuously, then arm RX, then stop TX | **identical** - `mem_waddr=97, apb_raddr=96` and 0 symbols read, the same numbers as before |

That last row originally said "worse - from 1 idle symbol captured to 0", and that
was wrong. The write pointer advanced by exactly one symbol in *both* orders; the
"1" I had been quoting is the pointer delta (`mem_waddr = base + 1`), not a count
of received symbols, and the symbols read out were zero in both cases. I asserted
a difference without re-reading the two numbers that would have shown there was
none - the same mistake as claiming a measurement without taking it. Corrected
here rather than quietly dropped, because the wrong version is the kind of thing
that gets quoted later.

**So all four attempts changed nothing.** That ends the investigation for now. The measurements say: the pad is
driven (31 transitions observed), the transmitter runs (`raddr=21`, `tx_end`),
the RX enable latches, the memory block is assigned to HW, and the receiver still
produces nothing. What is left is the RX input path itself - which signal index
`chmconf[0]` actually samples, and whether it is `RMT_SIG_IN0` - and that cannot be
separated without a second, independent edge source on the same pad.

The committed state is the version that captures one symbol rather than zero,
because a regression in a partial feature is still a regression. **RMT is TX-only,
and this is the second round it has stayed that way.**

### Negative 2: PARLIO cannot be done without the GDMA driver

The plan was PARLIO as the next self-testable peripheral, on the strength of
`SOC_PARLIO_TX_SUPPORT_LOOP_TRANSMISSION`. Reading further, that capability is
about a *DMA EOF callback workaround* (`parlio_tx.c:432`, "workaround for
DIG-559"), not an internal loopback - there is no way to route PARLIO TX back to
its own RX inside the chip, so a loopback needs a wire like SPI did.

Worse, PARLIO TX has **no CPU-writable data register**: the peripheral has a
`tx_data_cfg` and a FIFO reset but no `TXFIFO` port, so data can only arrive
through GDMA. `esp_driver_parlio` accordingly allocates a GDMA channel
unconditionally (`gdma_new_ahb_channel` + `gdma_connect`). On this port's C5
build, the GDMA driver is not compiled and `esp_hal_dma/esp32c5/include/hal/
gdma_ll.h` contains exactly **two** functions - bus clock and reset. The channel
machinery lives in `ahb_dma_ll.h` (descriptors, owner bits, EOF modes), which is
a driver's worth of work on its own.

So PARLIO is not a "next peripheral": it is "port the AHB GDMA driver first, then
PARLIO on top". **The GDMA half is done (§17n).**

**What remains, honestly:** RMT RX (needs a second edge source), PARLIO (needs
GDMA first), SDIO slave (needs a host), and the RT-Thread-heap-on-PSRAM decision.

## 17n. Round 25: AHB GDMA, memory-to-memory — the prerequisite PARLIO needed

Last round concluded that PARLIO needs a GDMA driver before it can exist, because
PARLIO TX has no CPU-writable data register. This round built the smallest GDMA
that a peripheral needs, and it works:

```
c5gdma  : AHB GDMA group 0, TX channel 0, mem-to-mem
c5gdma  : completion txint=0x0000000b rxint=0x00000003, owner tx=1 rx=0
c5gdma  : copied 128 byte(s), 0 mismatch(es), 0 byte(s) written past the end
c5gdma  : PASS
```

Verified at 1, 16, 128 and 256 bytes. Three independent things must be right for
that to pass — the descriptor owner/EoF encoding, the channel being started, and
the completion path — and a wrong one produces a visibly different failure. The
test also checks that **nothing was written past the requested length**, which is
the failure mode a wrong descriptor `size` produces and which a plain memcmp would
miss.

### Two things the driver had to learn, both from being wrong first

**1. Memory-to-memory needs BOTH halves of the channel pair.** The first version
connected a TX channel "to memory" and started it. It started, found nothing to
push its data into, and never completed — which reads as "the channel did not
run". IDF's own M2M path (`async_memcpy_gdma.c:121-133`) allocates TX and RX as a
*pair* and connects both to the same trigger. Reading that comment was worth more
than another hour of guessing:

> for M2M mode, hardware will automatically assign peri_sel ID depends on the
> channel number (ch0: 10, ch1: 11, ch2: 12)

So the trigger id is not a free choice and not the `-1` sentinel —
`AHB_DMA_LL_M2M_FREE_PERIPH_ID_MASK` is `0x1C00`, bits 10..12.

**2. Completion is the interrupt status, not the descriptor owner bit.** The
second version waited for `desc.dw0.owner` to return to CPU, which only happens
when auto write-back is enabled. The engine had finished long before the timeout;
the diagnostic that caught it printed:

```
c5gdma  : tx_desc=0x408237a0 rx_desc=0x40823790 tx_owner=1 rx_owner=1
c5gdma  : in_link_addr=0x40823790 out_link_addr=0x408237a0
          txint=0x0000000b rxint=0x00000003
```

`0x0b` is `TX_DONE | TX_EOF | TX_TOTAL_EOF` and `0x03` is `RX_DONE | RX_SUC_EOF` —
**the transfer was complete while the loop was still spinning on the owner bit**.
So "the descriptor never came back" was true and meant nothing.

The lesson is worth keeping beyond GDMA: *a completion condition that depends on
an optional feature being enabled is not a completion condition.* The interrupt
bits do not depend on write-back; the owner bit does. The owner bit is now
reported but not required.

### What this does and does not unlock

**Does:** descriptor chains, channel reset/start/stop, the M2M trigger encoding,
and a verified completion path — everything a peripheral-driven transfer needs
except the trigger itself.

**Does not:** peripheral-triggered transfers. `peri_out_sel` is still the M2M id;
connecting the channel to PARLIO and giving it `SOC_GDMA_TRIG_PERIPH_PARLIO0` (9)
is the next step and is not claimed here.

**Still open, unchanged:** PARLIO on top of this, RMT RX (needs a second edge
source, which this DMA can now provide), SDIO slave, and the RT-Thread-heap-on-PSRAM
decision.

## 17o. Round 26: RMT RX works — the carrier demodulator was on

`c5rmt` has been TX-only since round 14: the transmitter was proven four ways and
the receiver captured zero symbols. It passes now, both directions:

```
c5rmt   : pad self-check IO0: drove 1 -> read 1, drove 0 -> read 0 (pad and input path OK)
c5rmt   : hardware wrote 19 symbol(s) (waddr=115 base=96)
c5rmt   : tx 21 symbols built (20 pulses + stop), 4761 tick(s)/half = 100 us, tick=21 ns
c5rmt   : rx 19 symbol(s), 38 pulse(s), mean half = 4761 tick(s)
c5rmt   : PASS
```

Verified at 50, 100 and 200 us half periods: 2380 / 4761 / 9523 ticks, each
matching the request.

### Four bugs, found in this order, and only the first is the headline

**1. The RX carrier demodulator is ON out of reset.** `chmconf0.carrier_en_chm`
has **default 1** on this chip (`rmt_struct.h:156`). With it enabled the receiver
modulates what it samples with the carrier generator instead of passing the
incoming signal through — so it runs, latches its frame-end, and produces nothing
usable. IDF turns it off explicitly in `rmt_rx.c:279`.

**This is the same trap as the TX side in §17c**, where `chnconf1.carrier_en_chn`
also defaults to 1. I disabled it on TX three rounds ago and **never looked for the
RX counterpart**, because the RX failure looked like a signal-routing problem while
the TX failure had looked like a timing problem. Same silicon, same default, two
completely different-looking bugs. One line:

```c
rmt_ll_rx_enable_carrier_demodulation(s_rmt, C5_RMT_RX_CH, false);
```

**2. The test's own square wave was not a square wave.** Round 14 built it by
alternating the level per symbol:

```
sym0: H L    sym1: L H    -> H L L H H L L H ...
```

Each level then lasts two half-periods, and a correct receiver merges the adjacent
equal-level segments. Measured: the first captured symbol was `4761/9522` and every
later one `9522/9522` — exactly double, which reads like an RX clock divider
problem and is not. **A symbol is already a pair of levels**, so a clean square
wave is one repeated pattern, not two alternating ones.

**3. "Count symbols until a zero word" reads stale memory.** RMTMEM is not cleared
between frames, so everything past the frame end is a leftover from a previous run.
Measured with that loop: **48 symbols returned**, of which the first 19 were
exactly `4761/4761` and the rest were random durations — mean 12261 ticks, "2.5x
too slow". The hardware had written 19. The fix is to read
`chmstatus[].mem_waddr_ex_chm` and subtract the block base
(`C5_RMT_RX_MEM_CH * 48 = 96`):

```
hardware wrote 19 symbol(s) (waddr=115 base=96)
```

**4. A 1 ms printf inside a 688 us window.** The diagnostics from the earlier
rounds sat between arming RX and starting TX. Each `rt_kprintf` over the 115200
console takes about a millisecond, and the RX idle threshold at its 15-bit maximum
is 688 us — so the receiver timed out on the idle line **before the transmission
began** and latched one idle symbol. That is the real explanation of the "captured
exactly one symbol" result that three rounds of investigation were built on.

### What the mean is measured over, and why it matters

The last captured symbol carries the idle gap, so the mean is taken over pulses
**shorter than four half-periods** rather than over all symbols:

```c
if (s.duration0 && s.duration0 < ticks * 4u) { pulses++; sum_ticks += s.duration0; }
```

That is not a fudge to make the number pass — it is the difference between "the
mean duration of the edges" and "the mean duration of the edges plus however long
the line stayed idle", and a receiver's job is the former.

**Not verified:** the pad driver itself (the loop closes inside the GPIO matrix, so
nothing leaves the chip), and any real RMT peripheral such as a WS2812 or an IR
receiver. What is verified is the controller: symbol encoding, both directions, the
memory window, the completion path, and that the received durations equal the
transmitted ones.

## 17p. Round 27: PARLIO — driver written, transmit apparently dead (resolved in §17q)

PARLIO was the last peripheral in this port that could in principle be proven on a
bare board. The driver exists, the GDMA feeds it, and **the unit never starts
clocking**:

```
c5parlio: 16 byte(s) at 1000000 Hz, clk=IO13 data=IO14
c5parlio: no EOF after 200 ms: int_st=0x00000000 st=0x00000000 dma_txint=0x0000000b peri_out_sel=9
c5parlio: FAIL, transmit (-2)
```

### What the numbers already rule out

This is a failure with real information in it, not a silent one:

| value | meaning |
|---|---|
| `peri_out_sel=9` | the GDMA is connected to `SOC_GDMA_TRIG_PERIPH_PARLIO0` and the write took |
| `dma_txint=0x0b` | `TX_DONE \| TX_EOF \| TX_TOTAL_EOF` — the DMA channel ran to completion |
| `PCR.parl_clk_tx_en=1` | the PARLIO core clock is on |
| `tx_data_cfg=0x10010000` | decodes to `bitlen=128, bus_width=1, order_inv=1` — the frame is configured |
| `st=0x00000000` | **`tx_ready` never set, so from the unit's side the FIFO never filled** |

So: the DMA's AHB writes completed and the peripheral did not accept them. The
fault is in this driver's PARLIO setup, and the GDMA half — which is proven
separately by `c5gdma` — is not suspect.

### Four wrong guesses, and what each one cost

Recorded because they are all plausible and three of them were mine:

1. **"The clock must be on before the DMA."** Plausible (a gated FIFO cannot
   accept data) and wrong: with the clock on first, the result was identical.
2. **"Follow IDF's transaction order exactly."** Also failed — and the reason is
   worth keeping: IDF's order **toggles** a clock that its channel-construction
   path (`parlio_tx.c:237`) already enabled. Following the transaction sequence
   from a *cold* peripheral is not the same thing, and the order was never the
   problem.
3. **"Enable GDMA burst mode."** IDF sets `max_data_burst_size = 32` for PARLIO
   and the M2M test needed none. No change.
4. **"The GDMA register clock is off."** — **this one was real.** `peri_out_sel`
   read back 0 after writing 9, because `ahb_dma_ll_force_enable_reg_clock()` is
   only called from `rt_hw_gdma_init()`, which only the M2M self test called.
   Every write to a channel register went nowhere. Fixed by having the PARLIO
   driver initialise the block it uses rather than assuming another driver did:

   ```c
   /* A peripheral driver is not entitled to assume another driver initialised a
    * shared block. */
   rt_hw_gdma_init();
   ```

   That took the failure from "nothing happens at all" to "the DMA completes and
   the peripheral ignores it", which is a strictly more informative state — and is
   the state it is stuck in.

**One conclusion I had to withdraw.** After attempt 1 I wrote that "the clock has
to be on before the DMA can fill the FIFO". That was wrong; the real cause was the
missing GDMA init, and I had inferred a clock-ordering rule from a failure that had
a different cause entirely. Kept in the report rather than deleted, because the
same inference would be made again from the same three zeros.

### Also removed: a wait that can never end

`parlio_ll_tx_is_ready()` reads `st.tx_ready`, which means "the FIFO can accept
*more* data". Once the DMA has pushed the whole 16-byte frame in — which it has —
that bit is legitimately 0, so waiting for it to become 1 waits forever. IDF can
wait there because its DMA is still streaming; here the frame is already in. The
wait is gone.

### Where this leaves PARLIO

Written, compiling, routed to pads, fed by a working GDMA, **not transmitting**.
The one measurement that would settle it — does the clock line move at all — is
exactly what the self test cannot reach, because PARLIO's own `st` register is the
thing that is not responding. A scope on IO13 would answer it in a second;
failing that, the next thing to try is feeding the TX FIFO from the CPU side
entirely and seeing whether `tx_ready` ever moves, which isolates "the FIFO never
accepts data" from "the DMA never delivers it".

**Stated plainly: PARLIO was not working at the end of round 27.** **Resolved in
§17q** — it was transmitting the whole time; the completion register I was
polling could not report it.

## 17q. Round 28: PARLIO works — the polled register was masked, then misread

Last round ended with "PARLIO does not transmit". It transmits. Two separate
mistakes, both mine, both in the *measurement* rather than the peripheral:

```
c5parlio: 16 byte(s) at 1000000 Hz, clk=IO13 data=IO14
c5parlio: control - 0 edge(s) in 200 ms with the unit idle
c5parlio: unit read 127 of 128 bit(s) out of its FIFO, tx_cnt=127, dma_fifo_bytes=0
c5parlio: the unit read up to bit 127 (expected 127 for 128 bit(s)); PCNT counted 5974 edge(s) (control 0)
c5parlio: PASS
```

Verified at 1, 8, 16 and 32 bytes and at 1 MHz and 2 MHz.

### Mistake 1: `int_st` is masked, and the mask was zero

`parlio_ll_tx_get_interrupt_status()` returns **`int_st`**, and `int_st` is
`int_raw & int_ena`. This driver never wrote `int_ena`. So every poll returned
`0x00000000` **no matter what the peripheral did** — the loop condition could not
have become true. One line fixed it:

```c
parlio_ll_enable_interrupt(s_parlio, PARLIO_LL_EVENT_TX_MASK, true);
```

The cost of that omission was the whole of round 27. Every "no EOF" reading in
last round's report was produced by a register that was structurally incapable of
reporting an EOF.

### Mistake 2: the counter I then trusted is 0-based and wraps

Having fixed the mask, I read `tx_st0.tx_fifo_rd_bit_cnt` as "bits consumed" and
checked `== len * 8`. That failed on frames the hardware had transmitted
correctly. The register is documented as *"the current read bit number"* and that
is literal — a **0-based index that wraps at 128**:

| frame | measured | arithmetic |
|---|---|---|
| 8 bytes (64 bits) | 63 | 64 − 1 |
| 16 bytes (128 bits) | 127 | 128 − 1 |
| 20 bytes (160 bits) | 31 | 160 − 1 − 128 |
| 32 bytes (256 bits) | 127 | 256 − 1 − 128 |

Two consecutive wrong readings of the same register — first "the unit consumed
nothing" (it was read *after* `tx_start(false)`, which **clears `tx_st0`**), then
"the unit consumed the wrong amount" (semantics misread). The check is now
`(bits - 1) % 128`, and the test says out loud that above 128 bits it no longer
constrains the frame length:

```
c5parlio: note - at 256 bit(s) the counter wraps, so this confirms the unit read
          data but not the exact frame length
```

### What the control run bought

Before any of this, "PCNT counted 5974 edges" was not evidence — PCNT's glitch
filter is off in this port (§17d) and a floating pad can produce counts. The test
now counts edges for 200 ms **with the unit idle, first**:

```
c5parlio: control - 0 edge(s) in 200 ms with the unit idle
```

Measured before the attempt, so it cannot be used to explain away a result after
the fact. 0 idle edges means the 5974 are real; had it been 5000, the same output
would have proved nothing and the test now says so.

### Why the count is 5974 and not 128

PARLIO keeps clocking between the last data bit and the moment this CPU resumes
from the EOF poll — there is no way to stop it inside that window. So the edge
count is a **lower bound with an arbitrary tail**, not a frame-length measurement,
and the test treats it as such: it must exceed the control, and the exact claim
comes from the hardware's own FIFO counter instead. A test that demanded
`edges == bits` would fail a working transfer, which is what it did for two
attempts.

### What is and is not verified

**Verified:** the GDMA delivers the frame, the unit pulls every bit out of its
FIFO, the clock line runs (against a zero-edge control), and the frame completes.
**Not verified:** the levels on the data line. Counting clocks says the shift
engine ran; it says nothing about whether TXD0 carried the supplied bits. That
needs a second observation point the chip does not offer for a signal that only
ever leaves.

## 17r. Round 29: TWAI as an `rt_can` device — and five ordering bugs

`can0` is registered and a frame driven through the framework comes back:

```
c5can   : PASS (id=0x123, 4 byte(s), round trip through `can0` at 250 kbit/s;
                the analog layer is still unverified)
```

`list device` now shows `can0 CAN Device`. The register-level `c5twai` still passes
at all four rates, which matters: these two tests share one controller, and the
device class must not consume the events the register test looks at.

### What went wrong, five times, and the shape they share

Every one of these was an **ordering** problem that produced a plausible-looking
symptom somewhere else:

**1. `sendmsg` returned the payload length.** `_can_int_tx()` tests
`can->ops->sendmsg(...) != RT_EOK` and treats anything else as a failure — so a
successful send looked rejected, `rt_device_write` returned 0, and the frame was
never even waited on. The contract is "0 means armed".

**2. The interrupt mask was enabled, then wiped.** `twai.c`'s init calls
`twaifd_ll_reset()`, which clears `int_ena`. Both files export at
`INIT_BOARD_EXPORT`, and **within one stage the order follows link order, not
source order** — so `can.c` could arm the mask and have `twai.c` wipe it
afterwards. The frame was armed and the TX-complete interrupt never arrived;
`rt_device_write` then returned 0 after the framework's completion timeout. Fixed
by arming from `configure()`, which runs on the first `rt_device_open()` and is
therefore strictly after every init stage.

**3. `INIT_DEVICE_EXPORT` was the wrong stage.** The init order is
BOARD → PREV → DEVICE → COMPONENT → ENV → APP, so a DEVICE-stage call ran before
`twai.c` had set its controller pointer at BOARD stage. `list device` showed no
`can0` and the only message was "FAIL, device registration".

**4. `s_can0.config.baud_rate` was read before it was written.** A zero-initialised
struct member passed to `set_bitrate()` comes back `-RT_EINVAL` (-10), which looks
exactly like a rejected parameter rather than an uninitialised one.

**5. The controller was not told to rejoin the bus.** `rt_hw_twai_set_bitrate()`
takes the controller off the bus, reprograms the timing and puts it back — and it
then needs 11 consecutive recessive bits before accepting a frame. `c5twai` calls
`rt_hw_twai_start()` after every rate change for exactly this reason; the device
class did not, so the frame went to the Failed state and "no frame came back".

The shared lesson: **in a port where two drivers own one peripheral, the export
stage and the link order are part of the contract.** None of these five failures
pointed at the line that was wrong.

### One regression I introduced, and caught

The first version enabled `TWAI_LL_DRIVER_INTERRUPTS` — which includes
`FSM_CHANGE` — and its ISR cleared the whole status word. `c5twai` polls exactly
that bit to know the controller joined the bus, so a test that had passed since
round 12 started reporting:

```
c5twai  : 125000 bit/s: no FSM transition (-2), continuing
```

Now the mask covers only the three events the adapter handles
(`RX_NOT_EMPTY`, `RX_FULL`, `TX_DONE`) and each is cleared at its own site. A blind
`clr_intr_status(status)` also clears `ERROR_WARNING`, `BUS_ERROR` and
`ARBIT_LOST` — bits this class has no business consuming.

### Two build-level accommodations, both narrow

`dev_can.c` is upstream code that assumes a build without `-Werror`:

* its `/* FALLTHROUGH: ... */` marker is not one of the spellings GCC accepts at
  `-Wimplicit-fallthrough=3`;
* it exports msh commands through `MSH_CMD_EXPORT_ALIAS`, which hits the
  `cast-function-type` warning that `i2c.c` and `finsh` already suppress.

Both are suppressed for **that one file**, not globally.
`dev_can.c` also needs `ipc/ringbuffer.c`, which nothing in this port had pulled
in — five undefined `rt_ringbuffer_*` symbols at link time.

**Not verified:** the analog layer, unchanged from §17f. No transceiver, no
differential pair, no second node, no arbitration.

## 17s. Round 30: deep sleep, and the 8 MB does not survive it

Two questions came out of the same place - "the part has an HP and an LP core,
does the LP side need more work" and "you did not seem to detect the in-package
flash/PSRAM". Both are answered by measuring, and one of the answers is a warning.

### What the package actually contains

Measured, not assumed:

```
Chip type:          ESP32-C5HR8 (revision v1.2)
Detected flash size: 16MB          (esptool flash-id)
Manufacturer: 68  Device: 4018     (in-package flash, not an external chip)
c5psram : initialised=yes, reported size=8388608 byte(s)
c5psram : IDF heap: internal=146576, spiram=8388608, total=8535184 byte(s)
```

So the "R8" half is real and enabled: 8 388 608 bytes are mapped, and 7 168 000
of them were written and read back byte-exact in one allocation. The whole 8 MB
cannot be held in a single block because the allocator's metadata and the heap's
own bookkeeping come out of the same region - `c5psram test 8100` correctly reports
`heap_caps_malloc(8294400) returned NULL` rather than pretending.

**A hard-coded 1 MB cap was hiding this.** The self test used to clamp its own
argument to 1024 KB and then print `wrote and read back 1048576 byte(s)`, so
`c5psram test 7680` looked like it had exercised 7.5 MB and had exercised an
eighth of it. It now clamps to the reported size and says so.

### Deep sleep works, and takes the PSRAM with it

```
lp      : deep sleep for 3 s
lp      : RTC marker set, entering deep sleep - the verdict appears on the NEXT boot
lp      : woke from deep sleep, cause=4 (timer), 1 sleep(s) so far
lp      : PSRAM +0 is 0x14, wrote 0x31
lp      : PSRAM did NOT retain its contents: crc 0x02b86c3f, wrote 0x9dba3655
```

That is a genuine result and the important one for anyone planning to use the
8 MB: **the PSRAM is powered down in deep sleep and comes back holding garbage.**
A pointer into it does not survive a sleep, and neither does the data. Anything
that has to cross a sleep must live in RTC slow memory, or be re-initialised on
wake.

The measurement had to be built twice. The first version printed
`PSRAM held its contents: NO (crc 0x9dba3655)` from a variable it never set, and
`cause=4069716927` from `esp_sleep_get_wakeup_cause()` called **before** the sleep -
a wake cause is only meaningful after a wake. Both were "NO"-shaped answers with no
measurement behind them, which is worse than no answer: they look like findings.

The corrected version saves the PSRAM block's **address and length** in RTC memory
and re-reads those physical bytes on wake. PSRAM on this part is a fixed
memory-mapped region, so the same address is the same silicon after a reset; going
through the allocator again would test the allocator, not retention.

### What is still not proven about the low-power side

* **Current consumption.** Nothing on this board measures it. Not estimated.
* **Wake sources other than the RTC timer.** GPIO, LP UART and LP I2C wake are all
  listed as supported and none is wired up here.
* **Light sleep**, which keeps the HP core alive and is a different mechanism from
  the one just measured.
* **LP peripherals.** `SOC_LP_I2C_SUPPORTED` and `SOC_LP_UART_SUPPORTED` are set,
  but reaching them needs an LP-domain pad mux (`LP_IO`) and `rtc_io` layer that
  this port does not have, and I started that file and removed it rather than
  leave a half-written driver claiming support.

**So: the LP core executes (§17k) and deep sleep works (this section). The LP
peripherals do not exist yet, and that is the honest boundary.**

## 17t. Round 31: the LP domain — one real finding, one honest failure

The request was "port the LP/ULP side". Two of the three things it needed are done
and the third is not, and the boundary is worth being precise about.

### 1. Pad probing, which the LP work forced

`c5probe` drives every pad and reads it back. A pad the package does not bring out
never follows its own output, so this separates "exists" from "does not":

```
c5probe : IO0  follows (1 -> 1, 0 -> 0)
...
c5probe : IO23 follows ...  IO28 follows
c5probe : 6 of 6 pad(s) respond
```

All 29 respond. That mattered immediately: the LP pads are a **subset**.

`rtc_io_periph.c` maps only **GPIO0..GPIO6** to RTC/LP channels; IO7 upwards are
`-1`. And `i2c_ll.h` fixes LP I2C to IO2/IO3 outright:

```
#define LP_I2C_SDA_IOMUX_PAD 2
#define LP_I2C_SCL_IOMUX_PAD 3
```

with `i2c_common.c:376` refusing anything else ("SCL pin can only be configured as
GPIO#3"). This port had **SPI MISO on IO2 and the PWM test on IO3**. Both are
re-routable, LP I2C is not, so SPI MISO moved to IO23 and PWM to IO24 — verified
after the move:

```
c5spiloop: PASS, loopback identical
c5pwm   : PASS
c5pins  : PASS, no pad is claimed twice
```

### 2. LP I2C: written, and the FSM never starts

`lp_i2c0` registers as an `rt_i2c` bus and the pads are handed over correctly. The
controller then does nothing:

```
c5lpi2c: after init ctr=0x00000100 (clk_en=1) LPPERI.clk_en=0x59000000 (lp_ext_i2c_ck_en=1)
c5lpi2c: LP_I2C.date=0x02410160
c5lpi2c: idle levels via LP: SDA=1 SCL=1 | via HP: SDA=1 SCL=1
c5lpi2c: pre-start fifo_conf=0x00004088 ctr=0x00000100 cmd0=0x00003000
c5lpi2c: timeout dump ctr=0x00000100 status=0x00000002
c5lpi2c: cmd[0]=0x00003000 ... cmd[4]=0x00002000   (all done=0)
```

**What is ruled out by those numbers**, each of which was a separate hypothesis:

| hypothesis | ruled out by |
|---|---|
| the register block is not reachable | `date` reads `0x02410160`, a constant in the silicon |
| the LP peripheral clock is off | `LPPERI.clk_en.lp_ext_i2c_ck_en == 1` |
| the controller's own gate is off | `ctr.clk_en == 1` **after** init |
| the pads are held low | both idle high through their pull-ups, both paths agree |
| the command encoding is wrong | the five words match the HP driver's, which works |
| the FIFO/FSM reset bits are stuck | removing them changes nothing |
| the clock-source select is wrong | `LP_CLKRST.lpperi.lp_i2c_clk_sel` set to XTAL/2 |

`int_raw` reads `0x00000002`, which is `txfifo_wm_int_raw` — its **reset default**,
not an event. So the controller never saw a start condition, never clocked, and
never raised anything. `trans_start` is write-only and cannot be read back to
confirm it latched.

**Three things that are specifically notable:**

* `i2c_ll_enable_controller_clock()` **does nothing for LP_I2C**:

  ```c
  if (hw == &LP_I2C) { // Do nothing
      return;
  }
  ```

  The HP controller gets a PCR gate; the LP one gets nothing. Its clock enable is
  `ctr.clk_en`, which nothing in `i2c_ll.h` sets. That is now set directly, and it
  is the one real fix this section produced — before it, `ctr` read `0x00000208`
  with `clk_en` clear.

* `LP_I2C.ctr` has **no `ms_mode` field at all** — the LP I2C is master-only, so
  `i2c_ll_set_mode(hw, I2C_BUS_MODE_MASTER)` writes a bit that does not exist. It
  is harmless, and it explains the `ms_mode=0` reading that looked alarming.

* **Two different pull-up layers.** `rtcio_ll_pullup_enable()` drives the LP_AON
  block; on the C5 the pad's physical pull-up for IO2/IO3 is in the HP IO_MUX. With
  only the LP one enabled the lines read **0**, and the first version of this test
  reported "the pads are not released" for pads that were fine. Both are enabled
  now.

**Where it stands: not working, and the remaining cause is something the LL does
not expose.** IDF's own driver reaches LP I2C through `i2c_common.c`, and every
clock call it makes is one this driver already makes; there is no fourth gate to
find in the LL. The next real step is either a scope on IO3 (does the clock line
ever move?) or comparing against a build of IDF's own `i2c_master` on this board —
both outside what I can do on a bare board with no LP slave.

### 3. LP UART: not attempted, deliberately

`SOC_LP_UART_SUPPORTED` is set and `UART_LL_GET_HW(2)` is `&LP_UART`, so the same
route exists. I started it and removed the file rather than leave a second driver
sitting at the same unexplained failure — the point of §17s and this section is
that the LP peripherals are blocked on one unknown, and writing more of them does
not answer it.

**Stated plainly: the LP core executes (§17k), deep sleep works (§17s), the pad
layer is mapped and probed (here), and the LP I2C controller is reachable but does
not start.** That is the honest boundary.

## 17u. Round 32: ADC voltage conversion — the calibration was already in the image

`c5adc` reported raw codes for thirty rounds because "the calibration coefficients
come from the ADC calibration component, which is not part of this port". That was
wrong in a specific way: **the component was already compiled into every image.**

```
build/esp-idf/esp_adc/libesp_adc.a
  adc_cali.c.obj  adc_cali_curve_fitting.c.obj  curve_fitting_coefficients.c.obj
  00000000 T adc_cali_create_scheme_curve_fitting
```

It was linked and then dropped by `--gc-sections`, because nothing referenced it.
`nm c5_rtt_full.elf | grep adc_cali` returned nothing while the archive had the
symbols. That is the same failure this port keeps recording, from a new direction:
not "the code is missing" but "the code is present and unreachable".

### What it reads now

```
c5adc   : eFuse calib version=1, init_code=0x07af, cal point=2276 code -> 2300 mV (ok)
c5adc   : unit 1, 6 channel(s), 12-bit, atten 12 dB, calibrated full scale 4131 mV
c5adc   : ch0 (GPIO1) raw=2613 then 2599, max=4095, 2639 mV (calibrated)
c5adc   : ch3 (GPIO4) raw=4095 then 4095, max=4095, 4131 mV (calibrated)
```

The eFuse line is the provenance, and it is printed for a reason: `version=1`
means this **part** carries factory calibration, with a single reference point
(2276 counts = 2300 mV) that the curve fit is anchored to. If a part came back
with `version=0`, the component would fall back to run-time self-calibration and
the test now says so instead of quietly reporting numbers of a different quality.

### Two fixes, and the second is the one that matters

**1. `get_vref()` can be honest now.** RT-Thread models a conversion as a straight
line, `value * vref / (2^bits - 1)`, while this chip is calibrated by a curve with
per-channel compensation. The vref is derived from the calibration at **full
scale** — 4131 mV — which makes that line exact at the top of the range and close
in between. It is an approximation and the header says so. The exact per-channel
value comes from `rt_hw_adc_raw_to_mv()`, which calls
`adc_cali_raw_to_voltage()`.

**2. The hardware offset code was never applied.** This is the part that would
have been wrong even with perfect curve fitting. IDF's `adc_oneshot_read()` does
this on **every** conversion (`adc_oneshot.c:237-238`):

```c
adc_hal_calibration_init(handle->unit_id);
adc_set_hw_calibration_code(handle->unit_id, atten);
```

The port had neither. The raw codes therefore carried the SAR's offset error, and
the curve fit was correcting a number that had not been corrected upstream of it.
Applied per conversion now, at two register writes per read.

### What is still not proven

**Accuracy.** There is no way to inject a known voltage from this board — no DAC,
no reference, and the pads that are free enough to probe are being driven by other
peripherals in this image. What IS proven is: the calibration data exists on this
part and is the right version, the conversion path runs and returns millivolts,
and the numbers move with the input rather than sitting at a constant.

A real accuracy claim needs a bench supply on one channel and a comparison against
a meter. Stated rather than approximated.

**Also unchanged:** channels 0-2 sit on pads the rest of this image drives (GPIO1/2
are LP_UART, GPIO3 is the PWM test), and GPIO4 reads full scale because the CAN
driver parks it for analog. The readings from those four are real measurements of
those pads, not of an external signal.

## 17v. Round 33: `rt_malloc()` reaches the 8 MB

The last thing listed as "not done" for thirty rounds. It works now, and the
interesting part is that **the obvious configuration does not work at all**.

```
c5heap  : PSRAM heap registered, 8126464 byte(s); window 0x42000000..0x44000000
c5heap  :    1024 B  at 0x40833020  SRAM     content OK
c5heap  :   16384 B  at 0x40836450  SRAM     content OK
c5heap  : 1048576 B  at 0x421409dc  PSRAM    content OK
c5heap  : 4194304 B  at 0x421409dc  PSRAM    content OK
c5heap  : 8 x 512 KB held simultaneously
c5heap  : PASS
```

### The constraint nobody documents

The plan was to keep `RT_USING_SMALL_MEM_AS_HEAP` and register a second memheap
for the PSRAM. **That silently does nothing**, and the reason is in `kservice.c`:

```c
#if   defined(RT_USING_SMALL_MEM_AS_HEAP)
...
#elif defined(RT_USING_MEMHEAP_AS_HEAP)      /* <- the fallback lives in here */
...
#endif
```

The walk over "every other memheap" is inside `_memheap_alloc()`, which only
exists under `MEMHEAP_AS_HEAP`. With small-mem as the heap, a second memheap
appears in the object list and `rt_malloc()` never looks at it. Nothing warns.

So the system heap itself had to become a memheap. **The trade, stated plainly:**
the 65 KB internal region now pays a ~40-byte header per allocation instead of a
few bytes. In exchange `rt_malloc(4 MB)` succeeds. Measured effect on memory use:
system heap `used` went 17088 → 17088 after the test ran and freed, with `max`
rising from 20864 to 33504 - so the running peak costs about 12 KB more, not 40
bytes per live object multiplied by everything.

### Why not one merged pool

Merging internal SRAM and PSRAM into a single allocator has to pick one, and both
choices are wrong here:

* small-mem over PSRAM: every allocation pays the SPI-cache penalty, on every
  access to the object too;
* memheap over 65 KB: 40 bytes of header per allocation in the region that is
  tightest.

Two pools with small-first fallback gives the behaviour actually wanted, and the
test asserts **both directions** - a test that only checked "a big allocation
works" would pass even if everything went to PSRAM, which would be the wrong
outcome.

### Why the test writes a pattern

A pointer into the PSRAM window that is not backed by the part reads back whatever
the bus returns, and an allocator bug that overlaps blocks reads back the other
block's data. The pattern `(i * 31) ^ (i >> 8) ^ 0x5A` differs at every offset, so
a short block, an offset error or an overlap all show up as a mismatch instead of
a plausible run of zeros.

### What this does NOT prove

* **PSRAM under DMA.** The SPI DMA path to PSRAM is not exercised; `c5gdma`
  transfers from internal RAM only.
* **PSRAM as executable memory.** Not attempted.
* **The cache-coherency rules.** Anything the SPI peripherals DMA into or out of a
  PSRAM buffer needs `esp_cache_msync()` around the transfer. Nothing in this port
  does that yet, because nothing DMAs to PSRAM yet.
* **Wi-Fi throughput is unchanged by this.** Re-measured after the switch:
  **21822 kbit/s** on 5 GHz against 23686 before - within run-to-run noise, and
  notably the heap change did not cost throughput even though lwIP's buffers now
  come from memheap.

## 18. Both bands, measured separately



### 18.1 Which APs exist, and their names are not what they look like

`c5bands` prints the **raw SSID bytes** of every record; the console cannot render
UTF-8, so printing the name is not evidence:

```
bands   : 31 access point(s) heard
bands   : 2.4 GHz=22, 5 GHz=9, other=0
bands   : ssid[1] ch=60  len=21 : e5 88 bb e4 ba 8e e6 99 b4 e7 a9 ba ... 5f 35 47
bands   : ssid[2] ch=1   len=23 : e5 88 bb e4 ba 8e e6 99 b4 e7 a9 ba ... 5f 32 2e 34 47
```

`5f 35 47` is `_5G`, `5f 32 2e 34 47` is `_2.4G`. The two hotspots broadcast
**different SSIDs with the band in the name**:

| Band | SSID | Channel | BSSID | Board RSSI |
| --- | --- | --- | --- | --- |
| 2.4 GHz | `刻于晴空之上_2.4G` | 1 | `d4:ee:07:40:1f:26` | -53 to -57 |
| 5 GHz | `刻于晴空之上_5G` | 60 | `d0:ee:07:40:1f:27` | -56 to -57 |

No AP broadcasts the bare `刻于晴空之上`; joining that returns `reason=201`
(NO_AP_FOUND), which is correct.

### 18.2 Both bands, all four measurements (at the 5760-byte window)

| Measurement | 2.4 GHz (ch 1) | 5 GHz (ch 60) |
| --- | --- | --- |
| Association + DHCP | PASS, `10.0.0.195` | PASS, `10.0.0.195` |
| ICMP from host | 4/4, 3-54 ms | — |
| TCP round trip (12 B each way) | PASS, both directions | PASS, both directions |
| Sustained send | 4769 kbit/s (2.98 MB / 5 s) | 8864 kbit/s (5.54 MB / 5 s) |
| Sustained receive | 2077 kbit/s (401 KB) | 7200 kbit/s (401 KB) |
| Driver TX rejections | 2461 ok, 0 rejected | 4526 ok, 0 rejected |

5 GHz is ~1.9x faster outbound and ~3.5x inbound — what 80 MHz VHT on ch 60 versus
20 MHz on ch 1 should give. RSSI is essentially identical on both, so this is
bandwidth, not signal.

## 19. Final verification

```
step1   : ble_init=0
wifi    : GOT_IP, associated 192.168.95.71
c5all   : PASS (Wi-Fi associated, BLE advertising)

tcp     : sent 12 byte(s)
tcp     : received 12 byte(s): pong-from-pc
# host: received 12 byte(s): ping-from-c5   RESULT: PASS

tput    : 1645440 bytes in 5000 ms = 2632 kbit/s, 4535 window stall(s)
txstats : ok=1427 nomem=0 other=0

rx      : 401408 bytes in 1375 ms = 2335 kbit/s (checksum 0x88306)

found 14:C1:9F:8A:11:3A rssi=-41 name='c5-rtt'
identity characteristic: 8ec90002-... = b'C5RT'
notifications: 7 [3, 4, 5, 6, 7, 8, 9]
RESULT: PASS

c5rt    : PASS   (tick delta 3000, threads advancing, pin readback)
c5flash : PASS   (0 mismatched bytes across the cache-off window)
c5pinirq: PASS   (5 rising edges, 5 interrupts)

pwm0    : PASS at 1 / 20k / 100 Hz (register read-back)
i2c0    : nack=112 timeout=0 in 13 ms (bus driven, nothing attached)
adc1    : PASS, all 6 channels convert
```

## 20. Running it

```
powershell -File tools\build.ps1 -Project c5-rtt-full build
cd c5-rtt-full\build
esptool --chip esp32c5 -p COM21 -b 460800 write-flash --flash-mode dio \
        --flash-freq 80m --flash-size 16MB \
        0x2000 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin \
        0x10000 c5_rtt_full.bin

python tools\c5seq.py COM21 --wait 25 "c5all 0" "c5iperf <host-ip> 4444 5" "c5stat"

python tools\tcpsink.py 4444 40        # TCP sink for c5iperf/c5tcp
python tools\tcpsrc.py 5555 400000     # TCP source for c5rx
python tools\tcpecho.py 3333           # one-shot echo peer for c5tcp
python tools\blehold.py 40 c5-rtt      # hold a BLE link, count notifications
python tools\blegatt.py c5-rtt         # enumerate services, read, subscribe
python tools\blescan.py 15             # watch for the advertisement
```

Board commands:

| Command | Purpose |
| --- | --- |
| `c5all [hold]` | bring Wi-Fi + BLE up together |
| `c5stat` | tick, heaps, radio state |
| `c5tcp <ip> [port]` | one TCP round trip |
| `c5iperf <ip> [port] [s]` | sustained send rate |
| `c5rx <ip> [port] [s]` | sustained receive rate |
| `c5bands` | full dual-band scan with raw SSID bytes |
| `c5join2` / `c5join5` | join the 2.4 GHz / 5 GHz AP |
| `c5pwm [pin] [hz] [duty%]` | LEDC self test with register read-back |
| `i2cscan [port]` | I2C scan with NACK/timeout split |
| `c5adc` | read all six ADC channels |
| `c5bstall` / `c5tcpstate` / `c5txstats` | transmit diagnostics (§10, §14) |

Current combined image: **1,340,128 B** of a 2 MB app partition (63%).
HP SRAM: ~168 KB of 320,928 B (52%).

Memory as finally configured (`c5-rtt-full/sdkconfig.defaults`):

| Item | Value | Why |
| --- | --- | --- |
| `CONFIG_IDF_RTTHREAD_HEAP_SIZE` | 65536 | 128 KB starved the Wi-Fi TX pool (§14) |
| `CONFIG_IDF_RTTHREAD_INTERRUPT_STACK_SIZE` | 16384 | was 24 KB, same reason |
| `CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER_NUM` | 32 (default) | needs ~51 KB of IDF heap |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | 32768 | was 5760; §15.1 |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | 32768 | was 5760 |
| `CONFIG_LWIP_TCPIP_CORE_LOCKING` | off | neutral in retest, §15.2 |
| IDF heap free, both stacks up | ~97 KB | measured with `c5stat` |
| RT-Thread heap used | 17 of 64 KB at rest, 33 KB peak | `rt_malloc()` reaches the
8 MB PSRAM for large blocks since §17v |

## 21. What is not done

**Device drivers — seven RT-Thread devices, and eleven self-tested peripherals**

`list device` reports `uart0`, `pin`, `pwm0`, `i2c0`, `adc1`, `spi0`, `pcnt0`, and
every one of them has a self test that measures something rather than reporting
success:

| what | how it is checked | section |
| --- | --- | --- |
| `uart0`, `pin`, `pwm0`, `i2c0`, `adc1`, `spi0`, `pcnt0` | `list device`, plus per-device tests | §17a-§17j |
| ADC calibration | eFuse curve fitting + HW offset code; `c5adc` prints millivolts | §17u |
| TWAI (CAN) | loopback at 125k/250k/500k/1M, bit timing exact | §17f |
| `can0` (rt_can) | a frame through open/write/read comes back unchanged | §17r |
| `c5probe` | which pads respond on this package: all 29 do | §17t |
| `c5lpi2c` | **written, FSM never starts** - see the section for what is ruled out | §17t |
| RMT | TX waveform measured back by RX: 19 symbols, mean half 4761 ticks = request | §17o |
| MCPWM | frequency and duty measured at the pad, 50 Hz - 40 kHz | §17h |
| Temperature | two-band cross-check proves the range register took | §17e |
| USB Serial/JTAG | device side; FIFO coherent, no host | §17l |
| AHB GDMA | mem-to-mem byte-exact at 1/16/128/256 B, no overrun | §17n |
| PARLIO | GDMA feeds it, unit reads every bit, PCNT sees the clock | §17q |
| PSRAM | 1 MB byte-exact with a position-dependent pattern | §17g |
| `rt_malloc()` → PSRAM | **4 MB allocated, written and read back; small blocks stay in SRAM** | §17v |
| LP core | arithmetic exact at 100/2000/5000/20000 loops, restarts | §17k |

**Still absent, all reported as supported by `soc_caps.h`:**

| Peripheral | Note |
| --- | --- |
| SDIO slave | needs a host on the other end; cannot be self-tested |
| LCD / camera via PARLIO RX | PARLIO RX not written (TX is) |
| ADC accuracy | millivolts are computed (§17u); **accuracy against a reference is not measured** |

Each new peripheral needs its `RT_USING_*` macro in `port/esp32c5/rtconfig.h`,
its file in the C5 `target_srcs`, and - measured five times now - a `-u` link
flag, because an `INIT_*_EXPORT` data entry does not survive `--gc-sections`.

**Chip capabilities untouched**

1. **8 MB in-package PSRAM.** Enabled (§17g) and **now reachable through `rt_malloc()`**
   (§17v): a second memheap named `psram`, with auto-binding so small
   allocations stay in the 65 KB internal region and large ones fall through to
   PSRAM. Verified by pointer address and by content, not inferred from the
   configuration.
2. **The LP core runs** (§17k) and **deep sleep works** (§17s): a timer wake
   brings the board back with the RTC marker intact. **LP peripherals are not
   exercised** - LP I2C, LP UART and ETM all need their own port layer.
3. **Low-power modes.** Light sleep, deep sleep and automatic power management are
   all untested.

**Wireless features not exercised**

4. **802.15.4 / Thread / Zigbee.** `CONFIG_IEEE802154_ENABLED=y` is set, but the
   image contains only `ieee802154_txon_delay_set` and a modem-clock helper — no
   `esp_ieee802154_enable`, no `esp_ieee802154_transmit`, no `ot_radio_init`.
5. ~~**SoftAP.** Compiled in, never started.~~ **Verified in §17i** - beaconing and
   visible from another device; the AP/STA round trip works.
6. **ESP-NOW.** `esp_now_init` is not in the image.
7. **NAN**, WPA3/SAE, enterprise auth, promiscuous/sniffer: none tested.
8. **BLE beyond the peripheral minimum**: multiple simultaneous connections,
   pairing/bonding/encryption, Central role, BLE 5 extended advertising, 2M/Coded
   PHY, throughput characterisation.
9. **Wi-Fi power-save modes.** Only `WIFI_PS_NONE` measured; actual power
   consumption not measured at all.

**Platform features not implemented**

10. **OTA, secure boot, flash encryption.** Flash was tested with a single 16 KB
    write/read cycle.
11. **Long-run stability.** No soak test; the longest hold is 60 s.
12. **AP state after repeated runs** — `reason=2` (AUTH_EXPIRE) after six quick
    associations from one MAC.
13. ~~**ADC voltage conversion.**~~ **Done in §17u** - eFuse curve fitting, per-channel compensation, and the hardware offset code applied per conversion. `c5adc` prints millivolts and `rt_adc_voltage()` now returns a number.

**Numbers that are bounds, not characterisations**

14. **19 Mbit/s is not a Wi-Fi 6 result.** The ceiling is a single-threaded socket
    loop with a 32 KB window, not the radio.
15. **Range and roaming** untested; every measurement was taken with the AP in the
    same room.

## 22. Edits outside this port

Two changes were made to files that are not C5-specific, because someone else's
in-progress debugging broke the single-core build:

1. `rt-thread/src/thread.c` — a `[RESUME]` print in `rt_thread_resume()` reads
   `oncpu`/`bind_cpu`, which exist only under `RT_USING_SMP`. Guarded.
2. `esp-idf-rtt/components/rtthread/port/freertos/ft_task.c` — an `[FSP]` block
   references `cpus_lock_nest` (SMP only) and `rt_xt_unlock_n` (Xtensa only).
   Guarded.

Both blocks were kept, not deleted, so the SMP/Xtensa debugging they were added for
still works.
