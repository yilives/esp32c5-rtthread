# The C5 wiring in `components/rtthread/CMakeLists.txt`

Not a patch file: the five regions you have to reproduce, extracted from a tree
where the port builds and passes every test in the README. Line numbers refer to
that tree and will drift.


## 1. The port sources: the C5 `target_srcs` branch

Every `.c` in `port/esp32c5/`. The headers come in through the port include
directory, so only sources are listed.

```cmake
elseif(CONFIG_IDF_TARGET_ESP32C5)
# ESP32-C5 has exactly one device so far: uart0. The list below is therefore the
# serial v1 core plus what its TX path needs, and nothing else.
#
# The S31-shaped list below cannot be reused here, and the reason is not taste:
# it compiles dev_pin.c, dev_i2c_core.c, dev_spi_core.c, adc.c, rt_drv_pwm.c and
# pulse_encoder.c unconditionally, and every one of those files dereferences a
# struct that rtdevice.h only declares when its RT_USING_<class> macro is set.
# Measured with this port's rtconfig.h: pulse_encoder.c fails with
# "invalid use of undefined type 'struct rt_pulse_encoder_device'" and
# 'PULSE_ENCODER_CMD_ENABLE' undeclared - the class is off, the file is not.
#
# Enabling all six classes to make the long list compile would advertise buses
# this port does not drive, so the short list is the honest answer. Add a file
# back here (and its RT_USING_* to rtconfig.h) when its driver exists.
set(serial_srcs
    "${RTTHREAD_ROOT}/components/drivers/serial/dev_serial.c"
    # Pin device: rt_device_pin_register() and the rt_pin_* wrappers that
    # port/esp32c5/gpio.c plugs its rt_pin_ops into. It also carries the msh
    # `pin` command, so it cannot be dropped without the driver becoming
    # unreachable from the shell.
    "${RTTHREAD_ROOT}/components/drivers/pin/dev_pin.c"
    # PWM device class: rt_device_pwm_register()/rt_pwm_set()/rt_pwm_enable(),
    # which port/esp32c5/pwm.c plugs its rt_pwm_ops into. Only compiled now that
    # RT_USING_PWM is on in the C5 rtconfig.h - without that macro rtdevice.h
    # does not declare struct rt_device_pwm and this file does not compile.
    "${RTTHREAD_ROOT}/components/drivers/misc/rt_drv_pwm.c"
    # I2C device class: rt_i2c_bus_device_register() and rt_i2c_transfer(),
    # which port/esp32c5/i2c.c plugs its rt_i2c_bus_device_ops into. dev_i2c_dev.c
    # supplies rt_i2c_bus_device_device_init() that core calls - without it the
    # link fails with exactly that one undefined symbol (measured on S3/S31).
    "${RTTHREAD_ROOT}/components/drivers/i2c/dev_i2c_core.c"
    "${RTTHREAD_ROOT}/components/drivers/i2c/dev_i2c_dev.c"
    # ADC device class (v1 framework): rt_hw_adc_register(), rt_adc_enable(),
    # rt_adc_read(), which port/esp32c5/adc.c plugs its rt_adc_ops into. This
    # one is not optional - without it the link fails with undefined references
    # to exactly those three symbols (measured).
    "${RTTHREAD_ROOT}/components/drivers/adc/adc.c"
    # SPI bus class: rt_spi_bus_register() and rt_spi_transfer(), which
    # port/esp32c5/spi.c plugs its rt_spi_ops into. dev_spi.c supplies
    # rt_spidev_device_init() that the core calls.
    "${RTTHREAD_ROOT}/components/drivers/spi/dev_spi_core.c"
    "${RTTHREAD_ROOT}/components/drivers/spi/dev_spi.c"
    # Pulse encoder class: rt_device_pulse_encoder_register() and the
    # rt_device_read() path that returns the count, which port/esp32c5/pcnt.c
    # plugs its rt_pulse_encoder_ops into. Only compiles now that
    # RT_USING_PULSE_ENCODER is on - without that macro rtdevice.h does not
    # declare struct rt_pulse_encoder_device.
    "${RTTHREAD_ROOT}/components/drivers/misc/pulse_encoder.c"
    # CAN device class: rt_can_ops, the software RX FIFO, rt_hw_can_isr.
    "${RTTHREAD_ROOT}/components/drivers/can/dev_can.c"
    # dev_can.c's non-blocking TX path and its ISR use rt_ringbuffer_*, which
    # lives in ipc/ringbuffer.c. Nothing else in this port pulled that member in,
    # so the class compiled and the link failed on five ringbuffer symbols.
    "${RTTHREAD_ROOT}/components/drivers/ipc/ringbuffer.c"
    # completion_* moved to components/drivers/ipc/ in 5.x; dev_serial.c's TX path
    # waits on an rt_completion. Single core -> completion_up.c.
    "${RTTHREAD_ROOT}/components/drivers/ipc/completion_up.c"
    "${RTTHREAD_ROOT}/components/drivers/ipc/completion_comm.c")

else()
# Serial framework, v1. dev_serial.c is the only file needed: the tty half is
# behind RT_USING_SMART, and bypass/DMA are optional.
# completion_* moved to components/drivers/ipc/ in 5.x; dev_serial.c needs
# rt_completion_wait/init/done for its TX path. Single core -> completion_up.c.
# dev_pin.c provides rt_device_pin_register() and the rt_pin_* wrappers that
# port/esp32s31/gpio.c plugs its rt_pin_ops into.
set(serial_srcs
    "${RTTHREAD_ROOT}/components/drivers/serial/dev_serial.c"
    "${RTTHREAD_ROOT}/components/drivers/pin/dev_pin.c"
    "${RTTHREAD_ROOT}/components/drivers/misc/rt_drv_pwm.c"
    "${RTTHREAD_ROOT}/components/drivers/misc/pulse_encoder.c"
    # dev_i2c_bus.c is the device-model (RT_USING_DM) half: it needs
    # struct rt_i2c_driver / rt_i2c_client with a `parent` rt_device, which the
    # DM-aware rtdevice.h only defines with RT_USING_DM on. dev_i2c_core.c is
    # the bus registration and rt_i2c_transfer() implementation, and
    # dev_i2c_dev.c supplies rt_i2c_bus_device_device_init(), which core calls -
    # without it the link fails with exactly that one undefined symbol.
    "${RTTHREAD_ROOT}/components/drivers/i2c/dev_i2c_core.c"
    "${RTTHREAD_ROOT}/components/drivers/spi/dev_spi_core.c"
    "${RTTHREAD_ROOT}/components/drivers/spi/dev_spi.c"
    "${RTTHREAD_ROOT}/components/drivers/adc/adc.c"
    "${RTTHREAD_ROOT}/components/drivers/i2c/dev_i2c_dev.c"
    "${RTTHREAD_ROOT}/components/drivers/ipc/completion_up.c"
    "${RTTHREAD_ROOT}/components/drivers/ipc/completion_comm.c")
endif() # rtt_xtensa
```

