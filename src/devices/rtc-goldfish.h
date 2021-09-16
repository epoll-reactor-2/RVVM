/*
rtc-goldfish.h - Goldfish Real-time Clock
Copyright (C) 2021  LekKit <github.com/LekKit>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef RTC_GOLDFISH_H
#define RTC_GOLDFISH_H

#include "rvvm.h"

void rtc_goldfish_init(rvvm_machine_t* machine, paddr_t base_addr, void* intc_data, uint32_t irq);

#endif
 
