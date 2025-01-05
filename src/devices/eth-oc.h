/*
eth-oc.h - OpenCores Ethernet MAC controller
Copyright (C) 2021  cerg2010cerg2010 <github.com/cerg2010cerg2010>
                    LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef ETH_OC_H
#define ETH_OC_H

#include "rvvmlib.h"
#include "plic.h"
#include "tap_api.h"

#define ETHOC_DEFAULT_MMIO 0x21000000

PUBLIC rvvm_mmio_dev_t* ethoc_init(rvvm_machine_t* machine, tap_dev_t* tap,
                                   rvvm_addr_t base_addr, plic_ctx_t* plic, uint32_t irq);
PUBLIC rvvm_mmio_dev_t* ethoc_init_auto(rvvm_machine_t* machine);

#endif
