#include "orb_ota_assets.h"

#define SECTOR 4096u
#define MAX_IMAGE (8u * 1024u * 1024u)
#define STALL_US (30 * 1000000LL)

int orb_assets_tx_apply(const orb_assets_tx_port_t *p, size_t image_size,
                        size_t partition_size, void *buffer, size_t buffer_size)
{
    if (!p || !buffer || !buffer_size || image_size < 2 * SECTOR ||
        image_size > MAX_IMAGE || image_size > partition_size || image_size % SECTOR) {
        return ORB_ASSETS_TX_SIZE;
    }
    int result = p->begin(p->ctx);
    if (result) {
        return result;
    }
    result = p->erase(p->ctx, 0, image_size);
    size_t done = 0;
    int64_t last_progress = p->now(p->ctx);
    while (!result && done < image_size) {
        size_t size = image_size - done;
        if (size > buffer_size) {
            size = buffer_size;
        }
        int count = p->read(p->ctx, buffer, size);
        if (count < 0) {
            result = ORB_ASSETS_TX_READ;
        } else if ((size_t)count > size) {
            result = ORB_ASSETS_TX_SIZE;
        } else if (count > 0) {
            result = p->write(p->ctx, done, buffer, (size_t)count);
            if (!result) {
                done += (size_t)count;
                last_progress = p->now(p->ctx);
                p->progress(p->ctx, done, false);
            }
        } else if (p->complete(p->ctx)) {
            result = ORB_ASSETS_TX_SIZE;
        } else if (p->now(p->ctx) - last_progress >= STALL_US) {
            result = ORB_ASSETS_TX_STALLED;
        } else {
            p->wait(p->ctx);
        }
    }
    if (!result && !p->complete(p->ctx)) {
        result = ORB_ASSETS_TX_SIZE;
    }
    if (!result) {
        p->progress(p->ctx, done, true);
        result = p->check_sha(p->ctx);
    }
    if (!result) {
        result = p->verify(p->ctx);
    }
    if (result) {
        /* Even a complete, correctly signed image can fail the offer's digest
         * or version. A rejected image must not activate on a later font read. */
        p->erase(p->ctx, 0, SECTOR);
    }
    p->end(p->ctx);
    return result;
}
