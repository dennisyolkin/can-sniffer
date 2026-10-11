idf_component_register(
    SRCS 
        "main.c"
        "status_led.c"
        "sd_logger.c"
    INCLUDE_DIRS "."
    REQUIRES
      esp_driver_twai
      esp_driver_gpio
      log
      esp_timer
      led_strip
      fatfs sdmmc
      spi_flash driver
)