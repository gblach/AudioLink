# NCS adds flash_sim_mcuboot.overlay to MCUboot after sysbuild/mcuboot.overlay,
# so an override there is lost. NCS skips overlays that are already listed, so
# listing it here first puts mcuboot_flash_sim.overlay after it.
set(mcuboot_EXTRA_DTC_OVERLAY_FILE
  "${ZEPHYR_NRF_MODULE_DIR}/modules/mcuboot/flash_sim_mcuboot.overlay"
  "${CMAKE_CURRENT_LIST_DIR}/sysbuild/mcuboot_flash_sim.overlay"
  CACHE STRING "Extra devicetree overlays for MCUboot" FORCE
)
