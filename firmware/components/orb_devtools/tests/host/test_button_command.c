#include "orb_button_command.h"

#include <stdlib.h>

static unsigned s_checks;
#define CHECK(test) do { s_checks++; if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); exit(1); } } while (0)

int main(void)
{
    static const char *const buttons[] = {"pwr", "boot"};
    static const char *const gestures[] = {"short", "double", "hold"};
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned k = 0; k < 3; k++) {
            char command[32];
            snprintf(command, sizeof(command), "press %s %s", buttons[i], gestures[k]);
            orb_button_t button;
            orb_button_gesture_t gesture;
            CHECK(orb_devtools_button_command(command, &button, &gesture));
            CHECK((unsigned)button == i);
            CHECK((unsigned)gesture == k + ORB_BUTTON_SHORT);
        }
    }
    static const char *const invalid[] = {
        "press", "press pwr", "press gpio3 short", "press PWR short", "press pwr confirm",
        "press pwr down", "press pwr up", "press pwr hold 2000", "press pwr short\nheap",
        "press  pwr short", "press pwr  short", "press pwr short ", " press pwr short",
        "press\tpwr short", "press boot;reboot short", "buttons",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        orb_button_t button;
        orb_button_gesture_t gesture;
        CHECK(!orb_devtools_button_command(invalid[i], &button, &gesture));
    }
    printf("button command: %u checks passed\n", s_checks);
    return 0;
}
