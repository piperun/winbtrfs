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

/* RAID5/6 stripe verification and reconstruction. */
#include "scrub.h"

void scrub_raid5_stripe(device_extension* Vcb, chunk* c, scrub_context_raid56* context, uint64_t stripe_start, uint64_t bit_start,
                               uint64_t num, uint16_t missing_devices) {
    ULONG sectors_per_stripe = (ULONG)(c->chunk_item->stripe_len >> Vcb->sector_shift), off;
    uint16_t stripe, parity = (bit_start + num + c->chunk_item->num_stripes - 1) % c->chunk_item->num_stripes;
    uint64_t stripeoff;

    stripe = (parity + 1) % c->chunk_item->num_stripes;
    off = (ULONG)(bit_start + num - stripe_start) * sectors_per_stripe * (c->chunk_item->num_stripes - 1);
    stripeoff = num * sectors_per_stripe;

    if (missing_devices == 0)
        RtlCopyMemory(context->parity_scratch, &context->stripes[parity].buf[num * c->chunk_item->stripe_len], (ULONG)c->chunk_item->stripe_len);

    while (stripe != parity) {
        RtlClearAllBits(&context->stripes[stripe].error);

        for (ULONG i = 0; i < sectors_per_stripe; i++) {
            if (c->devices[stripe]->devobj && RtlCheckBit(&context->alloc, off)) {
                if (RtlCheckBit(&context->is_tree, off)) {
                    struct btrfs_header* th = (struct btrfs_header*)&context->stripes[stripe].buf[stripeoff << Vcb->sector_shift];
                    uint64_t addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 1) * c->chunk_item->stripe_len) + (off << Vcb->sector_shift);

                    if (!check_tree_checksum(Vcb, th) || th->bytenr != addr) {
                        RtlSetBits(&context->stripes[stripe].error, i, Vcb->superblock.nodesize >> Vcb->sector_shift);
                        log_device_error(Vcb, c->devices[stripe], BTRFS_DEV_STAT_CORRUPTION_ERRS);

                        if (missing_devices > 0)
                            log_error(Vcb, addr, c->devices[stripe]->devitem.devid, true, false, false);
                    }

                    off += Vcb->superblock.nodesize >> Vcb->sector_shift;
                    stripeoff += Vcb->superblock.nodesize >> Vcb->sector_shift;
                    i += (Vcb->superblock.nodesize >> Vcb->sector_shift) - 1;

                    continue;
                } else if (RtlCheckBit(&context->has_csum, off)) {
                    if (!check_sector_csum(Vcb, context->stripes[stripe].buf + (stripeoff << Vcb->sector_shift), (uint8_t*)context->csum + (Vcb->csum_size * off))) {
                        RtlSetBit(&context->stripes[stripe].error, i);
                        log_device_error(Vcb, c->devices[stripe], BTRFS_DEV_STAT_CORRUPTION_ERRS);

                        if (missing_devices > 0) {
                            uint64_t addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 1) * c->chunk_item->stripe_len) + (off << Vcb->sector_shift);

                            log_error(Vcb, addr, c->devices[stripe]->devitem.devid, false, false, false);
                        }
                    }
                }
            }

            off++;
            stripeoff++;
        }

        if (missing_devices == 0)
            do_xor(context->parity_scratch, &context->stripes[stripe].buf[num * c->chunk_item->stripe_len], (ULONG)c->chunk_item->stripe_len);

        stripe = (stripe + 1) % c->chunk_item->num_stripes;
        stripeoff = num * sectors_per_stripe;
    }

    // check parity

    if (missing_devices == 0) {
        RtlClearAllBits(&context->stripes[parity].error);

        for (ULONG i = 0; i < sectors_per_stripe; i++) {
            ULONG o, j;

            o = i << Vcb->sector_shift;
            for (j = 0; j < Vcb->superblock.sectorsize; j++) { // FIXME - use SSE
                if (context->parity_scratch[o] != 0) {
                    RtlSetBit(&context->stripes[parity].error, i);
                    break;
                }
                o++;
            }
        }
    }

    // log and fix errors

    if (missing_devices > 0)
        return;

    for (ULONG i = 0; i < sectors_per_stripe; i++) {
        ULONG num_errors = 0, bad_off = 0;
        uint64_t bad_stripe = 0;
        bool alloc = false;

        stripe = (parity + 1) % c->chunk_item->num_stripes;
        off = (ULONG)((bit_start + num - stripe_start) * sectors_per_stripe * (c->chunk_item->num_stripes - 1)) + i;

        while (stripe != parity) {
            if (RtlCheckBit(&context->alloc, off)) {
                alloc = true;

                if (RtlCheckBit(&context->stripes[stripe].error, i)) {
                    bad_stripe = stripe;
                    bad_off = off;
                    num_errors++;
                }
            }

            off += sectors_per_stripe;
            stripe = (stripe + 1) % c->chunk_item->num_stripes;
        }

        if (!alloc)
            continue;

        if (num_errors == 0 && !RtlCheckBit(&context->stripes[parity].error, i)) // everything fine
            continue;

        if (num_errors == 0 && RtlCheckBit(&context->stripes[parity].error, i)) { // parity error
            uint64_t addr;

            do_xor(&context->stripes[parity].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                   &context->parity_scratch[i << Vcb->sector_shift],
                   Vcb->superblock.sectorsize);

            bad_off = (ULONG)((bit_start + num - stripe_start) * sectors_per_stripe * (c->chunk_item->num_stripes - 1)) + i;
            addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 1) * c->chunk_item->stripe_len) + (bad_off << Vcb->sector_shift);

            context->stripes[parity].rewrite = true;

            log_error(Vcb, addr, c->devices[parity]->devitem.devid, false, true, true);
            log_device_error(Vcb, c->devices[parity], BTRFS_DEV_STAT_CORRUPTION_ERRS);
        } else if (num_errors == 1) {
            uint64_t addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 1) * c->chunk_item->stripe_len) + (bad_off << Vcb->sector_shift);

            if (RtlCheckBit(&context->is_tree, bad_off)) {
                struct btrfs_header* th;

                do_xor(&context->parity_scratch[i << Vcb->sector_shift],
                       &context->stripes[bad_stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                       Vcb->superblock.nodesize);

                th = (struct btrfs_header*)&context->parity_scratch[i << Vcb->sector_shift];

                if (check_tree_checksum(Vcb, th) && th->bytenr == addr) {
                    RtlCopyMemory(&context->stripes[bad_stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                  &context->parity_scratch[i << Vcb->sector_shift], Vcb->superblock.nodesize);

                    context->stripes[bad_stripe].rewrite = true;

                    RtlClearBits(&context->stripes[bad_stripe].error, i + 1, (Vcb->superblock.nodesize >> Vcb->sector_shift) - 1);

                    log_error(Vcb, addr, c->devices[bad_stripe]->devitem.devid, true, true, false);
                } else
                    log_error(Vcb, addr, c->devices[bad_stripe]->devitem.devid, true, false, false);
            } else {
                uint8_t hash[MAX_HASH_SIZE];

                do_xor(&context->parity_scratch[i << Vcb->sector_shift],
                       &context->stripes[bad_stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                       Vcb->superblock.sectorsize);

                get_sector_csum(Vcb, &context->parity_scratch[i << Vcb->sector_shift], hash);

                if (RtlCompareMemory(hash, (uint8_t*)context->csum + (Vcb->csum_size * bad_off), Vcb->csum_size) == Vcb->csum_size) {
                    RtlCopyMemory(&context->stripes[bad_stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                  &context->parity_scratch[i << Vcb->sector_shift], Vcb->superblock.sectorsize);

                    context->stripes[bad_stripe].rewrite = true;

                    log_error(Vcb, addr, c->devices[bad_stripe]->devitem.devid, false, true, false);
                } else
                    log_error(Vcb, addr, c->devices[bad_stripe]->devitem.devid, false, false, false);
            }
        } else {
            stripe = (parity + 1) % c->chunk_item->num_stripes;
            off = (ULONG)((bit_start + num - stripe_start) * sectors_per_stripe * (c->chunk_item->num_stripes - 1)) + i;

            while (stripe != parity) {
                if (RtlCheckBit(&context->alloc, off)) {
                    if (RtlCheckBit(&context->stripes[stripe].error, i)) {
                        uint64_t addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 1) * c->chunk_item->stripe_len) + (off << Vcb->sector_shift);

                        log_error(Vcb, addr, c->devices[stripe]->devitem.devid, RtlCheckBit(&context->is_tree, off), false, false);
                    }
                }

                off += sectors_per_stripe;
                stripe = (stripe + 1) % c->chunk_item->num_stripes;
            }
        }
    }
}

