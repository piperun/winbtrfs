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

#include "scrub.h"

NTSTATUS scrub_extent_raid10(device_extension* Vcb, chunk* c, uint64_t offset, uint32_t length, uint16_t startoffstripe, void* csum, scrub_context* context) {
    ULONG j;
    uint16_t stripe, sub_stripes = max(c->chunk_item->sub_stripes, 1);
    uint32_t pos, *stripeoff;
    bool csum_error = false;
    NTSTATUS Status;

    pos = 0;
    stripeoff = ExAllocatePoolWithTag(NonPagedPool, sizeof(uint32_t) * c->chunk_item->num_stripes / sub_stripes, ALLOC_TAG);
    if (!stripeoff) {
        ERR("out of memory\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(stripeoff, sizeof(uint32_t) * c->chunk_item->num_stripes / sub_stripes);

    stripe = startoffstripe;
    while (pos < length) {
        uint32_t readlen;

        if (pos == 0)
            readlen = (uint32_t)min(context->stripes[stripe * sub_stripes].length,
                                  c->chunk_item->stripe_len - (context->stripes[stripe * sub_stripes].start % c->chunk_item->stripe_len));
        else
            readlen = min(length - pos, (uint32_t)c->chunk_item->stripe_len);

        if (csum) {
            ULONG good_stripe = 0xffffffff;
            uint16_t k;

            for (k = 0; k < sub_stripes; k++) {
                if (c->devices[(stripe * sub_stripes) + k]->devobj) {
                    // if first stripe is okay, we only need to check that the others are identical to it
                    if (good_stripe != 0xffffffff) {
                        if (RtlCompareMemory(context->stripes[(stripe * sub_stripes) + k].buf + stripeoff[stripe],
                                            context->stripes[(stripe * sub_stripes) + good_stripe].buf + stripeoff[stripe],
                                            readlen) != readlen) {
                            context->stripes[(stripe * sub_stripes) + k].csum_error = true;
                            csum_error = true;
                            log_device_error(Vcb, c->devices[(stripe * sub_stripes) + k], BTRFS_DEV_STAT_CORRUPTION_ERRS);
                        }
                    } else {
                        for (j = 0; j < readlen; j += Vcb->superblock.sectorsize) {
                            if (!check_sector_csum(Vcb, context->stripes[(stripe * sub_stripes) + k].buf + stripeoff[stripe] + j,
                                                   (uint8_t*)csum + (((pos + j) * Vcb->csum_size) >> Vcb->sector_shift))) {
                                csum_error = true;
                                context->stripes[(stripe * sub_stripes) + k].csum_error = true;
                                log_device_error(Vcb, c->devices[(stripe * sub_stripes) + k], BTRFS_DEV_STAT_CORRUPTION_ERRS);
                                break;
                            }
                        }

                        if (!context->stripes[(stripe * sub_stripes) + k].csum_error)
                            good_stripe = k;
                    }
                }
            }

            pos += readlen;
            stripeoff[stripe] += readlen;
        } else {
            ULONG good_stripe = 0xffffffff;
            uint16_t k;

            for (k = 0; k < sub_stripes; k++) {
                if (c->devices[(stripe * sub_stripes) + k]->devobj) {
                    // if first stripe is okay, we only need to check that the others are identical to it
                    if (good_stripe != 0xffffffff) {
                        if (RtlCompareMemory(context->stripes[(stripe * sub_stripes) + k].buf + stripeoff[stripe],
                                            context->stripes[(stripe * sub_stripes) + good_stripe].buf + stripeoff[stripe],
                                            readlen) != readlen) {
                            context->stripes[(stripe * sub_stripes) + k].csum_error = true;
                            csum_error = true;
                            log_device_error(Vcb, c->devices[(stripe * sub_stripes) + k], BTRFS_DEV_STAT_CORRUPTION_ERRS);
                        }
                    } else {
                        for (j = 0; j < readlen; j += Vcb->superblock.nodesize) {
                            struct btrfs_header* th = (struct btrfs_header*)(context->stripes[(stripe * sub_stripes) + k].buf + stripeoff[stripe] + j);
                            uint64_t addr = offset + pos + j;

                            if (!check_tree_checksum(Vcb, th) || th->bytenr != addr) {
                                csum_error = true;
                                context->stripes[(stripe * sub_stripes) + k].csum_error = true;
                                log_device_error(Vcb, c->devices[(stripe * sub_stripes) + k], BTRFS_DEV_STAT_CORRUPTION_ERRS);
                                break;
                            }
                        }

                        if (!context->stripes[(stripe * sub_stripes) + k].csum_error)
                            good_stripe = k;
                    }
                }
            }

            pos += readlen;
            stripeoff[stripe] += readlen;
        }

        stripe = (stripe + 1) % (c->chunk_item->num_stripes / sub_stripes);
    }

    if (!csum_error) {
        Status = STATUS_SUCCESS;
        goto end;
    }

    for (j = 0; j < c->chunk_item->num_stripes; j += sub_stripes) {
        ULONG goodstripe = 0xffffffff;
        uint16_t k;
        bool hasbadstripe = false;

        if (context->stripes[j].length == 0)
            continue;

        for (k = 0; k < sub_stripes; k++) {
            if (c->devices[j + k]->devobj) {
                if (!context->stripes[j + k].csum_error)
                    goodstripe = k;
                else
                    hasbadstripe = true;
            }
        }

        if (hasbadstripe) {
            if (goodstripe != 0xffffffff) {
                for (k = 0; k < sub_stripes; k++) {
                    if (c->devices[j + k]->devobj && context->stripes[j + k].csum_error) {
                        uint32_t so = 0;
                        bool recovered = false;

                        pos = 0;

                        stripe = startoffstripe;
                        while (pos < length) {
                            uint32_t readlen;

                            if (pos == 0)
                                readlen = (uint32_t)min(context->stripes[stripe * sub_stripes].length,
                                              c->chunk_item->stripe_len - (context->stripes[stripe * sub_stripes].start % c->chunk_item->stripe_len));
                            else
                                readlen = min(length - pos, (uint32_t)c->chunk_item->stripe_len);

                            if (stripe == j / sub_stripes) {
                                if (csum) {
                                    ULONG l;

                                    for (l = 0; l < readlen; l += Vcb->superblock.sectorsize) {
                                        if (RtlCompareMemory(context->stripes[j + k].buf + so,
                                                             context->stripes[j + goodstripe].buf + so,
                                                             Vcb->superblock.sectorsize) != Vcb->superblock.sectorsize) {
                                            uint64_t addr = offset + pos;

                                            log_error(Vcb, addr, c->devices[j + k]->devitem.devid, false, true, false);

                                            recovered = true;
                                        }

                                        pos += Vcb->superblock.sectorsize;
                                        so += Vcb->superblock.sectorsize;
                                    }
                                } else {
                                    ULONG l;

                                    for (l = 0; l < readlen; l += Vcb->superblock.nodesize) {
                                        if (RtlCompareMemory(context->stripes[j + k].buf + so,
                                                            context->stripes[j + goodstripe].buf + so,
                                                            Vcb->superblock.nodesize) != Vcb->superblock.nodesize) {
                                            uint64_t addr = offset + pos;

                                            log_error(Vcb, addr, c->devices[j + k]->devitem.devid, true, true, false);

                                            recovered = true;
                                        }

                                        pos += Vcb->superblock.nodesize;
                                        so += Vcb->superblock.nodesize;
                                    }
                                }
                            } else
                                pos += readlen;

                            stripe = (stripe + 1) % (c->chunk_item->num_stripes / sub_stripes);
                        }

                        if (recovered) {
                            // write good data over bad

                            if (!c->devices[j + k]->readonly) {
                                Status = write_data_phys(c->devices[j + k]->devobj, c->devices[j + k]->fileobj, c->chunk_item->stripe[j + k].offset + context->stripes[j + k].start,
                                                         context->stripes[j + goodstripe].buf, context->stripes[j + goodstripe].length);

                                if (!NT_SUCCESS(Status)) {
                                    ERR("write_data_phys returned %08lx\n", Status);
                                    log_device_error(Vcb, c->devices[j + k], BTRFS_DEV_STAT_WRITE_ERRS);
                                    goto end;
                                }
                            }
                        }
                    }
                }
            } else {
                uint32_t so = 0;
                bool recovered = false;

                if (csum) {
                    for (k = 0; k < sub_stripes; k++) {
                        if (c->devices[j + k]->devobj) {
                            context->stripes[j + k].bad_csums = ExAllocatePoolWithTag(PagedPool, (context->stripes[j + k].length * Vcb->csum_size) >> Vcb->sector_shift,
                                                                                      ALLOC_TAG);
                            if (!context->stripes[j + k].bad_csums) {
                                ERR("out of memory\n");
                                Status = STATUS_INSUFFICIENT_RESOURCES;
                                goto end;
                            }

                            do_calc_job(Vcb, context->stripes[j + k].buf, context->stripes[j + k].length >> Vcb->sector_shift, context->stripes[j + k].bad_csums);
                        }
                    }
                } else {
                    for (k = 0; k < sub_stripes; k++) {
                        if (c->devices[j + k]->devobj) {
                            ULONG l;

                            context->stripes[j + k].bad_csums = ExAllocatePoolWithTag(PagedPool, context->stripes[j + k].length * Vcb->csum_size / Vcb->superblock.nodesize,
                                                                                      ALLOC_TAG);
                            if (!context->stripes[j + k].bad_csums) {
                                ERR("out of memory\n");
                                Status = STATUS_INSUFFICIENT_RESOURCES;
                                goto end;
                            }

                            for (l = 0; l < context->stripes[j + k].length / Vcb->superblock.nodesize; l++) {
                                struct btrfs_header* th = (struct btrfs_header*)&context->stripes[j + k].buf[l * Vcb->superblock.nodesize];

                                get_tree_checksum(Vcb, th, (uint8_t*)context->stripes[j + k].bad_csums + (Vcb->csum_size * l));
                            }
                        }
                    }
                }

                pos = 0;

                stripe = startoffstripe;
                while (pos < length) {
                    uint32_t readlen;

                    if (pos == 0)
                        readlen = (uint32_t)min(context->stripes[stripe * sub_stripes].length,
                                      c->chunk_item->stripe_len - (context->stripes[stripe * sub_stripes].start % c->chunk_item->stripe_len));
                    else
                        readlen = min(length - pos, (uint32_t)c->chunk_item->stripe_len);

                    if (stripe == j / sub_stripes) {
                        ULONG l;

                        if (csum) {
                            for (l = 0; l < readlen; l += Vcb->superblock.sectorsize) {
                                bool has_error = false;

                                goodstripe = 0xffffffff;
                                for (k = 0; k < sub_stripes; k++) {
                                    if (c->devices[j + k]->devobj) {
                                        if (RtlCompareMemory((uint8_t*)context->stripes[j + k].bad_csums + ((so * Vcb->csum_size) >> Vcb->sector_shift),
                                            (uint8_t*)csum + ((pos * Vcb->csum_size) >> Vcb->sector_shift),
                                            Vcb->csum_size) != Vcb->csum_size) {
                                            has_error = true;
                                        } else
                                            goodstripe = k;
                                    }
                                }

                                if (has_error) {
                                    if (goodstripe != 0xffffffff) {
                                        for (k = 0; k < sub_stripes; k++) {
                                            if (c->devices[j + k]->devobj &&
                                                RtlCompareMemory((uint8_t*)context->stripes[j + k].bad_csums + ((so * Vcb->csum_size) >> Vcb->sector_shift),
                                                                 (uint8_t*)csum + ((pos * Vcb->csum_size) >> Vcb->sector_shift),
                                                                 Vcb->csum_size) != Vcb->csum_size) {
                                                uint64_t addr = offset + pos;

                                                log_error(Vcb, addr, c->devices[j + k]->devitem.devid, false, true, false);

                                                recovered = true;

                                                RtlCopyMemory(context->stripes[j + k].buf + so, context->stripes[j + goodstripe].buf + so,
                                                              Vcb->superblock.sectorsize);
                                            }
                                        }
                                    } else {
                                        uint64_t addr = offset + pos;

                                        for (k = 0; k < sub_stripes; k++) {
                                            if (c->devices[j + k]->devobj) {
                                                log_error(Vcb, addr, c->devices[j + k]->devitem.devid, false, false, false);
                                                log_device_error(Vcb, c->devices[j + k], BTRFS_DEV_STAT_CORRUPTION_ERRS);
                                            }
                                        }
                                    }
                                }

                                pos += Vcb->superblock.sectorsize;
                                so += Vcb->superblock.sectorsize;
                            }
                        } else {
                            for (l = 0; l < readlen; l += Vcb->superblock.nodesize) {
                                for (k = 0; k < sub_stripes; k++) {
                                    if (c->devices[j + k]->devobj) {
                                        struct btrfs_header* th = (struct btrfs_header*)&context->stripes[j + k].buf[so];
                                        uint64_t addr = offset + pos;

                                        if (RtlCompareMemory((uint8_t*)context->stripes[j + k].bad_csums + (so * Vcb->csum_size / Vcb->superblock.nodesize), th, Vcb->csum_size) != Vcb->csum_size || th->bytenr != addr) {
                                            ULONG m;

                                            recovered = false;

                                            for (m = 0; m < sub_stripes; m++) {
                                                if (m != k) {
                                                    struct btrfs_header* th2 = (struct btrfs_header*)&context->stripes[j + m].buf[so];

                                                    if (RtlCompareMemory((uint8_t*)context->stripes[j + m].bad_csums + (so * Vcb->csum_size / Vcb->superblock.nodesize), th2, Vcb->csum_size) == Vcb->csum_size && th2->bytenr == addr) {
                                                        log_error(Vcb, addr, c->devices[j + k]->devitem.devid, true, true, false);

                                                        RtlCopyMemory(th, th2, Vcb->superblock.nodesize);

                                                        recovered = true;
                                                        break;
                                                    } else
                                                        log_device_error(Vcb, c->devices[j + m], BTRFS_DEV_STAT_CORRUPTION_ERRS);
                                                }
                                            }

                                            if (!recovered)
                                                log_error(Vcb, addr, c->devices[j + k]->devitem.devid, true, false, false);
                                        }
                                    }
                                }

                                pos += Vcb->superblock.nodesize;
                                so += Vcb->superblock.nodesize;
                            }
                        }
                    } else
                        pos += readlen;

                    stripe = (stripe + 1) % (c->chunk_item->num_stripes / sub_stripes);
                }

                if (recovered) {
                    // write good data over bad

                    for (k = 0; k < sub_stripes; k++) {
                        if (c->devices[j + k]->devobj && !c->devices[j + k]->readonly) {
                            Status = write_data_phys(c->devices[j + k]->devobj, c->devices[j + k]->fileobj, c->chunk_item->stripe[j + k].offset + context->stripes[j + k].start,
                                                     context->stripes[j + k].buf, context->stripes[j + k].length);

                            if (!NT_SUCCESS(Status)) {
                                ERR("write_data_phys returned %08lx\n", Status);
                                log_device_error(Vcb, c->devices[j + k], BTRFS_DEV_STAT_WRITE_ERRS);
                                goto end;
                            }
                        }
                    }
                }
            }
        }
    }

    Status = STATUS_SUCCESS;

end:
    ExFreePool(stripeoff);

    return Status;
}
