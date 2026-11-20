idf_component_register(SRCS "main.c" "esp_hid_gap.c" "keymap.c" "hid_parse.c" "keymap_nvs.c" "lcd.c" "led.c" "usb.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_hid
                       PRIV_REQUIRES nvs_flash fatfs wear_levelling esp_partition esp_driver_gpio esp_driver_spi esp_lcd espcoredump)

# Packs fatfs_image/ (DongleConfig.exe, built by tool/build.ps1) into the "storage" partition; written by `idf.py flash`.
set(FATFS_IMAGE_DIR ${CMAKE_CURRENT_LIST_DIR}/../fatfs_image)
file(MAKE_DIRECTORY ${FATFS_IMAGE_DIR})  # exe is gitignored, so a fresh clone has no folder
if(NOT EXISTS ${FATFS_IMAGE_DIR}/DongleConfig.exe)
    message(WARNING "fatfs_image/DongleConfig.exe missing: the USB drive will be empty. Build with dev.ps1 build (or tool/build.ps1 first).")
endif()
fatfs_create_spiflash_image(storage ${FATFS_IMAGE_DIR} FLASH_IN_PROJECT)
