# storage — microSD (docs/CONCEPT.md, "microSD"). The volume comes from bindings.json like every
# other driver-backed peripheral: drivers/sdmmc_card owns the controller, the power
# rail and the FAT mount, this module owns when it is mounted and what the panel is told about it.
# Only `vfs` is left here, for the opendir() that counts what is playable.
idf_component_register(
    SRCS "src/storage_module.cpp"
    INCLUDE_DIRS "include"
    REQUIRES modesp_core modesp_hal
    PRIV_REQUIRES vfs
)