## 2. Platform glue: the C5 branch that sets the port directory

Picks the port directory and `rtconfig.h`. The C5 is the C6's CLIC sibling -
one RV32IMAC core, CLIC with hardware nesting, no FPU - so it reuses the C6 CPU
port and interrupt bridge.

```cmake
elseif(CONFIG_IDF_TARGET_ESP32C5)
    # The C5 is the C6's CLIC sibling: one RV32IMAC core, a CLIC with hardware
    # nesting, no FPU. It reuses the C6 source set - the CPU port, the interrupt
    # bridge and the UART0 console driver - because port/esp32c6 and
    # port/esp32c5 differ only in the SoC name and in the two trap-path tables
    # cpuport.c has to define for a single-core build.
    #
    # gpio.c, pwm.c, i2c.c, spi.c and adc.c were absent in the first version,
    # because they were written against the S31's LL headers and pin maps. They
    # have since been brought up here, along with TWAI, RMT, PCNT, MCPWM, the
    # temperature sensor, PSRAM, USB Serial/JTAG, AHB GDMA and PARLIO; `list
    # device` now shows eight devices. The list below is the current one.
    set(target_srcs
        "port/esp32c5/cpuport.c"
        "port/esp32c5/interrupt.c"
        "port/esp32c5/serial.c"
        "port/esp32c5/gpio.c"
        # LEDC as an rt_device_pwm. Needs RT_USING_PWM and rt_drv_pwm.c, both of
        # which are already in this target's serial_srcs list above.
        "port/esp32c5/pwm.c"
        # I2C master on the I2C LL. Needs RT_USING_I2C and dev_i2c_core.c, which
        # are added to serial_srcs below.
        "port/esp32c5/i2c.c"
        # SAR ADC oneshot as an rt_adc_device. Needs RT_USING_ADC and the adc.c
        # framework file, which are added to serial_srcs below.
        "port/esp32c5/adc.c"
        # GPSPI master (SPI2) as an rt_spi_bus. Needs RT_USING_SPI and
        # dev_spi_core.c, which are added to serial_srcs below.
        "port/esp32c5/spi.c"
        # TWAI (CAN / CAN-FD) as a plain C API - the hardware path first, an
        # rt_can device class later. Needs no RT-Thread class macro.
        "port/esp32c5/twai.c"
        # RMT TX + RX as a plain C API. RT-Thread has no RMT class.
        "port/esp32c5/rmt.c"
        # PCNT as an rt_pulse_encoder device (`pcnt0`). Needs
        # RT_USING_PULSE_ENCODER and pulse_encoder.c.
        "port/esp32c5/pcnt.c"
        # MCPWM as a plain C API. RT-Thread has no motor-control class.
        "port/esp32c5/mcpwm.c"
        # Temperature sensor as a plain C API. It lives in the SAR ADC block.
        "port/esp32c5/tsens.c"
        # PSRAM: state reporting and a read/write check. Needs CONFIG_SPIRAM.
        # NOTE: the LP core (ULP RISC-V) driver is NOT here. An LP program is a
        # property of the project, not of the port: ulp_add_project() attaches the
        # generated shared-symbol linker script to the component that calls it, so
        # the shared variables are only visible from that component. Ours lives in
        # main/ulp_selftest.c for that reason.
        "port/esp32c5/psram.c"
        # Pin ownership table and the conflict report. Not a driver.
        "port/esp32c5/resources.c"
        # USB Serial/JTAG: the in-chip USB device controller. Pads IO13/IO14.
        "port/esp32c5/usj.c"
        # AHB GDMA: one TX channel, memory-to-memory. PARLIO needs it.
        "port/esp32c5/gdma.c"
        # PARLIO TX: parallel output fed by GDMA, verified against PCNT.
        "port/esp32c5/parlio.c"
        # TWAI as an rt_can device (`can0`).
        "port/esp32c5/can.c"
        # Deep sleep: what survives the HP core being powered down.
        "port/esp32c5/lp_sleep.c"
        # LP I2C: the low-power domain I2C controller as `lp_i2c0`.
        "port/esp32c5/lp_i2c.c")

    # The FreeRTOS-API compatibility layer, target independent - the same files
    # the S31 uses. Without this block a C5 build with
    # CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM=y does NOT link: measured, the first
    # attempt produced undefined references to xQueueGenericCreate,
    # xQueueSemaphoreTake, xTaskGetSchedulerState, vTaskSuspendAll, vTaskDelay and
    # more, from spi_flash, esp_libc/src/locks.c and freertos/portmacro.h. The
    # Kconfig switch turns the real FreeRTOS scheduler off; nothing turned the
    # shim on, because the shim was only listed for S3 and S31.
    #
    # ft_ipc/ft_qset/ft_event/ft_task are the machinery the whole shim is built
    # on and cost nothing when unused; the thin xQueueCreate -> ft_ipc_create
    # layer is conditional, because defining those symbols while libfreertos.a
    # still defines them makes the winner depend on archive order.
    list(APPEND target_srcs
        "port/freertos/ft_ipc.c"
        "port/freertos/ft_qset.c"
        "port/freertos/ft_event.c"
        "port/freertos/ft_task.c")

    if(CONFIG_IDF_RTOS_RTTHREAD_FREERTOS_SHIM)
        list(APPEND target_srcs
            "port/freertos/freertos_api.c"
            "port/freertos/ft_query.c"
            "port/freertos/ft_probe.c"
            "port/freertos/ft_notify.c")
    endif()

    set(rtt_config_dir "${CMAKE_CURRENT_LIST_DIR}/port/esp32c5")
