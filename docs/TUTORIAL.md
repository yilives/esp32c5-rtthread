# Tutorial: from a bare ESP32-C5 to a running RT-Thread shell

This walks the same path the port took, in the order that works. Every step has a
command whose output tells you whether it succeeded — a step that only says
"now configure X" is a step you cannot debug.

If you only want to build and run, the README's getting-started section is
shorter. This is for when something does not come up.

---

## 0. Know what you have before writing any code

```bash
esptool.py --chip esp32c5 flash-id
```

```
Chip type:          ESP32-C5HR8 (revision v1.2)
Crystal frequency:  48MHz
MAC:                AA:BB:CC:DD:EE:FF
Manufacturer: 68  Device: 4018
Detected flash size: 16MB
```

`Manufacturer 68 / Device 4018` is the **in-package** flash, not an external
chip. `HR8` means 8 MB of PSRAM is in the package too.

> Write this down. Every later "why is the heap only 65 KB" question starts here,
> and the answer is usually "the PSRAM exists but nothing is using it".

---

## 1. Get the kernel booting before touching a single peripheral

The failure mode to avoid is debugging Wi-Fi and the scheduler at the same time.

Minimum viable port, in this order:

1. **`cpuport.c`** — the context switch and the trap entry. Nothing runs without it.
2. **`interrupt.c`** — CLIC line management.
3. **`serial.c`** — `uart0`, so you have a console.
4. **`rtconfig.h`** — one core, 1 ms tick.

Then the one thing that catches everybody:

```cmake
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_uart0_init")
```

`INIT_BOARD_EXPORT()` stores a pointer in a data section. `--gc-sections` drops
the archive member because nothing references it **by symbol**. The object
compiles, `uart0.c.obj` sits in the build tree, and the linked image has no
console at all — with no error.

**Check:** `list device` shows `uart0`. If it shows an empty table, this is why.

---

## 2. Prove the pad layer before trusting any peripheral

Half the drivers in this port failed at first because of one function:

```c
gpio_ll_set_output_enable_ctrl(&GPIO, pin, ctrl_by_periph, oen_inv);
```

The second bool is **invert**, not enable. Passing `true` disables the pad
exactly when the peripheral drives it. Canonical:

```c
gpio_ll_set_output_enable_ctrl(&GPIO, pin, true, false);
```

Write a pad probe first, and use it to answer "does this pad exist on this
package" separately from "is my driver right":

```
msh > c5probe 23 28
c5probe : IO23 follows (1 -> 1, 0 -> 0)
...
c5probe : 6 of 6 pad(s) respond
```

A pad the package does not bring out never follows its own output.

---

## 3. One peripheral at a time, and make the test measure

The pattern that worked, eleven times, is:

1. prove the clock is on and the register block is reachable (read a `date` or
   `version` register — a non-zero constant proves the bus, not the driver),
2. prove the pad moves (drive it, read it back),
3. prove the data path (loopback, or an independent counter),
4. only then claim the peripheral works.

**Resist reporting success from a register that cannot report failure.** Three
concrete cases from this port:

| symptom | real cause |
|---|---|
| "PARLIO never reports EOF" for a whole round | `int_st` is `int_raw & int_ena`, and `int_ena` was never set — the condition could not become true |
| "RMT receives zero symbols" for three rounds | RX carrier demodulator resets to **on**; it modulated the input |
| "the pads are not released" (I2C) | two pull-up layers exist; only one was enabled |

Each of those looked like "the peripheral does not work" and was actually
"the instrument reads zero no matter what".

---

## 4. Bring up the radios last, and expect the heap to be the problem

Wi-Fi needs roughly 30 KB of IDF heap. If your RT-Thread port reserved the
internal heap generously, Wi-Fi transmits for a few kilobytes and then stalls
with `ESP_ERR_NO_MEM`, and lwIP latches `TF_NAGLEMEMERR` — so the *socket* appears
broken rather than the heap.

The measured fix in this port:

```
CONFIG_IDF_RTTHREAD_HEAP_SIZE          65536   (was 131072)
CONFIG_IDF_RTTHREAD_INTERRUPT_STACK_SIZE 16384  (was 24576)
```

52 → 2542 → 19213 kbit/s.

---

## 5. Where to look when a transfer never completes

An ordered checklist, because these look identical from the outside:

1. **Is the controller's own clock on, separate from the bus clock?**
   I2C needs `ctr.clk_en`; the LL's `i2c_ll_enable_controller_clock()` is a
   **no-op for the LP instance**.
2. **Is the clock source a PLL nobody enabled?** ADC, TWAI, RMT, LEDC and PARLIO
   all default to one. Point them at XTAL explicitly.
3. **Are you polling `int_st`?** It is masked. Use `int_raw`.
4. **Is the FIFO reset bit self-clearing?** In the LP domain it may not be.
5. **Does the peripheral need a command strobe** (`conf_upgate`, `tx_start` on a
   rising edge) rather than a level?

Then dump the registers once, with the same shape every time: control register,
raw interrupt status, and every command word with its `done` bit. That dump found
five separate bugs in this port and each time the answer was visible in it.

---

## 6. Debugging a core with no FPU and no cache

* **No FPU.** `float` works but is soft-float, and the LP core has none at all.
  Do not assume `ARCH_RISCV_FPU`.
* **No cache on the LP core.** Anything it runs must be linked into it.
* **IRAM.** An ISR that runs while the flash cache is off cannot live in
  `.flash.text`. `IRAM_ATTR` on handlers, and check that the dispatcher is not
  swapped to a flash path (`ESP_INTR_FLAG_IRAM`).

---

## 7. The two-core picture

The C5 has an **HP** core (RISC-V, 240 MHz, runs RT-Thread) and an **LP** core
(separate, bare metal, 4 KB RTC slow memory, its own toolchain).

What is proven here:

* the LP core runs a program, shares variables with the HP core, and its
  arithmetic checks out (`c5ulp`);
* deep sleep + timer wake works, but **powers down the PSRAM** — contents are
  garbage on wake (`c5sleep 3`);
* the LP **peripherals** are blocked: LP I2C is reachable with both clocks
  enabled and still never starts its FSM.

Do not plan on the LP core for anything yet without a scope.

---

## 8. Before you push anything

```bash
python tools/check_xtensa_guards.py
```

Nine times an Xtensa-only debugging probe in shared code broke the C5 build, at
link time, minutes in. The check takes a second. `build.ps1` runs it as a gate.

And two habits that cost real time here:

* **Never rewrite an important file with `open(..., "w")`.** Write a temp file and
  replace it. The porting report was truncated to zero bytes once that way.
* **When you fix a bug in one half of a peripheral, audit the other half in the
  same breath.** The RMT TX carrier modulator default was found in round 14; the
  RX one defaults identically, sat 30 lines away, and was not found until round
  23 — after three rounds of investigating the wrong thing.
