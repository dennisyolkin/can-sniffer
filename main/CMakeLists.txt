set(srcs
    "main.c"
    "status_led.c"
    "sd_logger.c"
    "live_data.c"
    "wifi_ap.c"
    "web_server.c"
    "trip_energy.c"
)
# Only the test build can transmit; the car image does not even contain the code.
if(CONFIG_CAN_TX_MODE)
    list(APPEND srcs "uds_client.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES
      esp_driver_twai
      esp_driver_gpio
      log
      esp_timer
      led_strip
      fatfs sdmmc
      spi_flash driver
      esp_wifi esp_netif esp_event nvs_flash
      esp_http_server esp_app_format
)

# The page is gzipped at build time and only the compressed copy goes into flash; web_server.c serves it as is.
idf_build_get_property(python PYTHON)
set(page_src "${COMPONENT_DIR}/web/index.html")
set(page_gz "${CMAKE_CURRENT_BINARY_DIR}/index.html.gz")
add_custom_command(OUTPUT "${page_gz}"
    COMMAND "${python}" "${PROJECT_DIR}/tools/gzip_asset.py" "${page_src}" "${page_gz}"
    DEPENDS "${page_src}" "${PROJECT_DIR}/tools/gzip_asset.py"
    VERBATIM)
add_custom_target(web_assets DEPENDS "${page_gz}")
target_add_binary_data(${COMPONENT_LIB} "${page_gz}" BINARY DEPENDS web_assets)
