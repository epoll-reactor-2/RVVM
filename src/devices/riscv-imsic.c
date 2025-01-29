/*
riscv-imsic.c - RISC-V Incoming Message-Signaled Interrupt Controller
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "riscv-imsic.h"
#include "riscv_hart.h"
#include "mem_ops.h"
#include "bit_ops.h"

#define IMSIC_REG_SETEIPNUM_LE 0x00
#define IMSIC_REG_SETEIPNUM_BE 0x04

static bool riscv_imsic_write(rvvm_mmio_dev_t* dev, void* data, size_t offset, uint8_t size)
{
    size_t hartid = offset >> 12;
    UNUSED(size);

    if (hartid < vector_size(dev->machine->harts)) {
        rvvm_hart_t* hart = vector_at(dev->machine->harts, hartid);
        switch (offset & 0xFFC) {
            case IMSIC_REG_SETEIPNUM_LE: {
                uint32_t identity = read_uint32_le(data);
                // TODO: Actual MSI interrupt injection
                UNUSED(hart);
                UNUSED(identity);
                break;
            }
            case IMSIC_REG_SETEIPNUM_BE: {
                uint32_t identity = read_uint32_be_m(data);
                // TODO: Actual MSI interrupt injection
                UNUSED(hart);
                UNUSED(identity);
                break;
            }
        }
    }

    return true;
}

static rvvm_mmio_type_t riscv_imsic_dev_type = {
    .name = "riscv_imsic",
};

PUBLIC void riscv_imsic_init(rvvm_machine_t* machine, rvvm_addr_t addr, bool smode)
{
    rvvm_mmio_dev_t riscv_imsic = {
        .addr = addr,
        .size = vector_size(machine->harts) << 12,
        .min_op_size = 4,
        .max_op_size = 4,
        .read = rvvm_mmio_none,
        .write = riscv_imsic_write,
        .type = &riscv_imsic_dev_type,
    };

    if (!rvvm_attach_mmio(machine, &riscv_imsic)) {
        rvvm_error("Failed to attach IMSIC!");
        return;
    }

#ifdef USE_FDT
    struct fdt_node* imsic_fdt = fdt_node_create_reg(smode ? "imsics_s" : "imsics_m", addr);
    struct fdt_node* cpus = fdt_node_find(rvvm_get_fdt_root(machine), "cpus");
    size_t irq_ext_cells = vector_size(machine->harts) << 1;
    uint32_t* irq_ext = safe_new_arr(uint32_t, irq_ext_cells);

    fdt_node_add_prop_str(imsic_fdt, "compatible", "riscv,imsics");
    fdt_node_add_prop_reg(imsic_fdt, "reg", riscv_imsic.addr, riscv_imsic.size);
    fdt_node_add_prop(imsic_fdt, "msi-controller", NULL, 0);
    fdt_node_add_prop(imsic_fdt, "interrupt-controller", NULL, 0);
    fdt_node_add_prop_u32(imsic_fdt, "#interrupt-cells", 0);
    fdt_node_add_prop_u32(imsic_fdt, "riscv,num-ids", 64);

    vector_foreach(machine->harts, i) {
        struct fdt_node* cpu = fdt_node_find_reg(cpus, "cpu", i);
        struct fdt_node* cpu_irq = fdt_node_find(cpu, "interrupt-controller");

        if (cpu_irq) {
            uint32_t irq_phandle = fdt_node_get_phandle(cpu_irq);
            irq_ext[(i << 1)] = irq_phandle;
            irq_ext[(i << 1) + 1] = smode ? RISCV_INTERRUPT_SEXTERNAL : RISCV_INTERRUPT_MEXTERNAL;
        } else {
            rvvm_warn("Missing CPU IRQ nodes in FDT!");
        }
    }

    fdt_node_add_prop_cells(imsic_fdt, "interrupts-extended", irq_ext, irq_ext_cells);
    fdt_node_add_child(rvvm_get_fdt_soc(machine), imsic_fdt);
    free(irq_ext);
#endif
}

PUBLIC void riscv_imsic_init_auto(rvvm_machine_t* machine)
{
    size_t imsic_size = vector_size(machine->harts) << 12;
    rvvm_addr_t m_addr = rvvm_mmio_zone_auto(machine, IMSIC_M_ADDR_DEFAULT, imsic_size);
    rvvm_addr_t s_addr = rvvm_mmio_zone_auto(machine, IMSIC_S_ADDR_DEFAULT, imsic_size);
    riscv_imsic_init(machine, m_addr, false);
    riscv_imsic_init(machine, s_addr, true);
}
