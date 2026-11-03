#!/usr/bin/env bash
# Reproduce Design 1.0 sound/system comparison sheets with isolated fixtures.
set -euo pipefail
cd "$(dirname "$0")/.."
nice cmake --build sim/build -j4
nice ctest --test-dir sim/build -j4 --output-on-failure \
    -R 'sound_design1|system_views|nowplaying_design1|^time_default$|^time_ax3_bold_rt$|sound_emu_'
for mode in default ax3-bold-rt; do
    for name in SoundSettings VolumeHUD SpokenReply AlarmRinging NowPlaying Battery Onboarding; do
        render="sim/build/sound-ui/$mode/$name.png"
        if [[ "$name" == AlarmRinging ]]; then
            render="sim/build/time-$mode/AlarmRinging.png"
        fi
        python3 sim/png_tools.py concat "docs/design/1.0/png/$name.png" "$render" \
            "sim/build/sound-ui/$name-$mode-sheet.png"
    done
done
