idf_component_register(
    INCLUDE_DIRS "include"
    SRCS "src/sd_driver.c"
         "src/fat32fs.c"
         "src/sdcard_vfs.c"
    REQUIRES driver esp_timer nvs_flash
    PRIV_REQUIRES spi_flash
)
