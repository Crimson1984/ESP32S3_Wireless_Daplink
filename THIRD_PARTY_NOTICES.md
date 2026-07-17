# Third-party notices

The MSPM0 SRAM Loader compiles the following Texas Instruments source from the
locally installed MSPM0 SDK 2.10.00.04:

```text
source/ti/driverlib/dl_flashctl.c
source/ti/driverlib/dl_flashctl.h
source/ti/devices/**
```

These files use the TI BSD-3-Clause-style license included in
`D:\ti\mspm0_sdk_2_10_00_04\license_mspm0_sdk_2_10_00_04.txt` and in the source
file headers. The loader build does not copy generated SysConfig files into this
repository. Preserve TI copyright, conditions and disclaimer when distributing
the generated loader or derivative source.

ESP-IDF and the managed `espressif/esp_tinyusb`/TinyUSB components retain their
respective upstream licenses.
