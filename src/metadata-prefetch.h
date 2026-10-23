/* Optional checksum readahead under the background flush's exclusive tree lock. */
#pragma once

void preload_checksum_trees(device_extension* Vcb);
