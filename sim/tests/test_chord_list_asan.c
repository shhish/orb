/* Instrument the actual painter while retaining the complete independent
 * ARGB/native/swapped oracle. The scalar geometry fixture has a private name
 * that otherwise collides with the painter's internal geometry helper. */
#include "../../firmware/components/orb_ui/orb_ui.c"
#define chord_geometry chord_oracle_geometry
#include "test_chord_list.c"
#undef chord_geometry
