/*
 * QEMU WCH CH32V307 RISC-V MCU
 *
 * Copyright (c) 2026 Driftless Software, Pvt. Ltd.
 *
 * Port of the WCH CH32V307 microcontroller in QEMU.
 *
 */

#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/misc/unimp.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/boot.h"
#include "hw/intc/riscv_aclint.h"
#include "chardev/char.h"
#include "system/system.h"
#include "qom/object.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "system/system.h"
#include "chardev/char-fe.h"

#define TYPE_CH32V_PFIC "ch32v-pfic"
OBJECT_DECLARE_SIMPLE_TYPE(CH32VPFICState, CH32V_PFIC)

#define CH32V_PFIC_NUM_WORDS 8 // 256 lines
#define CH32V_PFIC_NUM_IRQ (CH32V_PFIC_NUM_WORDS*32)
#define IRQ_M_EXT 11

#define R_ISR    0x000
#define R_IPR    0x020
#define R_GISR   0x04C
#define R_IENR   0x100
#define R_IRER   0x180
#define R_IPSR   0x200
#define R_IPRR   0x280
#define R_IACTR  0x300
#define R_IPRIOR 0x400
#define R_SCTLR  0xD10
#define PFIC_MMIO_SIZE 0x1100

struct CH32VPFICState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    qemu_irq output;

    uint32_t ienr[CH32V_PFIC_NUM_WORDS];
    uint32_t ipr[CH32V_PFIC_NUM_WORDS];
};

DeviceState *ch32v_pfic_create(hwaddr addr, qemu_irq cpu_irq);

static void ch32v_pfic_update(CH32VPFICState *s)
{
    bool level = false;
    int i;

    for (i = 0; i < CH32V_PFIC_NUM_WORDS; i++) {
        if (s->ienr[i] & s->ipr[i]) {
            level = true;
            break;
        }
    }
    qemu_set_irq(s->output, level);
}

//
// This function is called when a peripheral own qemu_irq
// changes level..
//
 
static void ch32v_pfic_set_irq(void *opaque, int n, int level)
{
    CH32VPFICState *s = CH32V_PFIC(opaque);
    int word = n / 32;
    int bit  = n % 32;

    if (level) {
        s->ipr[word] |= (1u << bit);
    } else {
        s->ipr[word] &= ~(1u << bit);
    }
    ch32v_pfic_update(s);
}

static uint64_t ch32v_pfic_read(void *opaque, hwaddr addr, unsigned size)
{
    CH32VPFICState *s = CH32V_PFIC(opaque);

    if (addr < R_ISR + CH32V_PFIC_NUM_WORDS * 4) {
        return s->ipr[(addr - R_ISR) / 4]; //ISR mirrors IPR
    }
    if (addr >= R_IPR && addr < R_IPR + CH32V_PFIC_NUM_WORDS * 4) {
        return s->ipr[(addr - R_IPR) / 4];
    }
    if (addr == R_GISR) {
        return 0;
    }
    if (addr >= R_IACTR && addr < R_IACTR + CH32V_PFIC_NUM_WORDS * 4) {
        return 0;
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%" HWADDR_PRIx "\n", __func__, addr);
    return 0;
}

static void ch32v_pfic_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    CH32VPFICState *s = CH32V_PFIC(opaque);

    if (addr >= R_IENR && addr < R_IENR + CH32V_PFIC_NUM_WORDS * 4) {
        s->ienr[(addr - R_IENR) / 4] |= (uint32_t)val;
        ch32v_pfic_update(s);
        return;
    }
    if (addr >= R_IRER && addr < R_IRER + CH32V_PFIC_NUM_WORDS * 4) {
        s->ienr[(addr - R_IRER) / 4] &= ~(uint32_t)val;
        ch32v_pfic_update(s);
        return;
    }
    if (addr >= R_IPSR && addr < R_IPSR + CH32V_PFIC_NUM_WORDS * 4) {
        s->ipr[(addr - R_IPSR) / 4] |= (uint32_t)val;
        ch32v_pfic_update(s);
        return;
    }
    if (addr >= R_IPRR && addr < R_IPRR + CH32V_PFIC_NUM_WORDS * 4) {
        s->ipr[(addr - R_IPRR) / 4] &= ~(uint32_t)val;
        ch32v_pfic_update(s);
        return;
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n", __func__, addr, val);
}

static const MemoryRegionOps ch32v_pfic_ops = {
    .read = ch32v_pfic_read,
    .write = ch32v_pfic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void ch32v_pfic_init(Object *obj)
{
    CH32VPFICState *s = CH32V_PFIC(obj);

    memory_region_init_io(&s->mmio, obj, &ch32v_pfic_ops, s, TYPE_CH32V_PFIC, PFIC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->output);

    qdev_init_gpio_in(DEVICE(obj), ch32v_pfic_set_irq, CH32V_PFIC_NUM_IRQ);
}

static const TypeInfo ch32v_pfic_info = {
    .name          = TYPE_CH32V_PFIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CH32VPFICState),
    .instance_init = ch32v_pfic_init,
};

static void ch32v_pfic_register_types(void)
{
    type_register_static(&ch32v_pfic_info);
}

type_init(ch32v_pfic_register_types)

DeviceState *ch32v_pfic_create(hwaddr addr, qemu_irq cpu_irq)
{
    DeviceState *dev = qdev_new(TYPE_CH32V_PFIC);

    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, cpu_irq);

    return dev;
}

