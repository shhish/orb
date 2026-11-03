#include "orb_ui.h"

lv_color_t orb_color_increased_contrast(lv_color_t color)
{
    /* HIG §§5/6: preserve semantic hues with the increased-contrast palette.
     * Secondary labels become white rather than relying on dim grey text. */
    static const struct { uint32_t normal, increased; } s_colours[] = {
        {ORB_RGB_LEGACY_SECONDARY, ORB_RGB_WHITE}, {ORB_RGB_LABEL_SECONDARY, ORB_RGB_WHITE}, {ORB_RGB_GRAY, ORB_RGB_WHITE},
        {ORB_RGB_POWER_LOW, ORB_RGB_POWER_LOW_CONTRAST}, {ORB_RGB_LEGACY_ORANGE, ORB_RGB_ORANGE_CONTRAST}, {ORB_RGB_LEGACY_YELLOW, ORB_RGB_YELLOW_CONTRAST},
        {ORB_RGB_GREEN, ORB_RGB_GREEN_CONTRAST}, {ORB_RGB_MINT, ORB_RGB_MINT_CONTRAST}, {ORB_RGB_LEGACY_TEAL, ORB_RGB_TEAL_CONTRAST},
        {ORB_RGB_CYAN, ORB_RGB_CYAN_CONTRAST}, {ORB_RGB_LEGACY_BLUE, ORB_RGB_BLUE_CONTRAST}, {ORB_RGB_LEGACY_INDIGO, ORB_RGB_INDIGO_CONTRAST},
        {ORB_RGB_PURPLE, ORB_RGB_PURPLE_CONTRAST}, {ORB_RGB_PINK, ORB_RGB_PINK_CONTRAST}, {ORB_RGB_BROWN, ORB_RGB_BROWN_CONTRAST},
        {ORB_RGB_GRAY2, ORB_RGB_GRAY2_CONTRAST}, {ORB_RGB_GRAY3, ORB_RGB_GRAY3_CONTRAST}, {ORB_RGB_GRAY4, ORB_RGB_GRAY4_CONTRAST},
        {ORB_RGB_GRAY5, ORB_RGB_GRAY5_CONTRAST}, {ORB_RGB_GRAY6, ORB_RGB_GRAY6_CONTRAST},
    };
    for (size_t i = 0; i < sizeof(s_colours) / sizeof(s_colours[0]); i++)
        if (lv_color_eq(color, orb_color_from_rgb(s_colours[i].normal)))
            return orb_color_from_rgb(s_colours[i].increased);
    return color;
}

