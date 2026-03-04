#include <cstdio>
#include <cstring>

#include "mrdrums_params.h"

static int fail(const char *msg) {
    std::fprintf(stderr, "FAIL: %s\n", msg);
    return 1;
}

int main() {
    char key[32];
    if (!mrdrums_make_pad_key(1, "pan", key, sizeof(key))) return fail("pad1 key generation");
    if (std::strcmp(key, "p01_pan") != 0) return fail("expected p01_pan");

    if (!mrdrums_make_pad_key(16, "chance_pct", key, sizeof(key))) return fail("pad16 key generation");
    if (std::strcmp(key, "p16_chance_pct") != 0) return fail("expected p16_chance_pct");

    if (mrdrums_make_pad_key(0, "pan", key, sizeof(key))) return fail("pad 0 should be invalid");
    if (mrdrums_make_pad_key(17, "pan", key, sizeof(key))) return fail("pad 17 should be invalid");

    const mrdrums_param_desc_t *global = mrdrums_find_global_param("g_master_vol");
    if (!global) return fail("missing g_master_vol descriptor");

    const mrdrums_param_desc_t *pad = mrdrums_find_pad_param("p07_tune");
    if (!pad) return fail("missing p07_tune descriptor");
    if (pad->pad_index != 7) return fail("pad_index expected 7");
    if (std::strcmp(pad->suffix, "tune") != 0) return fail("suffix expected tune");

    std::printf("PASS: mrdrums pad key mapping\n");
    return 0;
}
