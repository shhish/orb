#!/usr/bin/env bash
# Checks that the simulator's LVGL settings (sim/lv_conf.h) still match the
# firmware's: expands LVGL's lv_conf_internal.h once as the firmware build saw
# it (its sdkconfig.h) and once as the simulator does, and prints every
# setting that differs, apart from the simulator's deliberate differences
# (listed in lv_conf.h). Exit status 1 if there are any.
#
#   sim/conf-diff.sh [path/to/sdkconfig.h]   default: firmware/build/config/sdkconfig.h
set -euo pipefail

sim="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$sim/.." && pwd)"
lvgl="$repo/firmware/third_party/lvgl__lvgl"
sdkconfig="${1:-$repo/firmware/build/config/sdkconfig.h}"

if [[ ! -f "$sdkconfig" ]]; then
    echo "conf-diff.sh: no $sdkconfig; build the firmware first (scripts/idf.sh build)" >&2
    exit 2
fi

# The simulator's choices, the files each side configures LVGL with, and
# Kconfig-only extras (the calendar's names).
deliberate='LV_USE_OS|LV_OS_USE_FREERTOS|LV_OS_USE_PTHREAD|LV_USE_FREERTOS_TASK_NOTIFY|LV_DRAW_THREAD_STACK_SIZE'
deliberate+='|LV_USE_DRAW_SW_ASM|LV_DRAW_SW_ASM_CUSTOM_INCLUDE|LV_DRAW_SW_ASM_USE_CUSTOM_INCLUDE|LV_USE_LODEPNG'
deliberate+='|LV_ASSERT_HANDLER|LV_CONF_H|LV_CONF_KCONFIG_H|LV_CONF_SKIP|LV_KCONFIG_IGNORE'
deliberate+='|LV_CALENDAR_DISABLE_DEFAULT_DAY_NAMES|LV_CALENDAR_DISABLE_DEFAULT_MONTH_NAMES'

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# What ESP-IDF's sdkconfig.h path pulls in besides the settings.
mkdir "$tmp/esp"
: >"$tmp/esp/esp_attr.h"
: >"$tmp/esp/orb_lv_blend.h"

# expand OUT CFLAGS...: "NAME<tab>value" for every LV_ setting, sorted; values
# that are arithmetic are evaluated, so "0 && 0" and "0" compare equal.
expand() {
    local out=$1 name value
    shift
    printf '#include "lv_conf_internal.h"\n' >"$tmp/names.c"
    clang -E -dM "$@" "$tmp/names.c" | awk '$1 == "#define" && $2 ~ /^LV_[A-Z0-9_]+$/ { print $2 }' |
        sort -u >"$tmp/names"
    {
        printf '#include "lv_conf_internal.h"\n'
        while read -r name; do
            printf '@@"%s" = %s\n' "$name" "$name"
        done <"$tmp/names"
    } >"$tmp/values.c"
    clang -E -P "$@" "$tmp/values.c" | sed -n 's/^@@"\([A-Z0-9_]*\)" =/\1/p' |
        while read -r name value; do
            if [[ -n "$value" && "$value" =~ ^[0-9\ \(\)\!\&\|\=\<\>\+\*/-]+$ ]]; then
                value=$((value))
            fi
            printf '%s\t%s\n' "$name" "$value"
        done | LC_ALL=C sort -t $'\t' -k1,1 >"$out"
}

expand "$tmp/firmware" -DESP_PLATFORM -DLV_CONF_INCLUDE_SIMPLE -I"$tmp/esp" -I"$(dirname "$sdkconfig")" -I"$lvgl/include/lvgl/config"
expand "$tmp/sim" -DLV_CONF_INCLUDE_SIMPLE -DLV_KCONFIG_IGNORE -I"$sim" -I"$lvgl/include/lvgl/config"

LC_ALL=C join -t $'\t' -a 1 -a 2 -e '(unset)' -o '0,1.2,2.2' "$tmp/firmware" "$tmp/sim" |
    awk -F '\t' -v ok="^($deliberate)\$" '$2 != $3 && $1 !~ ok { printf "%s: firmware %s, sim %s\n", $1, $2, $3; n++ }
        END { if (n) exit 1; print "sim/lv_conf.h matches the firmware'"'"'s LVGL settings" }'
