/* Exercises the production transaction with flash/network/crypto fault ports. */
#include <stdio.h>
#include <string.h>

#include "orb_ota_assets.h"

static unsigned s_checks;
#define CHECK(value) do { s_checks++; if (!(value)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value); return 1; } } while (0)

typedef enum {
    GOOD, BEGIN_FAIL, ERASE_FAIL, READ_FAIL, WRITE_FAIL, STALL,
    SHORT_BODY, NOT_COMPLETE, SHA_FAIL, SIGNATURE_FAIL, VERSION_FAIL, OVERRUN,
} fault_t;

typedef struct {
    fault_t fault;
    unsigned begins, ends, erases, writes, reads, sha, verifies, verifying, progress;
    size_t read, written;
    int64_t time;
    bool first_sector_valid;
} fake_t;

static int begin(void *ctx)
{
    fake_t *f = ctx;
    f->begins++;
    return f->fault == BEGIN_FAIL ? 101 : 0;
}

static int erase(void *ctx, size_t offset, size_t size)
{
    fake_t *f = ctx;
    f->erases++;
    if (offset != 0 || (size != 8192 && size != 4096)) return 999;
    if (f->fault == ERASE_FAIL && f->erases == 1) return 102;
    f->first_sector_valid = false;
    return 0;
}

static int write_data(void *ctx, size_t offset, const void *data, size_t size)
{
    fake_t *f = ctx;
    f->writes++;
    if (f->fault == WRITE_FAIL && f->writes == 2) return 103;
    if (offset != f->written || ((const unsigned char *)data)[0] != 0xA5) return 999;
    f->written += size;
    f->first_sector_valid = true;
    return 0;
}

static int read_data(void *ctx, void *data, size_t size)
{
    fake_t *f = ctx;
    f->reads++;
    if (f->fault == READ_FAIL && f->reads == 2) return -1;
    if (f->fault == OVERRUN) return (int)size + 1;
    if (f->fault == STALL || (f->fault == SHORT_BODY && f->read >= 4096)) return 0;
    /* Short network reads are legal, and every byte must be written once. */
    if (size > 1024) size = 1024;
    memset(data, 0xA5, size);
    f->read += size;
    return (int)size;
}

static bool complete(void *ctx)
{
    fake_t *f = ctx;
    return f->fault != NOT_COMPLETE && (f->read == 8192 || (f->fault == SHORT_BODY && f->read == 4096));
}

static int check_sha(void *ctx)
{
    fake_t *f = ctx;
    f->sha++;
    return f->fault == SHA_FAIL ? 104 : 0;
}

static int verify(void *ctx)
{
    fake_t *f = ctx;
    f->verifies++;
    return f->fault == SIGNATURE_FAIL ? 105 : f->fault == VERSION_FAIL ? 106 : 0;
}

static void progress(void *ctx, size_t done, bool verifying)
{
    fake_t *f = ctx;
    if (verifying) f->verifying++;
    else f->progress++;
    (void)done;
}

static int64_t now(void *ctx)
{
    return ((fake_t *)ctx)->time;
}

static void wait_for_data(void *ctx)
{
    ((fake_t *)ctx)->time += 1000000;
}

static void end(void *ctx)
{
    ((fake_t *)ctx)->ends++;
}

static orb_assets_tx_port_t port(fake_t *f)
{
    return (orb_assets_tx_port_t){
        .ctx = f, .begin = begin, .erase = erase, .write = write_data,
        .read = read_data, .complete = complete, .check_sha = check_sha,
        .verify = verify, .progress = progress, .now = now, .wait = wait_for_data, .end = end,
    };
}

int main(void)
{
    unsigned char buffer[4096];
    fake_t f = { 0 };
    orb_assets_tx_port_t p = port(&f);
    CHECK(orb_assets_tx_apply(&p, 8192, 8 * 1024 * 1024, buffer, sizeof(buffer)) == 0);
    CHECK(f.begins == 1 && f.ends == 1 && f.erases == 1);
    CHECK(f.written == 8192 && f.read == 8192 && f.writes == 8);
    CHECK(f.sha == 1 && f.verifies == 1 && f.verifying == 1 && f.progress == 8);
    CHECK(f.first_sector_valid);

    const fault_t faults[] = {BEGIN_FAIL, ERASE_FAIL, READ_FAIL, WRITE_FAIL, STALL,
                             SHORT_BODY, NOT_COMPLETE, SHA_FAIL, SIGNATURE_FAIL, VERSION_FAIL, OVERRUN};
    const int expected[] = {101, 102, ORB_ASSETS_TX_READ, 103, ORB_ASSETS_TX_STALLED,
                           ORB_ASSETS_TX_SIZE, ORB_ASSETS_TX_SIZE, 104, 105, 106, ORB_ASSETS_TX_SIZE};
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
        f = (fake_t){ .fault = faults[i], .first_sector_valid = true };
        CHECK(orb_assets_tx_apply(&p, 8192, 8 * 1024 * 1024, buffer, sizeof(buffer)) == expected[i]);
        CHECK(f.begins == 1);
        CHECK(f.ends == (faults[i] == BEGIN_FAIL ? 0u : 1u));
        CHECK(f.erases == (faults[i] == BEGIN_FAIL ? 0u : 2u));
        CHECK(f.first_sector_valid == (faults[i] == BEGIN_FAIL));
        CHECK(f.verifies == (faults[i] == SIGNATURE_FAIL || faults[i] == VERSION_FAIL ? 1u : 0u));
        if (faults[i] == STALL) CHECK(f.time == 30000000 && f.reads <= 31);
    }
    const size_t bad_sizes[] = {0, 4096, 8193, 8 * 1024 * 1024 + 4096u};
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); i++) {
        f = (fake_t){ 0 };
        CHECK(orb_assets_tx_apply(&p, bad_sizes[i], 8 * 1024 * 1024, buffer, sizeof(buffer)) == ORB_ASSETS_TX_SIZE);
        CHECK(f.begins == 0 && f.erases == 0 && f.ends == 0);
    }
    CHECK(orb_assets_tx_apply(&p, 8192, 4096, buffer, sizeof(buffer)) == ORB_ASSETS_TX_SIZE);
    CHECK(orb_assets_tx_apply(&p, 8192, 8192, buffer, 0) == ORB_ASSETS_TX_SIZE);
    CHECK(orb_assets_tx_apply(&p, 8192, 8192, NULL, sizeof(buffer)) == ORB_ASSETS_TX_SIZE);
    CHECK(orb_assets_tx_apply(NULL, 8192, 8192, buffer, sizeof(buffer)) == ORB_ASSETS_TX_SIZE);
    printf("orb_ota_assets: %u checks passed\n", s_checks);
    return 0;
}
