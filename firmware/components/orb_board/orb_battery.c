#include "orb_battery.h"

#include <math.h>

static int adc14(const uint8_t *p)
{
    return ((p[0] & 0x3f) << 8) | p[1];
}

void orb_battery_parse(const orb_battery_registers_t *r, orb_battery_detail_t *out)
{
    static const int source_ua[] = {20, 40, 50, 60};
    static const int charge_mv[] = {5000, 4000, 4100, 4200, 4350, 4400, -1, -1};
    *out = (orb_battery_detail_t){
        .present = (r->status1 & 0x08) != 0,
        .usb = (r->status1 & 0x20) != 0,
        .thermal_regulation = (r->status1 & 0x02) != 0,
        .charger_enabled = (r->charger_control & 0x02) != 0,
        .gauge_enabled = (r->charger_control & 0x08) != 0,
        .ts_enabled = (r->adc_enable & 0x02) != 0,
        .ts_is_temperature = (r->ts_control & 0x10) == 0,
        .battery_pct = -1,
        .charge_current_ma = -1, .discharge_current_ma = -1, .health_pct = -1,
        .temperature_c = NAN,
        .charge_voltage_setting_mv = charge_mv[r->charge_voltage & 7],
        .vbus_mv = (r->adc_enable & 0x04) ? adc14(r->adc + 4) : -1,
        .system_mv = (r->adc_enable & 0x08) ? adc14(r->adc + 6) : -1,
        .die_temperature_raw = (r->adc_enable & 0x10) ? adc14(r->adc + 8) : -1,
        .ts_uv = -1, .ts_source_ua = -1, .ntc_ohms = -1,
    };
    unsigned current = r->charge_current & 0x1f;
    out->charge_current_setting_ma = current > 16 ? -1 :
                                    current <= 8 ? (int)current * 25 : 200 + ((int)current - 8) * 100;
    if (out->present) {
        unsigned direction = (r->status2 >> 5) & 3;
        out->charging = direction == 1;
        out->discharging = direction == 2;
        if (out->gauge_enabled && r->percent <= 100) out->battery_pct = r->percent;
        out->battery_mv = (r->adc_enable & 0x01) ? adc14(r->adc) : -1;
    } else out->battery_mv = -1;
    if (out->ts_enabled) {
        out->ts_uv = adc14(r->adc + 2) * 500;
        if ((r->ts_control >> 2) & 3) {
            out->ts_source_ua = source_ua[r->ts_control & 3];
            out->ntc_ohms = out->ts_uv / out->ts_source_ua;
        }
    }
}

bool orb_battery_temperature(orb_battery_detail_t *d, float r25, float beta)
{
    if (d) {
        d->temperature_valid = false;
        d->temperature_c = NAN;
    }
    if (!d || !d->present || !d->ts_enabled || !d->ts_is_temperature ||
        d->ntc_ohms <= 0 || !isfinite(r25) || !isfinite(beta) || r25 <= 0 || beta <= 0) return false;
    float inverse = 1.0f / 298.15f + logf((float)d->ntc_ohms / r25) / beta;
    if (inverse <= 0) return false;
    float c = 1.0f / inverse - 273.15f;
    if (!isfinite(c) || c < -40 || c > 125) return false;
    d->temperature_c = c;
    d->temperature_valid = true;
    return true;
}

bool orb_battery_desk_update(orb_battery_desk_t *s, bool usb, bool resting, int pct, uint64_t now)
{
    if (!s->enabled || !usb || !resting || pct < 0 || pct > 100) {
        s->timing = s->active = false;
        return false;
    }
    if (!s->timing || now < s->desk_since_ms) {
        s->desk_since_ms = now;
        s->timing = true;
    }
    if (now - s->desk_since_ms >= 2u * 60u * 60u * 1000u && pct >= 80) s->active = true;
    if (pct <= 75) s->active = false;
    return s->active;
}
