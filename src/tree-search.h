/* Private ownership of optional, pre-publication internal-node search arrays. */
#pragma once

void initialize_tree_search(tree* t);
void cache_tree_search(tree* t);
void invalidate_tree_search(tree* t);
