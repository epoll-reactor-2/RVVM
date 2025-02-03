/*
chardev_term.c - Terminal backend for UART
Copyright (C) 2023  LekKit <github.com/LekKit>
                    宋文武 <iyzsong@envs.net>

This Source Code Form is subject to the terms of the Mozilla Public
License, v. 2.0. If a copy of the MPL was not distributed with this
file, You can obtain one at https://mozilla.org/MPL/2.0/.
*/

#include "chardev.h"

#if (defined(__unix__) || defined(__APPLE__) || defined(__HAIKU__)) && !defined(__EMSCRIPTEN__)
#include <sys/types.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

#define POSIX_TERM_IMPL

#elif defined(_WIN32) && !defined(UNDER_CE)
#include <windows.h>
#include <conio.h>

#define WIN32_TERM_IMPL

#else
#include <stdio.h>
#warning No UART input support!

#endif

// RVVM internal headers come after system headers because of safe_free()
#include "spinlock.h"
#include "rvtimer.h"
#include "ringbuf.h"
#include "utils.h"
#include "mem_ops.h"

#if defined(POSIX_TERM_IMPL)
static struct termios orig_term_opts = {0};
#elif defined(WIN32_TERM_IMPL)
static DWORD orig_input_mode = 0;
static DWORD orig_output_mode = 0;
#endif

static void term_origmode(void)
{
#if defined(POSIX_TERM_IMPL)
    tcsetattr(0, TCSAFLUSH, &orig_term_opts);
#elif defined(WIN32_TERM_IMPL)
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), orig_input_mode);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), orig_output_mode);
#endif
}

static void term_rawmode(void)
{
#if defined(POSIX_TERM_IMPL)
    struct termios term_opts = {
        .c_oflag = OPOST | ONLCR,
        .c_cflag = CLOCAL | CREAD | CS8,
        .c_cc[VMIN] = 1,
    };
    tcgetattr(0, &orig_term_opts);
    tcsetattr(0, TCSANOW, &term_opts);
#elif defined(WIN32_TERM_IMPL)
    SetConsoleOutputCP(CP_UTF8);
    GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &orig_input_mode);
    GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &orig_output_mode);

    // Pre-Win10 raw console setup
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), 0x0);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), 0x1);

    // ENABLE_VIRTUAL_TERMINAL_INPUT
    SetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), 0x200);
    // ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), 0x5);
#endif
    call_at_deinit(term_origmode);
}

typedef struct {
    chardev_t chardev;
    spinlock_t lock;
    uint32_t flags;
    int rfd, wfd;
    ringbuf_t rx, tx;
    bool ctrl_a;
} chardev_term_t;

static uint32_t term_update_flags(chardev_term_t* term)
{
    uint32_t flags = 0;
    if (ringbuf_avail(&term->rx)) flags |= CHARDEV_RX;
    if (ringbuf_space(&term->tx)) flags |= CHARDEV_TX;

    return flags & ~atomic_swap_uint32(&term->flags, flags);
}

static void term_push_io(chardev_term_t* term, char* buffer, size_t* rx_size, size_t* tx_size)
{
    size_t to_read = rx_size ? *rx_size : 0;
    size_t to_write = tx_size ? *tx_size : 0;
    if (rx_size) *rx_size = 0;
    if (tx_size) *tx_size = 0;
    UNUSED(term);
#if defined(POSIX_TERM_IMPL)
    fd_set rfds, wfds;
    struct timeval timeout = {0};
    int nfds = EVAL_MAX(term->rfd, term->wfd) + 1;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    if (to_read) FD_SET(term->rfd, &rfds);
    if (to_write) FD_SET(term->wfd, &wfds);
    if ((to_read || to_write) && select(nfds, to_read ? &rfds : NULL, to_write ? &wfds : NULL, NULL, &timeout) > 0) {
        if (to_write && FD_ISSET(term->wfd, &wfds)) {
            int tmp = write(term->wfd, buffer, to_write);
            *tx_size = tmp > 0 ? tmp : 0;
        }
        if (to_read && FD_ISSET(term->rfd, &rfds)) {
            int tmp = read(term->rfd, buffer, to_read);
            *rx_size = tmp > 0 ? tmp : 0;
        }
    }
#elif defined(WIN32_TERM_IMPL)
    if (to_write) {
        DWORD count = 0;
        WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), buffer, to_write, &count, NULL);
        *tx_size = count;
    }
    if (to_read && _kbhit()) {
        wchar_t w_buf[64] = {0};
        size_t count = EVAL_MIN(to_read / 6, STATIC_ARRAY_SIZE(w_buf));
        DWORD w_chars = 0;
        ReadConsoleW(GetStdHandle(STD_INPUT_HANDLE), w_buf, count, &w_chars, NULL);
        *rx_size = WideCharToMultiByte(CP_UTF8, 0,
            w_buf, w_chars, buffer, to_read, NULL, NULL);
    }
