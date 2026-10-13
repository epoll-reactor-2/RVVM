#include "devices/gpu-xe2.c"
#include "utils.h"
#include <stdint.h>

void brw_read_disk(const char* path, uint32_t** out, uint32_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        rvvm_fatal("fopen(): %s", strerror(errno));
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    *out = safe_calloc(1, size);
    fread(*out, 1, size, f);
    fclose(f);
    *n = size;

    rvvm_info("Read %s: %u bytes", path, *n);
}

void brw_read_qwords(xe2_spirv_ctx_t* ctx, uint32_t* brw, uint32_t size)
{
    uint32_t len   = 0U;
    uint32_t zeros = 0U;


    for (uint32_t i = 0; i < size / 4;) {
        xe2_qword_t qw = {0};
        memcpy(&qw, brw + i, sizeof(qw));
        bool eot = 0;
        if (qw.hi == 0U) {
            if (++zeros >= 4 && len > 0) {
                break;
            }
        } else {
            zeros = 0;
            len   = i + 1;
        }
        i += xe2_brw_decode_one(ctx, &qw, &eot) / 4;
        if (eot) {
            break;
        }
    }
}

int main(void)
{
    rvvm_set_loglevel(LOG_INFO);
    uint32_t* brw      = NULL;
    uint32_t  brw_size = 0;
    brw_read_disk("decompiled.xe2", &brw, &brw_size);

    xe2_spirv_ctx_t ctx = {0};
    brw_read_qwords(&ctx, brw, brw_size);
    free(brw);
}
