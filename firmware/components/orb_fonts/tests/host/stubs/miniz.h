#pragma once

/* Exercise real zlib framing, streaming and Adler validation on the host.
 * IDF's ROM tinfl owns no allocations; this adapter releases zlib's private
 * state when the fake allocator frees the equivalent 10,992-byte workspace. */
#include <stdbool.h>
#include <string.h>
#include <zlib.h>

typedef enum {
    TINFL_STATUS_FAILED = -1,
    TINFL_STATUS_DONE,
    TINFL_STATUS_NEEDS_MORE_INPUT,
    TINFL_STATUS_HAS_MORE_OUTPUT,
} tinfl_status;

enum {
    TINFL_FLAG_PARSE_ZLIB_HEADER = 1,
    TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 2,
    TINFL_FLAG_HAS_MORE_INPUT = 4,
};

typedef union {
    struct { z_stream stream; bool active; } host;
    unsigned char workspace[10992];
} tinfl_decompressor;

static inline void tinfl_init(tinfl_decompressor *d) { memset(d, 0, sizeof(*d)); }

static inline void tinfl_test_cleanup(tinfl_decompressor *d)
{
    if (d->host.active) inflateEnd(&d->host.stream);
    d->host.active = false;
}

static inline tinfl_status tinfl_decompress(tinfl_decompressor *d, const void *input,
    size_t *input_size, void *output_start, void *output, size_t *output_size, unsigned flags)
{
    (void)output_start;
    if (!d->host.active) {
        if (inflateInit(&d->host.stream) != Z_OK) return TINFL_STATUS_FAILED;
        d->host.active = true;
    }
    z_stream *stream = &d->host.stream;
    stream->next_in = (Bytef *)input;
    stream->avail_in = (uInt)*input_size;
    stream->next_out = output;
    stream->avail_out = (uInt)*output_size;
    int result = inflate(stream, Z_NO_FLUSH);
    *input_size -= stream->avail_in;
    *output_size -= stream->avail_out;
    if (result == Z_STREAM_END) {
        tinfl_test_cleanup(d);
        return TINFL_STATUS_DONE;
    }
    if ((result == Z_OK || result == Z_BUF_ERROR) && !stream->avail_in &&
        (flags & TINFL_FLAG_HAS_MORE_INPUT)) return TINFL_STATUS_NEEDS_MORE_INPUT;
    tinfl_status status = !stream->avail_out ? TINFL_STATUS_HAS_MORE_OUTPUT : TINFL_STATUS_FAILED;
    tinfl_test_cleanup(d);
    return status;
}
