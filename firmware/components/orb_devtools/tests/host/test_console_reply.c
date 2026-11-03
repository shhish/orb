#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "orb_console_reply.h"

typedef struct {
    int64_t now, unavailable_until;
    uint8_t fifo[64], wire[4096];
    size_t used, wire_size;
    unsigned yields, packets, terminators;
    bool pending, stopped;
} fake_usb_t;

static bool writable(void *ctx)
{
    fake_usb_t *usb = ctx;
    if (usb->stopped || usb->now < usb->unavailable_until) return false;
    if (usb->pending) {
        memcpy(usb->wire + usb->wire_size, usb->fifo, usb->used);
        usb->wire_size += usb->used;
        usb->used = 0;
        usb->pending = false;
    }
    return true;
}

static size_t write_bytes(void *ctx, const uint8_t *data, size_t size)
{
    fake_usb_t *usb = ctx;
    if (!writable(ctx)) return 0;
    size_t sent = 64 - usb->used;
    if (sent > size) sent = size;
    memcpy(usb->fifo + usb->used, data, sent);
    usb->used += sent;
    if (usb->used == 64) {
        usb->pending = true;
        usb->unavailable_until = usb->now + 1000;
        usb->packets++;
    }
    return sent;
}

static void flush(void *ctx)
{
    fake_usb_t *usb = ctx;
    if (!writable(ctx)) return;
    if (usb->used) {
        usb->pending = true;
        usb->unavailable_until = usb->now + 1000;
        usb->packets++;
    } else {
        usb->terminators++;
    }
}

static int64_t now_us(void *ctx) { return ((fake_usb_t *)ctx)->now; }
static void yield(void *ctx)
{
    fake_usb_t *usb = ctx;
    usb->now += 1000;
    usb->yields++;
}

int main(void)
{
    const size_t sizes[] = {1, 63, 64, 65, 128, 1536, 2048};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        fake_usb_t usb = {.unavailable_until = 80000};
        orb_console_reply_io_t io = {
            .ctx = &usb, .write = write_bytes, .writable = writable,
            .flush = flush, .now_us = now_us, .yield = yield,
        };
        uint8_t receipt[2048];
        for (size_t n = 0; n < sizes[i]; n++) receipt[n] = (uint8_t)(n * 17);
        assert(orb_console_reply_write(&io, receipt, sizes[i], 2000000));
        assert(usb.now >= 80000 && usb.yields >= 80);
        assert(usb.wire_size == sizes[i]);
        assert(!memcmp(usb.wire, receipt, sizes[i]));
        assert(usb.terminators); /* Includes exact 64-byte packet endings. */
        assert(!usb.pending && !usb.used);
    }
    fake_usb_t stopped = {.stopped = true};
    orb_console_reply_io_t io = {
        .ctx = &stopped, .write = write_bytes, .writable = writable,
        .flush = flush, .now_us = now_us, .yield = yield,
    };
    assert(!orb_console_reply_write(&io, (const uint8_t *)"ACK\n", 4, 2000000));
    assert(stopped.now == 2000000 && stopped.wire_size == 0);
    puts("console reply: 44 checks passed");
    return 0;
}
