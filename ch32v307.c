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

#define TYPE_CH32V_RCC "ch32v-rcc"
OBJECT_DECLARE_SIMPLE_TYPE(CH32VRCCState, CH32V_RCC)

#define RCC_CTLR      0x00
#define RCC_CFGR0     0x04
#define RCC_INTR      0x08
#define RCC_APB2PRSTR 0x0C
#define RCC_APB1PRSTR 0x10
#define RCC_AHBPCENR  0x14
#define RCC_APB2PCENR 0x18
#define RCC_APB1PCENR 0x1C
#define RCC_RSTSCKR   0x24
#define RCC_MMIO_SIZE 0x400

#define RCC_CTLR_HSIRDY (1u << 1)
#define RCC_CTLR_HSERDY (1u << 17)
#define RCC_CTLR_PLLRDY (1u << 25)

#define RCC_APB2PCENR_USART1EN (1u << 14)

struct CH32VRCCState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    uint32_t ctlr;
    uint32_t cfgr0;
    uint32_t apb2pcenr;
    uint32_t apb1pcenr;
    uint32_t ahbpcenr;
};

DeviceState *ch32v_rcc_create(hwaddr addr);

static uint64_t ch32v_rcc_read(void *opaque, hwaddr addr, unsigned size)
{
    CH32VRCCState *s = CH32V_RCC(opaque);

    switch (addr) {
    case RCC_CTLR:

         //
         // NOT modelling a real oscillator.. Firmware clock wait
         // should not spin forever
         //
         
        return s->ctlr | RCC_CTLR_HSIRDY | RCC_CTLR_HSERDY | RCC_CTLR_PLLRDY;
    case RCC_CFGR0:
        return s->cfgr0;
    case RCC_APB2PCENR:
        return s->apb2pcenr;
    case RCC_APB1PCENR:
        return s->apb1pcenr;
    case RCC_AHBPCENR:
        return s->ahbpcenr;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }
}

static void ch32v_rcc_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    CH32VRCCState *s = CH32V_RCC(opaque);

    switch (addr) {
    case RCC_CTLR:
        s->ctlr = val;
        return;
    case RCC_CFGR0:
        s->cfgr0 = val;
        return;
    case RCC_APB2PCENR:
        s->apb2pcenr = val;
        return;
    case RCC_APB1PCENR:
        s->apb1pcenr = val;
        return;
    case RCC_AHBPCENR:
        s->ahbpcenr = val;
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n",__func__, addr, val);
    }
}

static const MemoryRegionOps ch32v_rcc_ops = {
    .read = ch32v_rcc_read,
    .write = ch32v_rcc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void ch32v_rcc_init(Object *obj)
{
    CH32VRCCState *s = CH32V_RCC(obj);

    memory_region_init_io(&s->mmio, obj, &ch32v_rcc_ops, s, TYPE_CH32V_RCC, RCC_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const TypeInfo ch32v_rcc_info = {
    .name          = TYPE_CH32V_RCC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CH32VRCCState),
    .instance_init = ch32v_rcc_init,
};

static void ch32v_rcc_register_types(void)
{
    type_register_static(&ch32v_rcc_info);
}

type_init(ch32v_rcc_register_types)

DeviceState *ch32v_rcc_create(hwaddr addr)
{
    DeviceState *dev = qdev_new(TYPE_CH32V_RCC);

    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);

    return dev;
}

#define TYPE_CH32V_GPIO "ch32v-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(CH32VGPIOState, CH32V_GPIO)

#define GPIO_CFGLR 0x00
#define GPIO_CFGHR 0x04
#define GPIO_INDR  0x08
#define GPIO_OUTDR 0x0C
#define GPIO_BSHR  0x10
#define GPIO_BCR   0x14
#define GPIO_LCKR  0x18
#define GPIO_MMIO_SIZE 0x400

struct CH32VGPIOState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;

    uint32_t cfglr;
    uint32_t cfghr;
    uint32_t outdr;
    uint32_t lckr;
};

DeviceState *ch32v_gpio_create(hwaddr addr);

