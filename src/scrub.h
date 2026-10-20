/* Copyright (c) Mark Harmstone 2017
 *
 * This file is part of WinBtrfs.
 *
 * WinBtrfs is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public Licence as published by
 * the Free Software Foundation, either version 3 of the Licence, or
 * (at your option) any later version.
 *
 * WinBtrfs is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public Licence for more details.
 *
 * You should have received a copy of the GNU Lesser General Public Licence
 * along with WinBtrfs.  If not, see <http://www.gnu.org/licenses/>. */

#pragma once

#include "btrfs_drv.h"

struct _scrub_context;

typedef struct {
    struct _scrub_context* context;
    PIRP Irp;
    uint64_t start;
    uint32_t length;
    IO_STATUS_BLOCK iosb;
    uint8_t* buf;
    bool csum_error;
    void* bad_csums;
} scrub_context_stripe;

typedef struct _scrub_context {
    KEVENT Event;
    scrub_context_stripe* stripes;
    LONG stripes_left;
} scrub_context;

NTSTATUS scrub_extent_raid10(device_extension* Vcb, chunk* c, uint64_t offset, uint32_t length, uint16_t startoffstripe, void* csum, scrub_context* context);
void log_error(device_extension* Vcb, uint64_t addr, uint64_t devid, bool metadata, bool recoverable, bool parity);

typedef struct {
    uint8_t* buf;
    PIRP Irp;
    void* context;
    IO_STATUS_BLOCK iosb;
    uint64_t offset;
    bool rewrite, missing;
    RTL_BITMAP error;
    ULONG* errorarr;
} scrub_context_raid56_stripe;

typedef struct {
    scrub_context_raid56_stripe* stripes;
    LONG stripes_left;
    KEVENT Event;
    RTL_BITMAP alloc;
    RTL_BITMAP has_csum;
    RTL_BITMAP is_tree;
    void* csum;
    uint8_t* parity_scratch;
    uint8_t* parity_scratch2;
} scrub_context_raid56;

void scrub_raid5_stripe(device_extension* Vcb, chunk* c, scrub_context_raid56* context, uint64_t stripe_start, uint64_t bit_start,
                        uint64_t num, uint16_t missing_devices);
NTSTATUS scrub_raid6_stripe(device_extension* Vcb, chunk* c, scrub_context_raid56* context, uint64_t stripe_start, uint64_t bit_start,
                            uint64_t num, uint16_t missing_devices);
NTSTATUS scrub_chunk_raid56_stripe_run(device_extension* Vcb, chunk* c, uint64_t stripe_start, uint64_t stripe_end);
