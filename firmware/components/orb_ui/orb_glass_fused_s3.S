/* Orb's eight-lane horizontal bilinear interpolation, ESP32-S3 PIE.
 * Semantics from Espressif's ESP32-S3 TRM §1.8 (VMUL.S16 uses SAR).
 * Inputs/output are 16-byte-aligned internal int16_t[8] arrays; colors are
 * 0..255 and weights 0..255. Negative deltas retain arithmetic-floor rounding.
 * SAR is preserved for the caller. The vector registers are caller-saved. */
    .section .text
    .align 4
    .global orb_glass_fused_interpolate8_s3
    .type orb_glass_fused_interpolate8_s3,@function
orb_glass_fused_interpolate8_s3:
    entry a1, 32
    rsr.sar a6
    movi a7, 8
    wsr.sar a7
    ee.vld.128.ip q0, a3, 0
    ee.vld.128.ip q1, a4, 0
    ee.vld.128.ip q2, a5, 0
    ee.vsubs.s16 q3, q1, q0
    ee.vmul.s16 q3, q3, q2
    ee.vadds.s16 q0, q0, q3
    ee.vst.128.ip q0, a2, 0
    wsr.sar a6
    retw.n
    .size orb_glass_fused_interpolate8_s3, .-orb_glass_fused_interpolate8_s3
