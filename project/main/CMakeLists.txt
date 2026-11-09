idf_component_register(SRCS "main.c" "wifi.c" "ble.c" "ulp_selftest.c" "ap.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_hal_uart soc esp_system esp_wifi esp_event esp_netif nvs_flash bt lwip ulp
                       PRIV_REQUIRES bt
                       WHOLE_ARCHIVE)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-cast-function-type)

# The LP core program: a separate CMake project with its own toolchain (rv32imac)
# and its own linker script. ulp_add_project() builds it, embeds the .bin into
# this component, and generates the .h/.ld that expose its .shared_mem globals as
# pointers this component can read.
if(CONFIG_ULP_COPROC_ENABLED)
    ulp_add_project("ulp_c5_lp" "${CMAKE_CURRENT_LIST_DIR}/ulp/")
endif()