#else
    UNUSED(to_read);
    if (to_write) {
        printf("%s", buffer);
        *tx_size = to_write;
    }
#endif
}

// Handles VM-related hotkeys
static void term_process_input(chardev_term_t* term, char* buffer, size_t size)
{
    for (size_t i=0; i<size; ++i) {
        if (term->ctrl_a) {
            if (buffer[i] == 'x') {
                // Exit on Ctrl+A, x
                exit(0);
            }
        }

        // Ctrl+A (SOH VT code)
        term->ctrl_a = buffer[i] == 1;
    }
}

static void term_update(chardev_t* dev)
{
    chardev_term_t* term = dev->data;

    if (spin_try_lock(&term->lock)) {
        char buffer[256] = {0};
        size_t rx_size = EVAL_MIN(ringbuf_space(&term->rx), sizeof(buffer));
        size_t tx_size = ringbuf_peek(&term->tx, buffer, sizeof(buffer));

        term_push_io(term, buffer, &rx_size, &tx_size);
        term_process_input(term, buffer, rx_size);

        ringbuf_write(&term->rx, buffer, rx_size);
        ringbuf_skip(&term->tx, tx_size);
        uint32_t flags = term_update_flags(term);
        spin_unlock(&term->lock);

        if (flags) chardev_notify(&term->chardev, flags);
    }
}

static uint32_t term_poll(chardev_t* dev)
{
    chardev_term_t* term = dev->data;
    return atomic_load_uint32_relax(&term->flags);
}

static size_t term_read(chardev_t* dev, void* buf, size_t nbytes)
{
    if (term_poll(dev) & CHARDEV_RX) {
        chardev_term_t* term = dev->data;
        spin_lock(&term->lock);
        size_t ret = ringbuf_read(&term->rx, buf, nbytes);
        if (!ringbuf_avail(&term->rx)) {
            char buffer[256] = {0};
            size_t rx_size = sizeof(buffer);
            term_push_io(term, buffer, &rx_size, NULL);
            ringbuf_write(&term->rx, buffer, rx_size);
        }
        term_update_flags(term);
        spin_unlock(&term->lock);
        return ret;
    }
    return 0;
}

static size_t term_write(chardev_t* dev, const void* buf, size_t nbytes)
{
    if (term_poll(dev) & CHARDEV_TX) {
        chardev_term_t* term = dev->data;
        spin_lock(&term->lock);
        size_t ret = ringbuf_write(&term->tx, buf, nbytes);
        if (!ringbuf_space(&term->tx)) {
            char buffer[257] = {0};
            size_t tx_size = ringbuf_peek(&term->tx, buffer, sizeof(buffer) - 1);
            term_push_io(term, buffer, NULL, &tx_size);
            ringbuf_skip(&term->tx, tx_size);
        }
        term_update_flags(term);
        spin_unlock(&term->lock);
        return ret;
    }
    return 0;
}

static void term_remove(chardev_t* dev)
{
    chardev_term_t* term = dev->data;
    term_update(dev);
    ringbuf_destroy(&term->rx);
    ringbuf_destroy(&term->tx);
#ifdef POSIX_TERM_IMPL
    if (term->rfd != 0) close(term->rfd);
    if (term->wfd != 1 && term->wfd != term->rfd) close(term->wfd);
#endif
    free(term);
}

PUBLIC chardev_t* chardev_term_create(void)
{
    DO_ONCE(term_rawmode());
    return chardev_fd_create(0, 1);
}

PUBLIC chardev_t* chardev_fd_create(int rfd, int wfd)
{
#ifndef POSIX_TERM_IMPL
    if (rfd != 0 || wfd != 1) {
        rvvm_error("No FD chardev support on non-POSIX");
        return NULL;
    }
#endif

    chardev_term_t* term = safe_new_obj(chardev_term_t);
    ringbuf_create(&term->rx, 256);
    ringbuf_create(&term->tx, 256);
    term->chardev.data = term;
    term->chardev.read = term_read;
    term->chardev.write = term_write;
    term->chardev.poll = term_poll;
    term->chardev.update = term_update;
    term->chardev.remove = term_remove;
    term->rfd = rfd;
    term->wfd = wfd;

    return &term->chardev;
}

PUBLIC chardev_t* chardev_pty_create(const char* path)
{
    if (rvvm_strcmp(path, "stdout")) return chardev_term_create();
    if (rvvm_strcmp(path, "null")) return NULL; // NULL chardev
#ifdef POSIX_TERM_IMPL
    int fd = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd >= 0) return chardev_fd_create(fd, fd);
    rvvm_error("Could not open PTY %s", path);
#else
    rvvm_error("No PTY chardev support on non-POSIX");
#endif
    return NULL;
}
