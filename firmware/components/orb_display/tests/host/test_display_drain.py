"""Exercise the real benchmark SPI barrier, including diagnostic allocation OOM."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path


def main():
    root = Path(__file__).resolve().parents[5]
    source = (root / "firmware/components/orb_display/orb_display.c").read_text()
    block = source.split("bool orb_display_drain(void)", 1)[1].split("\n}", 1)[0]
    fixture = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
typedef struct { int brightness; } job_t;
static void *s_drain_done, *s_jobs = (void *)2, *s_flush_done = (void *)3, *s_disp = (void *)4;
static bool s_flush_pending, s_fail_alloc, s_fail_queue, s_fail_wait;
static unsigned s_flush_reclaimed, s_flush_ready;
#define portMAX_DELAY 7
#define pdTRUE 1
static void *xSemaphoreCreateBinary(void) { return s_fail_alloc ? NULL : (void *)1; }
static int xQueueSend(void *queue, const job_t *job, int timeout) {
    assert(queue == s_jobs && job->brightness == -1 && timeout == portMAX_DELAY);
    return !s_fail_queue;
}
static int xSemaphoreTake(void *sem, int timeout) {
    if (sem == s_flush_done) { assert(!timeout); s_flush_reclaimed++; return 1; }
    assert(sem == s_drain_done && timeout == portMAX_DELAY);
    return !s_fail_wait;
}
static void lv_display_flush_ready(void *display) { assert(display == s_disp); s_flush_ready++; }
bool orb_display_drain(void)
"""
    fixture += (
        block
        + r"""
}
int main(void) {
    s_fail_alloc = true;
    assert(!orb_display_drain() && !s_drain_done);
    s_fail_alloc = false;
    s_fail_queue = true;
    assert(!orb_display_drain() && s_drain_done);
    s_fail_queue = false;
    s_fail_wait = true;
    assert(!orb_display_drain());
    s_fail_wait = false;
    assert(orb_display_drain() && !s_flush_reclaimed && !s_flush_ready);
    s_flush_pending = true;
    assert(orb_display_drain());
    assert(!s_flush_pending && s_flush_reclaimed == 1 && s_flush_ready == 1);
    assert(orb_display_drain() && s_flush_reclaimed == 1 && s_flush_ready == 1);
    puts("display drain: OOM, queue/wait failure, FIFO barrier and source ownership passed");
}
"""
    )
    with tempfile.TemporaryDirectory(prefix="orb-drain-") as temporary:
        directory = Path(temporary)
        path = directory / "drain.c"
        path.write_text(fixture)
        binary = directory / "drain"
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-O1",
                "-g",
                "-fsanitize=address,undefined",
                str(path),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
