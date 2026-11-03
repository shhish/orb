#pragma once

/* Install the process-wide cJSON allocator once, at app_main entry before
 * starting any task that creates JSON. Never change hooks while trees live. */
void orb_link_memory_init(void);

/* A 4 KiB link envelope uses a fixed startup-allocated PSRAM arena. Only the
 * link owner may bracket parsing; callback/build JSON keeps normal ownership.
 * End disables arena allocations; its tree remains valid until the next begin.
 * Exhaustion fails the parse without falling back to a message allocation. */
#include <stdbool.h>
#include <stddef.h>
#define ORB_LINK_PARSE_ARENA_BYTES (192u * 1024u)
bool orb_link_memory_begin_parse(void);
void orb_link_memory_end_parse(void);
size_t orb_link_memory_parse_used(void);

/* Fixed receive slots outlive WebSocket callbacks until the link drains them.
 * Extension frames fit 16 x 4 KiB slots; two 16 KiB slots preserve legacy/Home
 * oversize classification. Saturation fails closed, never allocates or drops. */
#define ORB_LINK_RX_SMALL_SLOTS 16u
#define ORB_LINK_RX_LARGE_SLOTS 2u
#define ORB_LINK_RX_SMALL_BYTES 4097u
#define ORB_LINK_RX_LARGE_BYTES 16385u
#define ORB_LINK_RX_POOL_BYTES (ORB_LINK_RX_SMALL_SLOTS * ORB_LINK_RX_SMALL_BYTES + \
                                ORB_LINK_RX_LARGE_SLOTS * ORB_LINK_RX_LARGE_BYTES)
char *orb_link_memory_rx_resize(char *pointer, size_t bytes);
void orb_link_memory_rx_free(char *pointer);