static uint64_t ch32v_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    CH32VGPIOState *s = CH32V_GPIO(opaque);

    switch (addr) {
    case GPIO_CFGLR:
        return s->cfglr;
    case GPIO_CFGHR:
        return s->cfghr;
    case GPIO_INDR:
        return s->outdr;
    case GPIO_OUTDR:
        return s->outdr;
    case GPIO_LCKR:
        return s->lckr;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read at 0x%" HWADDR_PRIx "\n", __func__, addr);
        return 0;
    }
}

static void ch32v_gpio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    CH32VGPIOState *s = CH32V_GPIO(opaque);

    switch (addr) {
    case GPIO_CFGLR:
        s->cfglr = val;
        return;
    case GPIO_CFGHR:
        s->cfghr = val;
        return;
    case GPIO_OUTDR:
        s->outdr = val;
        return;
    case GPIO_BSHR:
        s->outdr |= (uint32_t)(val & 0xFFFF);
        s->outdr &= ~(uint32_t)((val >> 16) & 0xFFFF);
        return;
    case GPIO_BCR:
        s->outdr &= ~(uint32_t)(val & 0xFFFF);
        return;
    case GPIO_LCKR:
        s->lckr = val;
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write at 0x%" HWADDR_PRIx " = 0x%" PRIx64 "\n", __func__, addr, val);
    }
}

static bool ch32v_gpio_pin_is_af_pp_output(CH32VGPIOState *s, int pin)
{
    uint32_t reg = (pin < 8) ? s->cfglr : s->cfghr;
    int local = pin % 8;
    uint32_t nibble = (reg >> (local * 4)) & 0xF;
    uint32_t cnf = (nibble >> 2) & 0x3;
    uint32_t mode = nibble & 0x3;

    return (cnf == 0x2) && (mode != 0x0);
}