#define TYPE_CH32V_USART "ch32v-usart"
OBJECT_DECLARE_SIMPLE_TYPE(CH32VUsartState, CH32V_USART)

#define USART_STATR 0x00
#define USART_DATAR 0x04
#define USART_BRR   0x08
#define USART_CTLR1 0x0C
#define USART_CTLR2 0x10
#define USART_CTLR3 0x14
#define USART_GPR   0x18

#define USART_STATR_TXE (1u << 7)
#define USART_STATR_TC  (1u << 6)

struct CH32VUsartState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    CharFrontend chr;

    uint32_t ctlr1, ctlr2, ctlr3, brr, gpr;
};

DeviceState *ch32v_usart_create(hwaddr addr, Chardev *chr);

static uint64_t ch32v_usart_read(void *opaque, hwaddr addr, unsigned size)
{
    CH32VUsartState *s = CH32V_USART(opaque);

    switch (addr) {
    case USART_STATR:
        return USART_STATR_TXE | USART_STATR_TC; // no real timing :(
    case USART_CTLR1:
        return s->ctlr1;
    case USART_CTLR2:
        return s->ctlr2;
    case USART_CTLR3:
        return s->ctlr3;
    case USART_BRR:
        return s->brr;
    case USART_GPR:
        return s->gpr;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }
}

static void ch32v_usart_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    CH32VUsartState *s = CH32V_USART(opaque);
    uint8_t ch;

    switch (addr) {
    case USART_DATAR:
        ch = (uint8_t)val;
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        return;
    case USART_CTLR1:
        s->ctlr1 = val;
        return;
    case USART_CTLR2:
        s->ctlr2 = val;
        return;
    case USART_CTLR3:
        s->ctlr3 = val;
        return;
    case USART_BRR:
        s->brr = val;
        return;
    case USART_GPR:
        s->gpr = val;
        return;
    default:
        qemu_log_mask(LOG_UNIMP,"%s: unimplemented write at 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n",__func__, addr, val);
    }
}

