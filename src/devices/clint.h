/*
clint.h - RISC-V Advanced Core Local Interruptor
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef CLINT_H
#define CLINT_H

#include "rvvmlib.h"

#define CLINT_DEFAULT_MMIO 0x2000000

PUBLIC void clint_init(rvvm_machine_t* machine, rvvm_addr_t addr);
PUBLIC void clint_init_auto(rvvm_machine_t* machine);

#endif
