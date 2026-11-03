# Golden screenshots

Reviewed hardware/simulator PNG baselines go here. None are fabricated or shipped
for the continuously scrolling demo. Establish a reproducible scene, run the
example scenarios with `--update`, inspect the report and PNGs, then commit the
accepted images. A missing golden fails comparison without `--update`.

The current demo moves its list every frame and shifts glass highlights with IMU
samples. Freeze or synchronize those inputs before using screenshots as a release
gate. Increasing tolerances or masking the entire panel would hide regressions.
