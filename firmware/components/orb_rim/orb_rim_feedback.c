#include "orb_rim_math.h"
#include "orb_sound.h"

bool orb_rim_detent(float value, unsigned steps, int *selected, bool audible)
{
    if (!selected) return false;
    int tick = orb_rim_detent_index(value, steps);
    if (tick == *selected) return false;
    *selected = tick;
    /* Every step, ends included, is the palette's detent tick (Sound and
     * Senses). START/STOP alias the listening chimes, which made a drag
     * alternate between two unrelated sounds. */
    (void)steps;
    if (audible) orb_sound_play(ORB_SOUND_DETENT);
    return true;
}
