/* Write the update and add reference calls rationalize_extents makes on the VM
 * layout of compare_test.c, for replaying against the real reference code. */
#define main compare_test_main
#include "compare_test.c"
#undef main

int main(int argc, char** argv) {
    static run r;
    layout l;
    CHECK(argc == 3);
    unsigned count = (unsigned)atoi(argv[1]);
    make_vm_layout(&l, count, gib, count);
    struct timespec t0, t1;
    start(&r, &l);
    current = &r;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    r.status = candidate_rationalize_extents(&r.file, &request);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(r.status == STATUS_SUCCESS && r.live == 0);
    FILE* f = fopen(argv[2], "w");
    CHECK(f != NULL);
    // Timing is not fixture data: canonicalize this unused header field.
    fprintf(f, "%u 0.000\n", l.num_chunks);
    for (size_t i = 0; i < r.num_calls; i++) {
        const call* c = &r.calls[i];
        if (c->op == UPDATE || c->op == ADD)
            fprintf(f, "%c %d %llu %llu %llu %llu %llu %lld\n", c->op == UPDATE ? 'U' : 'A', c->chunk,
                    (unsigned long long)c->address, (unsigned long long)c->size,
                    (unsigned long long)c->root, (unsigned long long)c->inode,
                    (unsigned long long)c->offset, (long long)c->count);
    }
    fclose(f);
    return 0;
}
