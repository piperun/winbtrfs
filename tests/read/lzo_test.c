/* Boundary validation of the guarded production LZO segment parser. */
#include "host.h"
#include "lzo_functions.inc"

static unsigned cases, failures;

static void check(const char* name, uint8_t* input, uint32_t input_len,
                  uint32_t output_len, uint32_t page_offset, NTSTATUS expected) {
    uint8_t output[4104];
    memset(output, 0xa5, sizeof(output));
    NTSTATUS status = lzo_decompress(input, input_len, output, output_len, page_offset);
    bool pass = status == expected;
    if (NT_SUCCESS(status)) {
        for (unsigned i = 0; i < output_len; i++) {
            uint8_t want = i < 4 ? (uint8_t)('a' + i) :
                           i < 4096 ? 0 : (uint8_t)('e' + i - 4096);
            pass = pass && output[i] == want;
        }
    }
    for (unsigned i = output_len; i < sizeof(output); i++)
        pass = pass && output[i] == 0xa5;
    cases++;
    if (!pass) {
        failures++;
        printf("FAIL %s status=%08x expected=%08x\n", name,
               (unsigned)status, (unsigned)expected);
    }
}

int main(void) {
    uint8_t valid[] = {8, 0, 0, 0, 21, 'a', 'b', 'c', 'd', 17, 0, 0};
    check("literal segment", valid, sizeof(valid), 4, 0, STATUS_SUCCESS);
    check("partial output", valid, sizeof(valid), 2, 0, STATUS_SUCCESS);

    uint8_t unaligned[sizeof(valid) + 1];
    memcpy(unaligned + 1, valid, sizeof(valid));
    check("unaligned segment header", unaligned + 1, sizeof(valid), 4, 0, STATUS_SUCCESS);

    /* These cases run only against the guarded parser, never a historical decoder. */
    for (uint32_t length = 0; length < 4; length++)
        check("incomplete segment header", valid, length, 4, 0, STATUS_INTERNAL_ERROR);
    check("incomplete segment payload", valid, sizeof(valid) - 1, 4, 0, STATUS_INTERNAL_ERROR);

    uint8_t pages[27] = {0};
    const uint8_t second[] = {8, 0, 0, 0, 21, 'e', 'f', 'g', 'h', 17, 0, 0};
    for (unsigned pad = 0; pad <= 3; pad++) {
        memset(pages, 0, sizeof(pages));
        memcpy(pages, valid, sizeof(valid));
        memcpy(pages + sizeof(valid) + pad, second, sizeof(second));
        uint32_t offset = pad ? 4096 - sizeof(valid) - pad : 0;
        check("two segments with page padding", pages, sizeof(valid) + sizeof(second) + pad,
              4100, offset, STATUS_SUCCESS);
    }
    memcpy(pages, valid, sizeof(valid));
    for (unsigned remainder = 1; remainder < 4; remainder++)
        check("incomplete next segment header", pages, sizeof(valid) + remainder,
              4100, 0, STATUS_INTERNAL_ERROR);

    printf("%u/%u LZO parser scenarios passed\n", cases - failures, cases);
    return failures != 0;
}
