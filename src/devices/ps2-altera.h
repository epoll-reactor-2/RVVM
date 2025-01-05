/*
ps2-altera.h - Altera PS2 Controller
Copyright (C) 2021  cerg2010cerg2010 <github.com/cerg2010cerg2010>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef PS2_ALTERA_H
#define PS2_ALTERA_H

#include "rvvmlib.h"
#include "plic.h"
#include "chardev.h"

#define ALTPS2_MMIO_SIZE 0x8

void altps2_init(rvvm_machine_t* machine, rvvm_addr_t base_addr, plic_ctx_t* plic, uint32_t irq, chardev_t* chardev);

#endif
