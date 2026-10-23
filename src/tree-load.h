/* Owner-side publication of an optional, already validated leaf read. */
#pragma once

bool publish_prefetched_tree(device_extension* Vcb, tree_holder* holder, root* r,
                             tree* parent, tree_data* item, uint64_t address,
                             uint64_t generation, uint8_t* buf);
