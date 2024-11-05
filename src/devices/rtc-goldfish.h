/*
rtc-goldfish.h - Goldfish Real-time Clock
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RTC_GOLDFISH_H
#define RTC_GOLDFISH_H

#include "rvvmlib.h"
#include "plic.h"

#define RTC_GOLDFISH_DEFAULT_MMIO 0x101000

PUBLIC rvvm_mmio_dev_t* rtc_goldfish_init(rvvm_machine_t* machine, rvvm_addr_t base_addr,
                                          plic_ctx_t* plic, uint32_t irq);
PUBLIC rvvm_mmio_dev_t* rtc_goldfish_init_auto(rvvm_machine_t* machine);

#endif

