/*
riscv_interpreter64.c - RISC-V 64-bit template interpreter
Copyright (C) 2024  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#define RV64
#define riscv_run_interpreter riscv64_run_interpreter

#include "compiler.h"

// The interpreter is faster on GCC with -O3 optimization for whatever reason.
// This overrides the optimization level set for the whole codebase
SOURCE_OPTIMIZATION_O3

#include "riscv_interpreter.h"
