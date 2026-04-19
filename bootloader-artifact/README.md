# Prebuilt bootloader + Apploader

`opendisplay-bootloader.s37` is the combined Gecko Bootloader, Bluetooth Apploader, and OTA stub built from the separate `opendisplay/bootloader` project (same tree as this repo’s historical layout).

To refresh after bootloader changes: build that project, then copy:

`bootloader/artifact/opendisplay-bootloader.s37` → `Firmware-silabs-bg22/bootloader-artifact/opendisplay-bootloader.s37`