endif()
```

## 3. Warning suppression for `dev_can.c`

Upstream `dev_can.c` assumes a build without `-Werror`: its
`/* FALLTHROUGH: ... */` is not a spelling GCC accepts at
`-Wimplicit-fallthrough=3`, and its msh commands hit `cast-function-type`. Both
suppressed **for that one file**, not globally.

```cmake

if(CONFIG_IDF_TARGET_ESP32C5)
    # dev_can.c relies on a `/* FALLTHROUGH: ... */` comment to mark a
    # deliberate case fall-through in rt_hw_can_isr. GCC only accepts the
    # bare `/* fall through */` spellings at -Wimplicit-fallthrough=3, which
    # this build enables, so upstream's marker is not recognised and the file
    # fails on a warning. Suppressing it for this one file is narrower than
    # changing the shared build flags.
    set_source_files_properties("${RTTHREAD_ROOT}/components/drivers/can/dev_can.c" PROPERTIES
        COMPILE_OPTIONS "-Wno-implicit-fallthrough;-Wno-cast-function-type")
    # i2c.c exports an `i2cscan` msh command through MSH_CMD_EXPORT_ALIAS, which
    # hits the same cast-function-type warning as finsh itself - the upstream
    # design, not a local bug. Same treatment as the S31 block below.
    set_source_files_properties("port/esp32c5/i2c.c" PROPERTIES
        COMPILE_DEFINITIONS "__RTTHREAD__;__RT_KERNEL_SOURCE__"
        COMPILE_OPTIONS "-ffreestanding;-fno-builtin;-Wno-cast-function-type")
