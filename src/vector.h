/*
vector.h - Vector Container
Copyright (C) 2021  LekKit <github.com/LekKit>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#ifndef VECTOR_H
#define VECTOR_H

#include <stddef.h>
#include <string.h>
#include "utils.h"

#define vector_t(type) struct {type* data; size_t size; size_t count;}

// Empty vectors do not preallocate memory
// This allows static initialization & conserves memory
#define VECTOR_INIT {0}

// Grow factor: 1.5 (Better memory reusage), initial capacity: 2
#define VECTOR_GROW(vec) \
    if ((vec).count >= (vec).size) { \
        (vec).size += (vec).size >> 1; \
        if ((vec).size == 0) (vec).size = 2; \
        (vec).data = safe_realloc((vec).data, (vec).size * sizeof(*(vec).data)); \
    }

#define vector_init(vec) \
do { \
    (vec).data = NULL; \
    (vec).size = 0; \
    (vec).count = 0; \
} while(0)

// May be called multiple times, the vector is empty yet reusable afterwards
// Semantically identical to clear(), but also frees memory
#define vector_free(vec) \
do { \
    free((vec).data); \
    (vec).data = NULL; \
    (vec).size = 0; \
    (vec).count = 0; \
} while(0)

#define vector_clear(vec) do { (vec).count = 0; } while(0)

#define vector_size(vec) (vec).count

#define vector_capacity(vec) (vec).size

#define vector_at(vec, pos) (vec).data[pos]

#define vector_push_back(vec, val) \
do { \
    VECTOR_GROW(vec); \
    (vec).data[(vec).count++] = val; \
} while(0)

#define vector_insert(vec, pos, val) \
do { \
    VECTOR_GROW(vec); \
    for (size_t _vec_i=(vec).count; _vec_i>pos; --_vec_i) (vec).data[_vec_i] = (vec).data[_vec_i-1]; \
    (vec).data[pos] = val; \
    (vec).count++; \
} while(0)

#define vector_emplace_back(vec) \
do { \
    VECTOR_GROW(vec); \
    memset(&(vec).data[(vec).count++], 0, sizeof(*(vec).data)); \
} while(0)

#define vector_emplace(vec, pos) \
do { \
    VECTOR_GROW(vec); \
    for (size_t _vec_i=(vec).count; _vec_i>pos; --_vec_i) (vec).data[_vec_i] = (vec).data[_vec_i-1]; \
    memset(&(vec).data[pos], 0, sizeof(*(vec).data)); \
    (vec).count++; \
} while(0)

#define vector_erase(vec, pos) \
do { \
    if (pos < (vec).count) { \
        (vec).count--; \
        for (size_t _vec_i=pos; _vec_i<(vec).count; ++_vec_i) (vec).data[_vec_i] = (vec).data[_vec_i+1]; \
    } \
} while(0)

// Be sure to break loop after vector_erase() since it invalidates forward iterators
#define vector_foreach(vec, iter) \
    for (size_t iter=0; iter<(vec).count; ++iter)

// Iterates the vector in reversed order, which is safe for vector_erase()
#define vector_foreach_back(vec, iter) \
    for (size_t iter=(vec).count; iter--;)

#endif
