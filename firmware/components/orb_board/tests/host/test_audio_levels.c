#include "orb_audio_levels.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    assert(orb_audio_volume_register(0) == 0);
    assert(orb_audio_volume_register(1) == 71); /* -60 dB */
    assert(orb_audio_volume_register(100) == 191); /* unity, never boost */
    for (unsigned i = 2; i <= 100; i++) {
        assert(orb_audio_volume_register(i) >= orb_audio_volume_register(i - 1));
        assert(orb_audio_volume_register(i) <= 191);
    }
    assert(orb_audio_volume_register(101) == 191);
    puts("volume register mapping: passed");
}
