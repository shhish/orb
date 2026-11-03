#include <stdlib.h>
#include "orb_devtools_wire.h"

static unsigned s_checks;
#define CHECK(test) do { s_checks++; if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); exit(1); } } while (0)

static void command(const char *method, const char *json, const char *expected)
{
    cJSON *params = cJSON_Parse(json);
    uint32_t stream = 0;
    char buffer[ORB_DEVTOOLS_COMMAND_BYTES];
    bool ok = orb_devtools_wire_command(method, params, &stream, buffer);
    CHECK(ok == (expected != NULL));
    if (expected) { CHECK(stream == 7); CHECK(!strcmp(buffer, expected)); }
    cJSON_Delete(params);
}

int main(void)
{
    char *span = malloc(12);
    memcpy(span, "@@ORBDEV OK ", 12);
    CHECK(orb_devtools_wire_contains(span, 12, "@@ORBDEV OK "));
    CHECK(!orb_devtools_wire_contains(span, 11, "@@ORBDEV OK "));
    CHECK(!orb_devtools_wire_contains(span, 12, "@@ORBDEV ERROR"));
    free(span);
    const char *const no_args[] = {"shot", "heap", "top", "power", "buttons", "wake", "senses", "soundstatus", "soundstop", "micrecord"};
    for (unsigned i = 0; i < sizeof(no_args) / sizeof(no_args[0]); i++) {
        char method[40]; snprintf(method, sizeof(method), "devtools.%s", no_args[i]);
        command(method, "{\"stream\":7,\"args\":[]}", no_args[i]);
        command(method, "{\"stream\":7,\"args\":[1]}", NULL);
    }
    command("devtools.lightsleep", "{\"stream\":7,\"args\":[\"usb\",\"on\"]}", "lightsleep usb on");
    command("devtools.lightsleep", "{\"stream\":7,\"args\":[\"usb\",\"off\"]}", "lightsleep usb off");
    command("devtools.lightsleep", "{\"stream\":7,\"args\":[\"usb\",true]}", NULL);
    command("devtools.lightsleep", "{\"stream\":7,\"args\":[\"battery\",\"on\"]}", NULL);
    command("devtools.lightsleep", "{\"stream\":7,\"args\":[\"usb\",\"on\",1]}", NULL);
    command("devtools.tap", "{\"stream\":7,\"args\":[0,465]}", "tap 0 465");
    command("devtools.tilt", "{\"stream\":7,\"args\":[-0.25,0,5000]}", "tilt -0.25 0 5000");
    command("devtools.tilt", "{\"stream\":7,\"args\":[1,0,1]}", "tilt 1 0 1");
    command("devtools.tilt", "{\"stream\":7,\"args\":[0,0,-1]}", NULL);
    command("devtools.tilt", "{\"stream\":7,\"args\":[0,0,5001]}", NULL);
    command("devtools.tilt", "{\"stream\":7,\"args\":[0,0,1.5]}", NULL);
    command("devtools.tilt", "{\"stream\":7,\"args\":[1,1,100]}", NULL);
    command("devtools.tilt", "{\"stream\":7,\"args\":[true,0,100]}", NULL);
    command("devtools.tilt", "{\"stream\":7,\"args\":[0,0,100,1]}", NULL);
    command("devtools.tilt", "{\"stream\":7,\"args\":[\"0\",0,100]}", NULL);
    command("devtools.soundplay", "{\"stream\":7,\"args\":[18]}", "soundplay 18");
    command("devtools.soundplay", "{\"stream\":7,\"args\":[19]}", NULL);
    command("devtools.soundvolume", "{\"stream\":7,\"args\":[100]}", "soundvolume 100");
    command("devtools.soundvolume", "{\"stream\":7,\"args\":[101]}", NULL);
    command("devtools.soundsilent", "{\"stream\":7,\"args\":[1]}", "soundsilent 1");
    command("devtools.soundalwayson", "{\"stream\":7,\"args\":[1]}", "soundalwayson 1");
    command("devtools.soundalwayson", "{\"stream\":7,\"args\":[2]}", NULL);
    command("devtools.hold", "{\"stream\":7,\"args\":[233,400,5000]}", "hold 233 400 5000");
    command("devtools.swipe", "{\"stream\":7,\"args\":[0,465,465,0,1]}", "swipe 0 465 465 0 1");
    command("devtools.arc", "{\"stream\":7,\"args\":[216,350,370,400]}", "arc 216 350 370 400");
    command("devtools.arc", "{\"stream\":7,\"args\":[232,10,-10,5000]}", "arc 232 10 -10 5000");
    command("devtools.arc", "{\"stream\":7,\"args\":[0,0,90,400]}", NULL);
    command("devtools.arc", "{\"stream\":7,\"args\":[233,0,90,400]}", NULL);
    command("devtools.arc", "{\"stream\":7,\"args\":[216,-3601,90,400]}", NULL);
    command("devtools.arc", "{\"stream\":7,\"args\":[216,0,3601,400]}", NULL);
    command("devtools.arc", "{\"stream\":7,\"args\":[216,0,90,0]}", NULL);
    command("devtools.arc", "{\"stream\":7,\"args\":[216,0,90,5001]}", NULL);
    command("devtools.arc", "{\"stream\":7,\"args\":[216,0,90]}", NULL);
    command("devtools.arc", "{\"stream\":7,\"args\":[216,0,90,400,1]}", NULL);
    command("devtools.arc", "{\"stream\":7,\"args\":[216,false,90,400]}", NULL);
    command("devtools.arc", "{\"stream\":7,\"args\":[216,0.5,90,400]}", NULL);
    command("devtools.bench", "{\"stream\":7,\"args\":[60000]}", "bench 60000");
    command("devtools.bench_open", "{\"stream\":7,\"args\":[\"apps\"]}", "bench_open apps");
    command("devtools.bench_nav", "{\"stream\":7,\"args\":[\"back\"]}", "bench_open back");
    command("devtools.bench_scroll", "{\"stream\":7,\"args\":[\"favourites\",1000]}", "bench_scroll favourites 1000");
    command("devtools.bench_game", "{\"stream\":7,\"args\":[\"ring-snake\",1000]}", "bench_game ring-snake 1000");
    command("devtools.bench_game", "{\"stream\":7,\"args\":[\"beads\",6000]}", "bench_game beads 6000");
    command("devtools.press", "{\"stream\":7,\"args\":[\"pwr\",\"double\"]}", "press pwr double");
    command("devtools.goto", "{\"stream\":7,\"args\":[\"home\"]}", "goto home");
    command("devtools.goto", "{\"stream\":7,\"args\":[\"nowhere\"]}", NULL);
    command("devtools.goto", "{\"stream\":7,\"args\":[1]}", NULL);
    command("devtools.press", "{\"stream\":7,\"args\":[\"boot\",\"hold\"]}", "press boot hold");
    command("devtools.press", "{\"stream\":7,\"args\":[\"boot\",\"short\"]}", "press boot short");
    const char *const invalid[] = {
        "{}", "[]", "null", "{\"stream\":true,\"args\":[]}", "{\"stream\":0,\"args\":[]}",
        "{\"stream\":2147483648,\"args\":[]}", "{\"stream\":7.5,\"args\":[]}",
        "{\"stream\":7,\"args\":[],\"extra\":1}", "{\"stream\":7,\"args\":{}}",
        "{\"stream\":7,\"stream\":8,\"args\":[]}", "{\"stream\":7,\"args\":false}",
    };
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) command("devtools.shot", invalid[i], NULL);
    command("devtools.tap", "{\"stream\":7,\"args\":[466,0]}", NULL);
    command("devtools.tap", "{\"stream\":7,\"args\":[false,0]}", NULL);
    command("devtools.tap", "{\"stream\":7,\"args\":[1.5,0]}", NULL);
    command("devtools.tap", "{\"stream\":7,\"args\":[-1,0]}", NULL);
    command("devtools.hold", "{\"stream\":7,\"args\":[1,2,0]}", NULL);
    command("devtools.hold", "{\"stream\":7,\"args\":[1,2,5001]}", NULL);
    command("devtools.bench", "{\"stream\":7,\"args\":[999]}", NULL);
    command("devtools.bench", "{\"stream\":7,\"args\":[60001]}", NULL);
    command("devtools.bench_open", "{\"stream\":7,\"args\":[\"apps;reboot\"]}", NULL);
    command("devtools.bench_scroll", "{\"stream\":7,\"args\":[\"dot\",1000]}", NULL);
    command("devtools.bench_game", "{\"stream\":7,\"args\":[\"unknown\",1000]}", NULL);
    command("devtools.press", "{\"stream\":7,\"args\":[\"pwr;reboot\",\"short\"]}", NULL);
    command("devtools.press", "{\"stream\":7,\"args\":[\"pwr\",\"confirm\"]}", NULL);
    command("devtools.press", "{\"stream\":7,\"args\":[\"pwr\",1]}", NULL);
    command("devtools.press", "{\"stream\":7,\"args\":[\"pwr\",\"short\",1]}", NULL);
    command("devtools.profile", "{\"stream\":7,\"args\":[]}", NULL);
    command("shot", "{\"stream\":7,\"args\":[]}", NULL);
    command(NULL, "{\"stream\":7,\"args\":[]}", NULL);
    printf("devtools wire: %u checks passed\n", s_checks);
    return 0;
}
