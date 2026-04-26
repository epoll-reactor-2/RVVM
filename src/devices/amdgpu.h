/*
amdgpu.h - AMD graphic cards
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_AMDGPU_H
#define RVVM_AMDGPU_H

#include "rvvmlib.h"
#include "pci-bus.h"

PUBLIC pci_dev_t *amdgpu_init(pci_bus_t *pci_bus);
PUBLIC pci_dev_t *amdgpu_init_auto(rvvm_machine_t *machine);

#endif
