/*
rvvm_types.c - RVVM integer types
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef RVVM_TYPES_H
#define RVVM_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include <inttypes.h>

// Fix for MSVCRT printf specifier
#if defined(_WIN32) && defined(PRIx64)
#undef PRIx64
#define PRIx64 "I64x"
#endif

#ifndef PRIx64
#define PRIx64 "llx"
#endif

#ifndef PRIx32
#define PRIx32 "x"
#endif

#ifdef __SIZEOF_INT128__
#define INT128_SUPPORT 1
typedef unsigned __int128 uint128_t;
typedef __int128 int128_t;
#endif

// Max XLEN/SXLEN values
#ifdef USE_RV64
typedef uint64_t maxlen_t;
typedef int64_t smaxlen_t;
#define MAX_XLEN 64
#define MAX_SHAMT_BITS 6
#define PRIxXLEN PRIx64
#else
typedef uint32_t maxlen_t;
typedef int32_t smaxlen_t;
#define MAX_XLEN 32
#define MAX_SHAMT_BITS 5
#define PRIxXLEN PRIx32
#endif

typedef double fmaxlen_t;

// Distinguish between virtual and physical addresses
typedef maxlen_t virt_addr_t;
typedef maxlen_t phys_addr_t;

typedef uint8_t regid_t;  // Register index
typedef uint8_t bitcnt_t; // Bits count
typedef uint8_t* vmptr_t; // Pointer to VM memory

#endif