endif()
```

## 4. The `-u` link anchors

**The part that is easy to omit and impossible to notice.** An
`INIT_BOARD_EXPORT()` stores a function pointer in a data section, and
`--gc-sections` then drops the archive member because nothing references it *by
symbol*. The object compiles, sits in the build tree, and is absent from the
image - no error, and `list device` simply shows fewer devices.

Measured eleven times in this port.

```cmake

if(CONFIG_IDF_TARGET_ESP32C5)
    # port/esp32c5/serial.c registers uart0 through INIT_BOARD_EXPORT and also
    # carries rt_hw_console_output(), which rt_kprintf resolves through the weak
    # stub in kservice.c. Both are data-section entries or weak, so neither
    # creates the reference that would pull the member out of librtthread.a -
    # --gc-sections drops it and the board then boots with no console at all.
    # Measured on S31 with the same arrangement.
    #
    # rt_hw_pin_init is the same story one file further on: measured here, the
    # first build with port/esp32c5/gpio.c compiled the object (it is present in
    # build/esp-idf/rtthread/CMakeFiles as gpio.c.obj) and the linked image
    # contained neither rt_hw_pin_init nor the `pin` command, so `list device`
    # showed uart0 only.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_uart0_init" "-u rt_hw_pin_init")
    # Same reason again for pwm.c: its rt_hw_pwm_init() is referenced only by an
    # INIT_BOARD_EXPORT data entry, so --gc-sections would drop the member and
    # `list device` would show no pwm0 with no error anywhere.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_pwm_init")
    # And a third time for i2c.c. This one anchor also brings in the `i2cscan`
    # command, because both live in the same archive member - `-u cmd_i2cscan`
    # would NOT work, since that handler is static and `-u` creates an external
    # reference a local symbol cannot satisfy (see the note in the xtensa block).
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_i2c_init")
    # adc.c and spi.c, same reason each time. Measured: without the spi one the
    # object compiled, spi.c.obj is in the build tree, and the linked image
    # contains no rt_hw_spi_init at all - `list device` just never shows spi0.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_adc_init")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_spi_init")
    # can.c registers can0 from INIT_DEVICE_EXPORT, a data-section entry, so
    # --gc-sections drops the member and `list device` shows no can0 - the
    # same shape as the seven above. This one anchor also brings in `c5can`,
    # which lives in the same archive member.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_can_c5_init")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u rt_hw_pin_init" "-u c5_probe_pads")
    # lp_sleep.c has no init function of its own - it is driven from the
    # shell and from the boot banner - so the anchor is its command.
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u c5_lp_sleep_selftest")
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u c5_lp_i2c_selftest")
endif()
```

## 5. The HAL components to link

Each `idf::esp_hal_*` is needed by exactly one driver; the report section for
that driver says which.

```cmake

if(CONFIG_IDF_TARGET_ESP32C5)
# Same mechanism, same measured reason: adding esp_hal_ledc to PRIV_REQUIRES did
# NOT put hal/ledc_ll.h on this component's compile line - the build still failed
# with "pwm.c: fatal error: hal/ledc_ll.h: No such file or directory" - while
# linking the component alias does. Only LEDC is needed so far; i2c/spi/adc are
# not driven on this target yet and adding their aliases would pull in hardware
# support code that nothing here uses.
target_link_libraries(${COMPONENT_LIB} PRIVATE idf::esp_hal_ledc idf::esp_hal_i2c idf::esp_hal_ana_conv idf::esp_hal_gpspi idf::esp_hal_twai idf::esp_hal_rmt idf::esp_hal_pcnt idf::esp_hal_mcpwm idf::esp_psram idf::esp_hal_usb idf::esp_hal_dma idf::esp_hal_parlio)
endif()
```