NTSTATUS scrub_raid6_stripe(device_extension* Vcb, chunk* c, scrub_context_raid56* context,
                                   uint64_t stripe_start, uint64_t bit_start,
                                   uint64_t num, uint16_t missing_devices) {
    ULONG sectors_per_stripe = (ULONG)(c->chunk_item->stripe_len >> Vcb->sector_shift), off;
    uint16_t stripe, parity1 = (bit_start + num + c->chunk_item->num_stripes - 2) % c->chunk_item->num_stripes;
    uint16_t parity2 = (parity1 + 1) % c->chunk_item->num_stripes;
    uint64_t stripeoff;

    stripe = (parity1 + 2) % c->chunk_item->num_stripes;
    off = (ULONG)(bit_start + num - stripe_start) * sectors_per_stripe * (c->chunk_item->num_stripes - 2);
    stripeoff = num * sectors_per_stripe;

    if (c->devices[parity1]->devobj)
        RtlCopyMemory(context->parity_scratch, &context->stripes[parity1].buf[num * c->chunk_item->stripe_len], (ULONG)c->chunk_item->stripe_len);

    if (c->devices[parity2]->devobj)
        RtlZeroMemory(context->parity_scratch2, (ULONG)c->chunk_item->stripe_len);

    while (stripe != parity1) {
        RtlClearAllBits(&context->stripes[stripe].error);

        for (ULONG i = 0; i < sectors_per_stripe; i++) {
            if (c->devices[stripe]->devobj && RtlCheckBit(&context->alloc, off)) {
                if (RtlCheckBit(&context->is_tree, off)) {
                    struct btrfs_header* th = (struct btrfs_header*)&context->stripes[stripe].buf[stripeoff << Vcb->sector_shift];
                    uint64_t addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 2) * c->chunk_item->stripe_len) + (off << Vcb->sector_shift);

                    if (!check_tree_checksum(Vcb, th) || th->bytenr != addr) {
                        RtlSetBits(&context->stripes[stripe].error, i, Vcb->superblock.nodesize >> Vcb->sector_shift);
                        log_device_error(Vcb, c->devices[stripe], BTRFS_DEV_STAT_CORRUPTION_ERRS);

                        if (missing_devices == 2)
                            log_error(Vcb, addr, c->devices[stripe]->devitem.devid, true, false, false);
                    }

                    off += Vcb->superblock.nodesize >> Vcb->sector_shift;
                    stripeoff += Vcb->superblock.nodesize >> Vcb->sector_shift;
                    i += (Vcb->superblock.nodesize >> Vcb->sector_shift) - 1;

                    continue;
                } else if (RtlCheckBit(&context->has_csum, off)) {
                    uint8_t hash[MAX_HASH_SIZE];

                    get_sector_csum(Vcb, context->stripes[stripe].buf + (stripeoff << Vcb->sector_shift), hash);

                    if (RtlCompareMemory(hash, (uint8_t*)context->csum + (Vcb->csum_size * off), Vcb->csum_size) != Vcb->csum_size) {
                        uint64_t addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 2) * c->chunk_item->stripe_len) + (off << Vcb->sector_shift);

                        RtlSetBit(&context->stripes[stripe].error, i);
                        log_device_error(Vcb, c->devices[stripe], BTRFS_DEV_STAT_CORRUPTION_ERRS);

                        if (missing_devices == 2)
                            log_error(Vcb, addr, c->devices[stripe]->devitem.devid, false, false, false);
                    }
                }
            }

            off++;
            stripeoff++;
        }

        if (c->devices[parity1]->devobj)
            do_xor(context->parity_scratch, &context->stripes[stripe].buf[num * c->chunk_item->stripe_len], (uint32_t)c->chunk_item->stripe_len);

        stripe = (stripe + 1) % c->chunk_item->num_stripes;
        stripeoff = num * sectors_per_stripe;
    }

    RtlClearAllBits(&context->stripes[parity1].error);

    if (missing_devices == 0 || (missing_devices == 1 && !c->devices[parity2]->devobj)) {
        // check parity 1

        for (ULONG i = 0; i < sectors_per_stripe; i++) {
            ULONG o, j;

            o = i << Vcb->sector_shift;
            for (j = 0; j < Vcb->superblock.sectorsize; j++) { // FIXME - use SSE
                if (context->parity_scratch[o] != 0) {
                    RtlSetBit(&context->stripes[parity1].error, i);
                    break;
                }
                o++;
            }
        }
    }

    RtlClearAllBits(&context->stripes[parity2].error);

    if (missing_devices == 0 || (missing_devices == 1 && !c->devices[parity1]->devobj)) {
        // check parity 2

        stripe = parity1 == 0 ? (c->chunk_item->num_stripes - 1) : (parity1 - 1);

        while (stripe != parity2) {
            galois_double(context->parity_scratch2, (uint32_t)c->chunk_item->stripe_len);
            do_xor(context->parity_scratch2, &context->stripes[stripe].buf[num * c->chunk_item->stripe_len], (uint32_t)c->chunk_item->stripe_len);

            stripe = stripe == 0 ? (c->chunk_item->num_stripes - 1) : (stripe - 1);
        }

        for (ULONG i = 0; i < sectors_per_stripe; i++) {
            if (RtlCompareMemory(&context->stripes[parity2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                 &context->parity_scratch2[i << Vcb->sector_shift], Vcb->superblock.sectorsize) != Vcb->superblock.sectorsize)
                RtlSetBit(&context->stripes[parity2].error, i);
        }
    }

    if (missing_devices == 2)
        return STATUS_SUCCESS;

    // log and fix errors

    for (ULONG i = 0; i < sectors_per_stripe; i++) {
        ULONG num_errors = 0;
        uint64_t bad_stripe1 = 0, bad_stripe2 = 0;
        ULONG bad_off1 = 0, bad_off2 = 0;
        bool alloc = false;

        stripe = (parity1 + 2) % c->chunk_item->num_stripes;
        off = (ULONG)((bit_start + num - stripe_start) * sectors_per_stripe * (c->chunk_item->num_stripes - 2)) + i;

        while (stripe != parity1) {
            if (RtlCheckBit(&context->alloc, off)) {
                alloc = true;

                if (!c->devices[stripe]->devobj || RtlCheckBit(&context->stripes[stripe].error, i)) {
                    if (num_errors == 0) {
                        bad_stripe1 = stripe;
                        bad_off1 = off;
                    } else if (num_errors == 1) {
                        bad_stripe2 = stripe;
                        bad_off2 = off;
                    }
                    num_errors++;
                }
            }

            off += sectors_per_stripe;
            stripe = (stripe + 1) % c->chunk_item->num_stripes;
        }

        if (!alloc)
            continue;

        if (num_errors == 0 && !RtlCheckBit(&context->stripes[parity1].error, i) && !RtlCheckBit(&context->stripes[parity2].error, i)) // everything fine
            continue;

        if (num_errors == 0) { // parity error
            uint64_t addr;

            if (RtlCheckBit(&context->stripes[parity1].error, i)) {
                do_xor(&context->stripes[parity1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                       &context->parity_scratch[i << Vcb->sector_shift],
                       Vcb->superblock.sectorsize);

                bad_off1 = (ULONG)((bit_start + num - stripe_start) * sectors_per_stripe * (c->chunk_item->num_stripes - 2)) + i;
                addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 2) * c->chunk_item->stripe_len) + (bad_off1 << Vcb->sector_shift);

                context->stripes[parity1].rewrite = true;

                log_error(Vcb, addr, c->devices[parity1]->devitem.devid, false, true, true);
                log_device_error(Vcb, c->devices[parity1], BTRFS_DEV_STAT_CORRUPTION_ERRS);
            }

            if (RtlCheckBit(&context->stripes[parity2].error, i)) {
                RtlCopyMemory(&context->stripes[parity2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                              &context->parity_scratch2[i << Vcb->sector_shift],
                              Vcb->superblock.sectorsize);

                bad_off1 = (ULONG)((bit_start + num - stripe_start) * sectors_per_stripe * (c->chunk_item->num_stripes - 2)) + i;
                addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 2) * c->chunk_item->stripe_len) + (bad_off1 << Vcb->sector_shift);

                context->stripes[parity2].rewrite = true;

                log_error(Vcb, addr, c->devices[parity2]->devitem.devid, false, true, true);
                log_device_error(Vcb, c->devices[parity2], BTRFS_DEV_STAT_CORRUPTION_ERRS);
            }
        } else if (num_errors == 1) {
            uint32_t len;
            uint64_t addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 2) * c->chunk_item->stripe_len) + (bad_off1 << Vcb->sector_shift);
            uint8_t* scratch;

            len = RtlCheckBit(&context->is_tree, bad_off1) ? Vcb->superblock.nodesize : Vcb->superblock.sectorsize;

            scratch = ExAllocatePoolWithTag(PagedPool, len, ALLOC_TAG);
            if (!scratch) {
                ERR("out of memory\n");
                return STATUS_INSUFFICIENT_RESOURCES;
            }

            RtlZeroMemory(scratch, len);

            do_xor(&context->parity_scratch[i << Vcb->sector_shift],
                   &context->stripes[bad_stripe1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)], len);

            stripe = parity1 == 0 ? (c->chunk_item->num_stripes - 1) : (parity1 - 1);

            if (c->devices[parity2]->devobj) {
                uint16_t stripe_num, bad_stripe_num = 0;

                stripe_num = c->chunk_item->num_stripes - 3;
                while (stripe != parity2) {
                    galois_double(scratch, len);

                    if (stripe != bad_stripe1)
                        do_xor(scratch, &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)], len);
                    else
                        bad_stripe_num = stripe_num;

                    stripe = stripe == 0 ? (c->chunk_item->num_stripes - 1) : (stripe - 1);
                    stripe_num--;
                }

                do_xor(scratch, &context->stripes[parity2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)], len);

                if (bad_stripe_num != 0)
                    galois_divpower(scratch, (uint8_t)bad_stripe_num, len);
            }

            if (RtlCheckBit(&context->is_tree, bad_off1)) {
                uint8_t hash1[MAX_HASH_SIZE];
                uint8_t hash2[MAX_HASH_SIZE];
                struct btrfs_header *th1 = NULL, *th2 = NULL;

                if (c->devices[parity1]->devobj) {
                    th1 = (struct btrfs_header*)&context->parity_scratch[i << Vcb->sector_shift];
                    get_tree_checksum(Vcb, th1, hash1);
                }

                if (c->devices[parity2]->devobj) {
                    th2 = (struct btrfs_header*)scratch;
                    get_tree_checksum(Vcb, th2, hash2);
                }

                if ((c->devices[parity1]->devobj && RtlCompareMemory(hash1, th1, Vcb->csum_size) == Vcb->csum_size && th1->bytenr == addr) ||
                    (c->devices[parity2]->devobj && RtlCompareMemory(hash2, th2, Vcb->csum_size) == Vcb->csum_size && th2->bytenr == addr)) {
                    if (!c->devices[parity1]->devobj || RtlCompareMemory(hash1, th1, Vcb->csum_size) != Vcb->csum_size || th1->bytenr != addr) {
                        RtlCopyMemory(&context->stripes[bad_stripe1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                      scratch, Vcb->superblock.nodesize);

                        if (c->devices[parity1]->devobj) {
                            // fix parity 1

                            stripe = (parity1 + 2) % c->chunk_item->num_stripes;

                            RtlCopyMemory(&context->stripes[parity1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                          &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                          Vcb->superblock.nodesize);

                            stripe = (stripe + 1) % c->chunk_item->num_stripes;

                            while (stripe != parity1) {
                                do_xor(&context->stripes[parity1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                       &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                       Vcb->superblock.nodesize);

                                stripe = (stripe + 1) % c->chunk_item->num_stripes;
                            }

                            context->stripes[parity1].rewrite = true;

                            log_error(Vcb, addr, c->devices[parity1]->devitem.devid, false, true, true);
                            log_device_error(Vcb, c->devices[parity1], BTRFS_DEV_STAT_CORRUPTION_ERRS);
                        }
                    } else {
                        RtlCopyMemory(&context->stripes[bad_stripe1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                      &context->parity_scratch[i << Vcb->sector_shift], Vcb->superblock.nodesize);

                        if (!c->devices[parity2]->devobj || RtlCompareMemory(hash2, th2, Vcb->csum_size) != Vcb->csum_size || th2->bytenr != addr) {
                            // fix parity 2
                            stripe = parity1 == 0 ? (c->chunk_item->num_stripes - 1) : (parity1 - 1);

                            if (c->devices[parity2]->devobj) {
                                RtlCopyMemory(&context->stripes[parity2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                              &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                              Vcb->superblock.nodesize);

                                stripe = stripe == 0 ? (c->chunk_item->num_stripes - 1) : (stripe - 1);

                                while (stripe != parity2) {
                                    galois_double(&context->stripes[parity2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)], Vcb->superblock.nodesize);

                                    do_xor(&context->stripes[parity2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                           &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                           Vcb->superblock.nodesize);

                                    stripe = stripe == 0 ? (c->chunk_item->num_stripes - 1) : (stripe - 1);
                                }

                                context->stripes[parity2].rewrite = true;

                                log_error(Vcb, addr, c->devices[parity2]->devitem.devid, false, true, true);
                                log_device_error(Vcb, c->devices[parity2], BTRFS_DEV_STAT_CORRUPTION_ERRS);
                            }
                        }
                    }

                    context->stripes[bad_stripe1].rewrite = true;

                    RtlClearBits(&context->stripes[bad_stripe1].error, i + 1, (Vcb->superblock.nodesize >> Vcb->sector_shift) - 1);

                    log_error(Vcb, addr, c->devices[bad_stripe1]->devitem.devid, true, true, false);
                } else
                    log_error(Vcb, addr, c->devices[bad_stripe1]->devitem.devid, true, false, false);
            } else {
                uint8_t hash1[MAX_HASH_SIZE];
                uint8_t hash2[MAX_HASH_SIZE];

                if (c->devices[parity1]->devobj)
                    get_sector_csum(Vcb, &context->parity_scratch[i << Vcb->sector_shift], hash1);

                if (c->devices[parity2]->devobj)
                    get_sector_csum(Vcb, scratch, hash2);

                if ((c->devices[parity1]->devobj && RtlCompareMemory(hash1, (uint8_t*)context->csum + (bad_off1 * Vcb->csum_size), Vcb->csum_size) == Vcb->csum_size) ||
                    (c->devices[parity2]->devobj && RtlCompareMemory(hash2, (uint8_t*)context->csum + (bad_off1 * Vcb->csum_size), Vcb->csum_size) == Vcb->csum_size)) {
                    if (c->devices[parity2]->devobj && RtlCompareMemory(hash2, (uint8_t*)context->csum + (bad_off1 * Vcb->csum_size), Vcb->csum_size) == Vcb->csum_size) {
                        RtlCopyMemory(&context->stripes[bad_stripe1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                      scratch, Vcb->superblock.sectorsize);

                        if (c->devices[parity1]->devobj && RtlCompareMemory(hash1, (uint8_t*)context->csum + (bad_off1 * Vcb->csum_size), Vcb->csum_size) != Vcb->csum_size) {
                            // fix parity 1

                            stripe = (parity1 + 2) % c->chunk_item->num_stripes;

                            RtlCopyMemory(&context->stripes[parity1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                          &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                        Vcb->superblock.sectorsize);

                            stripe = (stripe + 1) % c->chunk_item->num_stripes;

                            while (stripe != parity1) {
                                do_xor(&context->stripes[parity1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                       &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                       Vcb->superblock.sectorsize);

                                stripe = (stripe + 1) % c->chunk_item->num_stripes;
                            }

                            context->stripes[parity1].rewrite = true;

                            log_error(Vcb, addr, c->devices[parity1]->devitem.devid, false, true, true);
                            log_device_error(Vcb, c->devices[parity1], BTRFS_DEV_STAT_CORRUPTION_ERRS);
                        }
                    } else {
                        RtlCopyMemory(&context->stripes[bad_stripe1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                      &context->parity_scratch[i << Vcb->sector_shift], Vcb->superblock.sectorsize);

                        if (c->devices[parity2]->devobj && RtlCompareMemory(hash2, (uint8_t*)context->csum + (bad_off1 * Vcb->csum_size), Vcb->csum_size) != Vcb->csum_size) {
                            // fix parity 2
                            stripe = parity1 == 0 ? (c->chunk_item->num_stripes - 1) : (parity1 - 1);

                            RtlCopyMemory(&context->stripes[parity2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                          &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                          Vcb->superblock.sectorsize);

                            stripe = stripe == 0 ? (c->chunk_item->num_stripes - 1) : (stripe - 1);

                            while (stripe != parity2) {
                                galois_double(&context->stripes[parity2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)], Vcb->superblock.sectorsize);

                                do_xor(&context->stripes[parity2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                       &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                       Vcb->superblock.sectorsize);

                                stripe = stripe == 0 ? (c->chunk_item->num_stripes - 1) : (stripe - 1);
                            }

                            context->stripes[parity2].rewrite = true;

                            log_error(Vcb, addr, c->devices[parity2]->devitem.devid, false, true, true);
                            log_device_error(Vcb, c->devices[parity2], BTRFS_DEV_STAT_CORRUPTION_ERRS);
                        }
                    }

                    context->stripes[bad_stripe1].rewrite = true;

                    log_error(Vcb, addr, c->devices[bad_stripe1]->devitem.devid, false, true, false);
                } else
                    log_error(Vcb, addr, c->devices[bad_stripe1]->devitem.devid, false, false, false);
            }

            ExFreePool(scratch);
        } else if (num_errors == 2 && missing_devices == 0) {
            uint16_t x = 0, y = 0, k;
            uint64_t addr;
            uint32_t len = (RtlCheckBit(&context->is_tree, bad_off1) || RtlCheckBit(&context->is_tree, bad_off2)) ? Vcb->superblock.nodesize : Vcb->superblock.sectorsize;
            uint8_t gyx, gx, denom, a, b, *p, *q, *pxy, *qxy;
            uint32_t j;

            stripe = parity1 == 0 ? (c->chunk_item->num_stripes - 1) : (parity1 - 1);

            // put qxy in parity_scratch
            // put pxy in parity_scratch2

            k = c->chunk_item->num_stripes - 3;
            if (stripe == bad_stripe1 || stripe == bad_stripe2) {
                RtlZeroMemory(&context->parity_scratch[i << Vcb->sector_shift], len);
                RtlZeroMemory(&context->parity_scratch2[i << Vcb->sector_shift], len);

                if (stripe == bad_stripe1)
                    x = k;
                else
                    y = k;
            } else {
                RtlCopyMemory(&context->parity_scratch[i << Vcb->sector_shift],
                              &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)], len);
                RtlCopyMemory(&context->parity_scratch2[i << Vcb->sector_shift],
                              &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)], len);
            }

            stripe = stripe == 0 ? (c->chunk_item->num_stripes - 1) : (stripe - 1);

            k--;
            do {
                galois_double(&context->parity_scratch[i << Vcb->sector_shift], len);

                if (stripe != bad_stripe1 && stripe != bad_stripe2) {
                    do_xor(&context->parity_scratch[i << Vcb->sector_shift],
                           &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)], len);
                    do_xor(&context->parity_scratch2[i << Vcb->sector_shift],
                           &context->stripes[stripe].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)], len);
                } else if (stripe == bad_stripe1)
                    x = k;
                else if (stripe == bad_stripe2)
                    y = k;

                stripe = stripe == 0 ? (c->chunk_item->num_stripes - 1) : (stripe - 1);
                k--;
            } while (stripe != parity2);

            gyx = gpow2(y > x ? (y-x) : (255-x+y));
            gx = gpow2(255-x);

            denom = gdiv(1, gyx ^ 1);
            a = gmul(gyx, denom);
            b = gmul(gx, denom);

            p = &context->stripes[parity1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)];
            q = &context->stripes[parity2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)];
            pxy = &context->parity_scratch2[i << Vcb->sector_shift];
            qxy = &context->parity_scratch[i << Vcb->sector_shift];

            for (j = 0; j < len; j++) {
                *qxy = gmul(a, *p ^ *pxy) ^ gmul(b, *q ^ *qxy);

                p++;
                q++;
                pxy++;
                qxy++;
            }

            do_xor(&context->parity_scratch2[i << Vcb->sector_shift], &context->parity_scratch[i << Vcb->sector_shift], len);
            do_xor(&context->parity_scratch2[i << Vcb->sector_shift], &context->stripes[parity1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)], len);

            addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 2) * c->chunk_item->stripe_len) + (bad_off1 << Vcb->sector_shift);

            if (RtlCheckBit(&context->is_tree, bad_off1)) {
                struct btrfs_header* th = (struct btrfs_header*)&context->parity_scratch[i << Vcb->sector_shift];

                if (check_tree_checksum(Vcb, th) && th->bytenr == addr) {
                    RtlCopyMemory(&context->stripes[bad_stripe1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                  &context->parity_scratch[i << Vcb->sector_shift], Vcb->superblock.nodesize);

                    context->stripes[bad_stripe1].rewrite = true;

                    RtlClearBits(&context->stripes[bad_stripe1].error, i + 1, (Vcb->superblock.nodesize >> Vcb->sector_shift) - 1);

                    log_error(Vcb, addr, c->devices[bad_stripe1]->devitem.devid, true, true, false);
                } else
                    log_error(Vcb, addr, c->devices[bad_stripe1]->devitem.devid, true, false, false);
            } else {
                if (check_sector_csum(Vcb, &context->parity_scratch[i << Vcb->sector_shift], (uint8_t*)context->csum + (Vcb->csum_size * bad_off1))) {
                    RtlCopyMemory(&context->stripes[bad_stripe1].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                  &context->parity_scratch[i << Vcb->sector_shift], Vcb->superblock.sectorsize);

                    context->stripes[bad_stripe1].rewrite = true;

                    log_error(Vcb, addr, c->devices[bad_stripe1]->devitem.devid, false, true, false);
                } else
                    log_error(Vcb, addr, c->devices[bad_stripe1]->devitem.devid, false, false, false);
            }

            addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 2) * c->chunk_item->stripe_len) + (bad_off2 << Vcb->sector_shift);

            if (RtlCheckBit(&context->is_tree, bad_off2)) {
                struct btrfs_header* th = (struct btrfs_header*)&context->parity_scratch2[i << Vcb->sector_shift];

                if (check_tree_checksum(Vcb, th) && th->bytenr == addr) {
                    RtlCopyMemory(&context->stripes[bad_stripe2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                  &context->parity_scratch2[i << Vcb->sector_shift], Vcb->superblock.nodesize);

                    context->stripes[bad_stripe2].rewrite = true;

                    RtlClearBits(&context->stripes[bad_stripe2].error, i + 1, (Vcb->superblock.nodesize >> Vcb->sector_shift) - 1);

                    log_error(Vcb, addr, c->devices[bad_stripe2]->devitem.devid, true, true, false);
                } else
                    log_error(Vcb, addr, c->devices[bad_stripe2]->devitem.devid, true, false, false);
            } else {
                if (check_sector_csum(Vcb, &context->parity_scratch2[i << Vcb->sector_shift], (uint8_t*)context->csum + (Vcb->csum_size * bad_off2))) {
                    RtlCopyMemory(&context->stripes[bad_stripe2].buf[(num * c->chunk_item->stripe_len) + (i << Vcb->sector_shift)],
                                  &context->parity_scratch2[i << Vcb->sector_shift], Vcb->superblock.sectorsize);

                    context->stripes[bad_stripe2].rewrite = true;

                    log_error(Vcb, addr, c->devices[bad_stripe2]->devitem.devid, false, true, false);
                } else
                    log_error(Vcb, addr, c->devices[bad_stripe2]->devitem.devid, false, false, false);
            }
        } else {
            stripe = (parity2 + 1) % c->chunk_item->num_stripes;
            off = (ULONG)((bit_start + num - stripe_start) * sectors_per_stripe * (c->chunk_item->num_stripes - 2)) + i;

            while (stripe != parity1) {
                if (c->devices[stripe]->devobj && RtlCheckBit(&context->alloc, off)) {
                    if (RtlCheckBit(&context->stripes[stripe].error, i)) {
                        uint64_t addr = c->offset + (stripe_start * (c->chunk_item->num_stripes - 2) * c->chunk_item->stripe_len) + (off << Vcb->sector_shift);

                        log_error(Vcb, addr, c->devices[stripe]->devitem.devid, RtlCheckBit(&context->is_tree, off), false, false);
                    }
                }

                off += sectors_per_stripe;
                stripe = (stripe + 1) % c->chunk_item->num_stripes;
            }
        }
    }

    return STATUS_SUCCESS;
}
