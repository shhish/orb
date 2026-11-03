#include "orb_mem.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned s_checks, s_heap_reads;
#define CHECK(test) do { \
    ++s_checks; \
    if (!(test)) { \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); \
        exit(1); \
    } \
} while (0)

void heap_caps_get_info(multi_heap_info_t *info, uint32_t caps)
{
    CHECK(caps == MALLOC_CAP_INTERNAL);
    ++s_heap_reads;
    info->minimum_free_bytes = 12345;
}

int main(void)
{
    /* No monitor, mutex or callback symbols are linked in this configuration. */
    orb_mem_init();
    orb_mem_heap_regions_begin();
    orb_mem_heap_regions_end();
    orb_mem_begin(ORB_MEM_LINK_CONNECT);
    orb_mem_end_as(ORB_MEM_LINK_CONNECT, ORB_MEM_TLS_HANDSHAKE);
    orb_mem_end(ORB_MEM_HELLO);
    orb_mem_report();
    multi_heap_info_t info;
    orb_mem_heap_info(&info, MALLOC_CAP_INTERNAL);
    CHECK(info.minimum_free_bytes == 12345);
    CHECK(s_heap_reads == 1);
    printf("orb_mem disabled: %u checks passed\n", s_checks);
    return 0;
}
