#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The actual single-slot transaction, kept free of IDF so flash/network faults
 * can be exercised on the host. Callback errors pass through unchanged. */
enum {
    ORB_ASSETS_TX_SIZE = -1,
    ORB_ASSETS_TX_READ = -2,
    ORB_ASSETS_TX_STALLED = -3,
};

typedef struct {
    void *ctx;
    int (*begin)(void *ctx);
    int (*erase)(void *ctx, size_t offset, size_t size);
    int (*write)(void *ctx, size_t offset, const void *data, size_t size);
    int (*read)(void *ctx, void *data, size_t size); /* count, zero to retry, negative on failure */
    bool (*complete)(void *ctx);
    int (*check_sha)(void *ctx);
    int (*verify)(void *ctx); /* signature + signed header + offered version */
    void (*progress)(void *ctx, size_t done, bool verifying);
    int64_t (*now)(void *ctx);
    void (*wait)(void *ctx);
    void (*end)(void *ctx);
} orb_assets_tx_port_t;

/* A successful begin always has exactly one end. Any later failure invalidates
 * sector zero before reads resume. No app boot/otadata operation exists here. */
int orb_assets_tx_apply(const orb_assets_tx_port_t *port, size_t image_size,
                        size_t partition_size, void *buffer, size_t buffer_size);
