# qemu-ch32v307
A CH32V307 MCU port to QEMU.

# Usage

## Build from source
To use this, simply copy `ch32v307.c` to your QEMU source tree's `hw/riscv/` folder and add to `hw/riscv/Kconfig`:

```
config CH32V307
    bool
    default y
    depends on RISCV32
    select SIFIVE_PLIC
    select SIFIVE_UART
```

And finally, add to `hw/riscv/meson.build`:

```meson
riscv_ss.add(when: 'CONFIG_CH32V307', if_true: files('ch32v307.c'))
```

Simply run `ninja` in your desired build folder and use via `-M ch32v307` argument in qemu riscv32.

## Use pre-built release

If you do not want to build from source, simply head over to the releases tab of this repository and download the pre-built binary. Keep in mind it will not be the exact up-to-date binary and if you want the truly latest version, build from source.
