#pragma once

/* ES8311 DS rev 10, registers 0x31/32/34/35/37. Keep the output below unity
 * gain; the PA needs no digital boost. 1..100 maps -60..0 dB, 0 is hard mute. */
static inline unsigned orb_audio_volume_register(unsigned volume)
{
    if (volume > 100) volume = 100;
    return volume ? 71u + (120u * (volume - 1u) + 49u) / 99u : 0u;
}
