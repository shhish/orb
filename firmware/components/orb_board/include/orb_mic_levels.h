#pragma once

/* Relative to the reviewed PGA30 / digital0 recording: +6 dB analogue and
 * +16 dB digital. Keep DSP absolute floors in the same input-level domain. */
#define ORB_MIC_PGA_REGISTER 13u /* ES7210 36 dB, register 0x43/44 bits 3:0. */
#define ORB_MIC_DIGITAL_REGISTER 223u /* 0xBF unity + 32 half-dB steps. */
#define ORB_MIC_LEVEL_SCALE 12.58925412f /* 10 ^ (22 / 20). */
