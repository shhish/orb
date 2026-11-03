#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Command receipts cannot use the console's best-effort idle logging path:
 * the no-driver VFS silently drops bytes after 50 ms of host backpressure.
 * The caller serializes stdout and supplies a bounded monotonic deadline. */
typedef struct {
    void *ctx;
    size_t (*write)(void *ctx, const uint8_t *data, size_t size);
    bool (*writable)(void *ctx);
    void (*flush)(void *ctx);
    int64_t (*now_us)(void *ctx);
    void (*yield)(void *ctx);
} orb_console_reply_io_t;

static inline bool orb_console_reply_write(const orb_console_reply_io_t *io,
                                           const uint8_t *data, size_t size,
                                           int64_t deadline_us)
{
    while (size) {
        if (io->now_us(io->ctx) >= deadline_us) return false;
        size_t sent = io->write(io->ctx, data, size);
        if (sent > size) return false;
        data += sent;
        size -= sent;
        if (!sent) {
            io->flush(io->ctx);
            io->yield(io->ctx);
        }
    }
    io->flush(io->ctx);
    while (!io->writable(io->ctx)) {
        if (io->now_us(io->ctx) >= deadline_us) return false;
        io->yield(io->ctx);
    }
    /* An exact 64-byte packet auto-flushes. Once it drains, a second flush
     * supplies the USB short/ZLP terminator; otherwise CDC may hold the ACK. */
    io->flush(io->ctx);
    return true;
}
