# Panel viewer wire protocol shared by ../../firmware-e1004 (E1004, UART) and
# ../../firmware-ee05 (EE05, USB-Serial-JTAG); both add this directory's
# parent to EXTRA_COMPONENT_DIRS.
idf_component_register(
    SRCS "protocol.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_uart esp_driver_usb_serial_jtag freertos idfxx_core
    PRIV_REQUIRES esp_rom
)