static const MemoryRegionOps ch32v_gpio_ops = {
    .read = ch32v_gpio_read,
    .write = ch32v_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void ch32v_gpio_init(Object *obj)
{
    CH32VGPIOState *s = CH32V_GPIO(obj);

    memory_region_init_io(&s->mmio, obj, &ch32v_gpio_ops, s, TYPE_CH32V_GPIO, GPIO_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const TypeInfo ch32v_gpio_info = {
    .name          = TYPE_CH32V_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CH32VGPIOState),
    .instance_init = ch32v_gpio_init,
};

static void ch32v_gpio_register_types(void)
{
    type_register_static(&ch32v_gpio_info);
}

type_init(ch32v_gpio_register_types)

DeviceState *ch32v_gpio_create(hwaddr addr)
{
    DeviceState *dev = qdev_new(TYPE_CH32V_GPIO);

    sysbus_realize(SYS_BUS_DEVICE(dev), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, addr);

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
#define USART_STATR_RXNE (1u << 5)

#define CH32V307_USART1_TX_PIN 9

struct CH32VUsartState {
    SysBusDevice parent_obj;

    MemoryRegion mmio;
    CharFrontend chr;
    CH32VRCCState *rcc;
    CH32VGPIOState *gpioa;

    uint32_t ctlr1, ctlr2, ctlr3, brr, gpr;
    uint8_t rx_data;
    bool rx_pending;
};

DeviceState *ch32v_usart_create(hwaddr addr, Chardev *chr, CH32VRCCState *rcc, CH32VGPIOState *gpioa);

static uint64_t ch32v_usart_read(void *opaque, hwaddr addr, unsigned size)
{
    CH32VUsartState *s = CH32V_USART(opaque);

    switch (addr) {
        case USART_STATR: {
            uint32_t v = USART_STATR_TXE | USART_STATR_TC;
            if (s->rx_pending) {
                v |= USART_STATR_RXNE;
            }
            return v;
        }
        case USART_DATAR:
            if (s->rx_pending) {
                s->rx_pending = false;
                return s->rx_data;
            }
            return 0;
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
            if (s->rcc && !(s->rcc->apb2pcenr & RCC_APB2PCENR_USART1EN)) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: DATAR write while USART1 clock disabled (RCC_APB2PCENR.USART1EN=0)\n", __func__);
                return;
            }
            if (!(s->ctlr1 & (1u << 3))) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: DATAR write while transmitter disabled (CTLR1.TE=0)\n", __func__);
                return;
            }
            if (s->gpioa && !ch32v_gpio_pin_is_af_pp_output(s->gpioa, CH32V307_USART1_TX_PIN)) {
                qemu_log_mask(LOG_GUEST_ERROR, "%s: DATAR write while PA9 not configured as AF push-pull output (GPIOA_CFGHR)\n", __func__);
                return;
            }
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

static int ch32v_usart_can_receive(void *opaque)
{
    CH32VUsartState *s = CH32V_USART(opaque);
    return s->rx_pending ? 0 : 1; // refuse until read
}

static void ch32v_usart_receive(void *opaque, const uint8_t *buf, int size)
{
    CH32VUsartState *s = CH32V_USART(opaque);

    s->rx_data = buf[0];
    s->rx_pending = true;
}

type_init(ch32v_usart_register_types)

DeviceState *ch32v_usart_create(hwaddr addr, Chardev *chr, CH32VRCCState *rcc, CH32VGPIOState *gpioa)
{
    DeviceState *dev = qdev_new(TYPE_CH32V_USART);
    CH32VUsartState *s = CH32V_USART(dev);

    s->rcc = rcc;
    s->gpioa = gpioa;
    qemu_chr_fe_init(&s->chr, chr, &error_abort);
    qemu_chr_fe_set_handlers(&s->chr, ch32v_usart_can_receive, ch32v_usart_receive, NULL, NULL, s, NULL, true);
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
	DeviceState *rcc;
    DeviceState *gpioa;

    RISCVHartArrayState cpus;
    MemoryRegion flash;
    MemoryRegion flash_alias;
    MemoryRegion sram;
};

struct CH32V307State {
    MachineState parent_obj;

    CH32V307SoCState soc;
};

enum {
    CH32V307_DEV_FLASH,
    CH32V307_DEV_FLASH_ALIAS,
    CH32V307_DEV_SRAM,
    CH32V307_DEV_PFIC,
    CH32V307_DEV_RCC,
    CH32V307_DEV_GPIOA,
    CH32V307_DEV_USART1
};

#define FLASH_SIZE_KB 256
#define SRAM_SIZE_KB 64

//
// Memory map of the chip..
// here are the sources used
// https://github.com/openwch/ch32v307
// https://www.wch-ic.com/downloads/CH32V307DS0_PDF.html
//
static const MemMapEntry ch_memmap[] = {
    [CH32V307_DEV_FLASH] = {0x08000000, FLASH_SIZE_KB * 1024},       // apparently the actual silicon resides at 0x08000000
	[CH32V307_DEV_FLASH_ALIAS] = {0x00000000, FLASH_SIZE_KB * 1024}, // but 0x0 is also a hardware alias for ARM compat
    [CH32V307_DEV_SRAM]  = {0x20000000,  SRAM_SIZE_KB * 1024},
    [CH32V307_DEV_PFIC] = {0xE000E000, 0x1100},
    [CH32V307_DEV_RCC] = {0x40021000, 0x400},
    [CH32V307_DEV_GPIOA] = {0x40010800, 0x400},
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
    // currently.. USART1, RCC and PFIC, GPIO
    //

    s->pfic = ch32v_pfic_create(memmap[CH32V307_DEV_PFIC].base, qdev_get_gpio_in(DEVICE(&s->cpus.harts[0]), IRQ_M_EXT));
    s->rcc = ch32v_rcc_create(memmap[CH32V307_DEV_RCC].base);
    s->gpioa = ch32v_gpio_create(memmap[CH32V307_DEV_GPIOA].base);
    s->usart1 = ch32v_usart_create(memmap[CH32V307_DEV_USART1].base, serial_hd(0), CH32V_RCC(s->rcc), CH32V_GPIO(s->gpioa));

    //
    // Initialize PHYSICAL flash, rom like for now
    // no self programming (yet)
    //
    
    memory_region_init_rom(&s->flash, OBJECT(dev), "ch32v307.flash", memmap[CH32V307_DEV_FLASH].size, &error_fatal);
    memory_region_add_subregion(sys_mem, memmap[CH32V307_DEV_FLASH].base, &s->flash);

    //
    // Initialize ALIAS flash address
    //

    memory_region_init_alias(&s->flash_alias, OBJECT(dev), "ch32v307.flash_alias", &s->flash, 0, memmap[CH32V307_DEV_FLASH_ALIAS].size);
    memory_region_add_subregion(sys_mem, memmap[CH32V307_DEV_FLASH_ALIAS].base, &s->flash_alias);

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