static const MemoryRegionOps ch32v_usart_ops = {
    .read = ch32v_usart_read,
    .write = ch32v_usart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void ch32v_usart_init(Object *obj)
{
    CH32VUsartState *s = CH32V_USART(obj);

    memory_region_init_io(&s->mmio, obj, &ch32v_usart_ops, s, TYPE_CH32V_USART, 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const TypeInfo ch32v_usart_info = {
    .name          = TYPE_CH32V_USART,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CH32VUsartState),
    .instance_init = ch32v_usart_init,
};

static void ch32v_usart_register_types(void)
{
    type_register_static(&ch32v_usart_info);
}

type_init(ch32v_usart_register_types)

DeviceState *ch32v_usart_create(hwaddr addr, Chardev *chr)
{
    DeviceState *dev = qdev_new(TYPE_CH32V_USART);
    CH32VUsartState *s = CH32V_USART(dev);

    qemu_chr_fe_init(&s->chr, chr, &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);

    return dev;
}

#define TYPE_CH32V307_MACHINE MACHINE_TYPE_NAME("ch32v307")
#define TYPE_CH32V307_SOC "ch32v307-soc"

OBJECT_DECLARE_SIMPLE_TYPE(CH32V307State, CH32V307_MACHINE)
OBJECT_DECLARE_SIMPLE_TYPE(CH32V307SoCState, CH32V307_SOC)

struct CH32V307SoCState {
    DeviceState parent_obj;
    DeviceState *pfic;
    DeviceState *usart1;

    RISCVHartArrayState cpus;
    MemoryRegion flash;
    MemoryRegion sram;
};

struct CH32V307State {
    MachineState parent_obj;

    CH32V307SoCState soc;
};

enum {
    CH32V307_DEV_FLASH,
    CH32V307_DEV_SRAM,
    CH32V307_DEV_PFIC,
    CH32V307_DEV_USART1
};

//
// Memory map of the chip..
// here are the sources used
// https://github.com/openwch/ch32v307
// https://www.wch-ic.com/downloads/CH32V307DS0_PDF.html
//
static const MemMapEntry ch_memmap[] = {
    [CH32V307_DEV_FLASH] = {0x00000000, 480 * 1024}, // apparently the actual silicon resides at 0x08000000
                                                     // but 0x0 is also a hardware alias for ARM compat
    [CH32V307_DEV_SRAM]  = {0x20000000,  64 * 1024},
    [CH32V307_DEV_PFIC] = {0xE000E000, 0x1100},
    [CH32V307_DEV_USART1] = {0x40013800, 0x400},
};

static void ch32v307_soc_init(Object *obj)
{
    CH32V307SoCState *s = CH32V307_SOC(obj);

    object_initialize_child(obj, "cpus", &s->cpus, TYPE_RISCV_HART_ARRAY);
}

static void ch32v307_soc_realize(DeviceState *dev, Error **errp)
{
    CH32V307SoCState *s = CH32V307_SOC(dev);
    MachineState *ms = MACHINE(qdev_get_machine());
    const MemMapEntry *memmap = ch_memmap;
    MemoryRegion *sys_mem = get_system_memory();

    //
    // Initialize CPU Array
    //
    
    object_property_set_str(OBJECT(&s->cpus), "cpu-type", ms->cpu_type, &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "num-harts", 1, &error_abort);
    object_property_set_int(OBJECT(&s->cpus), "resetvec", memmap[CH32V307_DEV_FLASH].base, &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->cpus), errp)) {
        return;
    }

    //
    // Initialize other peripherals
    // currently.. USART1 and PFIC
    //

    s->pfic = ch32v_pfic_create(memmap[CH32V307_DEV_PFIC].base, qdev_get_gpio_in(DEVICE(&s->cpus.harts[0]), IRQ_M_EXT));
    s->usart1 = ch32v_usart_create(memmap[CH32V307_DEV_USART1].base, serial_hd(0));

    //
    // Initialize flash, rom like for now
    // no self programming (yet)
    //
    
    memory_region_init_rom(&s->flash, OBJECT(dev), "ch32v307.flash", memmap[CH32V307_DEV_FLASH].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[CH32V307_DEV_FLASH].base, &s->flash);

    //
    // Init SRAM
    //
    
    memory_region_init_ram(&s->sram, OBJECT(dev), "ch32v307.sram", memmap[CH32V307_DEV_SRAM].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[CH32V307_DEV_SRAM].base, &s->sram);
}

static void ch32v307_soc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ch32v307_soc_realize;
    dc->user_creatable = false;
}

static const TypeInfo ch32v307_soc_type_info = {
    .name          = TYPE_CH32V307_SOC,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(CH32V307SoCState),
    .instance_init = ch32v307_soc_init,
    .class_init    = ch32v307_soc_class_init,
};

static void ch32v307_machine_instance_init(Object *obj)
{
    CH32V307State *s = CH32V307_MACHINE(obj);

    object_initialize_child(obj, "soc", &s->soc, TYPE_CH32V307_SOC);
}

static void ch32v307_machine_init(MachineState *machine)
{
    CH32V307State *s = CH32V307_MACHINE(machine);
    const MemMapEntry *memmap = ch_memmap;
    RISCVBootInfo boot_info;

    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    //
    // Straight into flash
    //
    
    riscv_boot_info_init(&boot_info, &s->soc.cpus);
    if (machine->kernel_filename) {
        riscv_load_kernel(machine, &boot_info, memmap[CH32V307_DEV_FLASH].base, false, NULL);
    }
}

static void ch32v307_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "WCH CH32V307 RISC-V Microcontroller";
    mc->init = ch32v307_machine_init;
    mc->default_cpu_type = RISCV_CPU_TYPE_NAME("rv32");
}

static const TypeInfo ch32v307_machine_type = {
    .name          = TYPE_CH32V307_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(CH32V307State),
    .class_init    = ch32v307_machine_class_init,
    .instance_init = ch32v307_machine_instance_init,
};

static void ch32v307_register_types(void)
{
    type_register_static(&ch32v307_soc_type_info);
    type_register_static(&ch32v307_machine_type);
}

type_init(ch32v307_register_types)
