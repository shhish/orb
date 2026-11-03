"""Exercise production voice callback mapping without ESP task headers."""

import os
import shlex
import subprocess
import tempfile
from pathlib import Path

here = Path(__file__).resolve().parent
component = here.parents[1]
source = (component / "orb_voice.c").read_text()
definitions = source[
    source.index("typedef enum { EVENT_REPLY") : source.index("#define VOICE_EVENTS")
]
reply = source[source.index("static void on_reply(") : source.index("static bool request_send(")]
events = source[source.index("static bool stream_id(") : source.index("static void voice_task(")]
speech = source[
    source.index("static void speech_stopped(") : source.index("static void on_speech_pcm(")
]
cjson = component.parents[1] / "managed_components" / "espressif__cjson" / "cJSON"
with tempfile.TemporaryDirectory(prefix="orb-voice-events-") as tmp:
    work = Path(tmp)
    (work / "voice_events_types.inc").write_text(definitions)
    (work / "voice_events.inc").write_text(reply + events + speech)
    binary = work / "test_events"
    compiler = shlex.split(os.environ.get("CC", "clang"))
    flags = shlex.split(
        os.environ.get(
            "CFLAGS", "-std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined"
        )
    )
    subprocess.run(
        [
            *compiler,
            *flags,
            "-Wno-deprecated-declarations",
            "-I",
            str(cjson),
            "-c",
            str(cjson / "cJSON.c"),
            "-o",
            str(work / "cJSON.o"),
        ],
        check=True,
    )
    subprocess.run(
        [
            *compiler,
            *flags,
            "-I",
            str(work),
            "-I",
            str(component),
            "-I",
            str(cjson),
            "-I",
            str(component.parent / "orb_model" / "include"),
            "-I",
            str(component.parent / "orb_link"),
            "-pthread",
            str(here / "test_events.c"),
            str(component.parent / "orb_link" / "link_v11.c"),
            str(component.parent / "orb_model" / "orb_model.c"),
            str(component.parent / "orb_model" / "orb_model_voice_notify.c"),
            str(component / "voice_core.c"),
            str(component / "speech_core.c"),
            str(work / "cJSON.o"),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
