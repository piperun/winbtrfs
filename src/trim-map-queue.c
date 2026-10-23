/* Copyright (c) Mark Harmstone 2016-17
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

// RAID mapping for reserved asynchronous discard ranges.
#include "trim-state.h"

bool trim_map_chunk(device_extension* Vcb, chunk* c, trim_batch* batch) {
    LIST_ENTRY* le;
    ULONG type;

    if (c->chunk_item->type & BTRFS_BLOCK_GROUP_DUP)
        type = BTRFS_BLOCK_GROUP_DUP;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID0)
        type = BTRFS_BLOCK_GROUP_RAID0;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID1)
        type = BTRFS_BLOCK_GROUP_DUP;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID10)
        type = BTRFS_BLOCK_GROUP_RAID10;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID5)
        type = BTRFS_BLOCK_GROUP_RAID5;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID6)
        type = BTRFS_BLOCK_GROUP_RAID6;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID1C3)
        type = BTRFS_BLOCK_GROUP_DUP;
    else if (c->chunk_item->type & BTRFS_BLOCK_GROUP_RAID1C4)
        type = BTRFS_BLOCK_GROUP_DUP;
    else // SINGLE
        type = BTRFS_BLOCK_GROUP_DUP;

    le = c->deleting.Flink;
    while (le != &c->deleting) {
        space* s = CONTAINING_RECORD(le, space, list_entry);

        if (!Vcb->options.no_barrier || !(c->chunk_item->type & BTRFS_BLOCK_GROUP_METADATA)) {
            if (type == BTRFS_BLOCK_GROUP_DUP) {
                uint16_t i;

                for (i = 0; i < c->chunk_item->num_stripes; i++) {
                    if (c->devices[i] && c->devices[i]->devobj && !c->devices[i]->readonly && c->devices[i]->trim)
                        if (!trim_add_range(batch, c->devices[i], s->address - c->offset + c->chunk_item->stripe[i].offset, s->size))
                            return false;
                }
            } else if (type == BTRFS_BLOCK_GROUP_RAID0) {
                uint64_t startoff, endoff;
                uint16_t startoffstripe, endoffstripe, i;

                get_raid0_offset(s->address - c->offset, c->chunk_item->stripe_len, c->chunk_item->num_stripes, &startoff, &startoffstripe);
                get_raid0_offset(s->address - c->offset + s->size - 1, c->chunk_item->stripe_len, c->chunk_item->num_stripes, &endoff, &endoffstripe);

                for (i = 0; i < c->chunk_item->num_stripes; i++) {
                    if (c->devices[i] && c->devices[i]->devobj && !c->devices[i]->readonly && c->devices[i]->trim) {
                        uint64_t stripestart, stripeend;

                        if (startoffstripe > i)
                            stripestart = startoff - (startoff % c->chunk_item->stripe_len) + c->chunk_item->stripe_len;
                        else if (startoffstripe == i)
                            stripestart = startoff;
                        else
                            stripestart = startoff - (startoff % c->chunk_item->stripe_len);

                        if (endoffstripe > i)
                            stripeend = endoff - (endoff % c->chunk_item->stripe_len) + c->chunk_item->stripe_len;
                        else if (endoffstripe == i)
                            stripeend = endoff + 1;
                        else
                            stripeend = endoff - (endoff % c->chunk_item->stripe_len);

                        if (stripestart != stripeend)
                            if (!trim_add_range(batch, c->devices[i], stripestart + c->chunk_item->stripe[i].offset, stripeend - stripestart))
                                return false;
                    }
                }
            } else if (type == BTRFS_BLOCK_GROUP_RAID10) {
                uint64_t startoff, endoff;
                uint16_t sub_stripes, startoffstripe, endoffstripe, i;

                sub_stripes = max(1, c->chunk_item->sub_stripes);

                get_raid0_offset(s->address - c->offset, c->chunk_item->stripe_len, c->chunk_item->num_stripes / sub_stripes, &startoff, &startoffstripe);
                get_raid0_offset(s->address - c->offset + s->size - 1, c->chunk_item->stripe_len, c->chunk_item->num_stripes / sub_stripes, &endoff, &endoffstripe);

                startoffstripe *= sub_stripes;
                endoffstripe *= sub_stripes;

                for (i = 0; i < c->chunk_item->num_stripes; i += sub_stripes) {
                    ULONG j;
                    uint64_t stripestart, stripeend;

                    if (startoffstripe > i)
                        stripestart = startoff - (startoff % c->chunk_item->stripe_len) + c->chunk_item->stripe_len;
                    else if (startoffstripe == i)
                        stripestart = startoff;
                    else
                        stripestart = startoff - (startoff % c->chunk_item->stripe_len);

                    if (endoffstripe > i)
                        stripeend = endoff - (endoff % c->chunk_item->stripe_len) + c->chunk_item->stripe_len;
                    else if (endoffstripe == i)
                        stripeend = endoff + 1;
                    else
                        stripeend = endoff - (endoff % c->chunk_item->stripe_len);

                    if (stripestart != stripeend) {
                        for (j = 0; j < sub_stripes; j++) {
                            if (c->devices[i+j] && c->devices[i+j]->devobj && !c->devices[i+j]->readonly && c->devices[i+j]->trim)
                                if (!trim_add_range(batch, c->devices[i+j], stripestart + c->chunk_item->stripe[i+j].offset, stripeend - stripestart))
                                    return false;
                        }
                    }
                }
            }
            // FIXME - RAID5(?), RAID6(?)
        }

        le = le->Flink;
    }
    return true;
}
