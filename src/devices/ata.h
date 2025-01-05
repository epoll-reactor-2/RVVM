/*
ata.h - IDE/ATA disk controller
Copyright (C) 2021  cerg2010cerg2010 <github.com/cerg2010cerg2010>
                    LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_ATA_H
#define RVVM_ATA_H

#include "rvvmlib.h"
#include "pci-bus.h"

#define ATA_DATA_DEFAULT_MMIO 0x40000000
#define ATA_CTL_DEFAULT_MMIO  0x40001000

PUBLIC bool ata_init_pio(rvvm_machine_t* machine, rvvm_addr_t data_base_addr, rvvm_addr_t ctl_base_addr, const char* image_path, bool rw);
PUBLIC pci_dev_t* ata_init_pci(pci_bus_t* pci_bus, const char* image_path, bool rw);

PUBLIC bool ata_init_auto(rvvm_machine_t* machine, const char* image_path, bool rw);

#endif
