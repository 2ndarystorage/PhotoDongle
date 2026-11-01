idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_spi esp_driver_gpio esp_driver_sdspi
                                esp_driver_ledc
                                sdmmc fatfs nvs_flash bt spi_flash)
