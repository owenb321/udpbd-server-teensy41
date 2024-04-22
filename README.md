# updbd-server-teensy41
Port of the PlayStation 2 [updbd-server](https://gitlab.com/ps2max/udpbd-server) to the Teensy 4.1 microcontroller that supports reading and writing from a FAT32/exFAT SD card or USB drive.

## Hardware

The Teensy 4.1 will need the [ethernet kit](https://www.pjrc.com/store/ethernet_kit.html) installed as well as the [USB host](https://www.pjrc.com/store/cable_usb_host_t36.html) headers (if a USB drive will be used).

## PS2 ISOs

The [OPL folder structure](https://www.ps2-home.com/forum/app.php/page/opl_folder_structure) should be placed on the root of the SD card or USB drive and PS2 ISOs will go in the `/DVD` directory. Loading ISOs in OPL will probably require `Mode 1` for compatibility.

Files will be mounted from SD first and USB if no SD card is detected.

## Notes

- SD cards will need to support the SDIO interface.
- Storage is limited to 2TB due to library support.
- Reads and writes will fail if the storage device is too slow.

## Build Prerequisites
- [Teensyduino](https://www.pjrc.com/teensy/teensyduino.html)
- [QNEthernet](https://github.com/ssilverman/QNEthernet) library