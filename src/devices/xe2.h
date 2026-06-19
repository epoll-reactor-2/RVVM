/*
xe2.h - Intel XE2 graphics
Copyright (C) 2026  David Korenchuk <github.com/epoll-reactor-2>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_XE2_H
#define RVVM_XE2_H

#include "pci-bus.h"
#include "rvvm/rvvm_fb.h"

// Pass a host fbdev (from a GUI window) to drive the emulated display onto
// screen, or NULL to run the GPU headless.
PUBLIC pci_dev_t *xe2_init(pci_bus_t *pci_bus, rvvm_fbdev_t *fbdev);
PUBLIC pci_dev_t *xe2_init_auto(rvvm_machine_t *machine, rvvm_fbdev_t *fbdev);

#endif
