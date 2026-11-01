# microSD/TF slot on the SDMMC controller (optional via menuconfig).
# Same dependency set modules/storage used to carry, moved here with the code.
include("${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/modesp_driver.cmake")
modesp_driver_component(
    SRCS "src/sdmmc_card_driver.cpp"
    PRIV_REQUIRES fatfs esp_driver_sdmmc sdmmc vfs
)
