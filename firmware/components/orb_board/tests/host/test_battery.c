#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "orb_battery.h"

static void test_parse(void)
{
    orb_battery_registers_t r = {
        .status1=0x2a, .status2=0x20, .percent=80, .charger_control=0x0a,
        .adc_enable=0x1f, .ts_control=0x0e, .charge_current=9, .charge_voltage=3,
        .adc={0xcf,0xa0, 0xc3,0xe8, 0xd3,0x88, 0xd0,0x68, 0xc0,0x7b},
    };
    orb_battery_detail_t d;
    orb_battery_parse(&r,&d);
    assert(d.present && d.usb && d.charging && !d.discharging && d.thermal_regulation);
    assert(d.charger_enabled && d.gauge_enabled && d.ts_is_temperature);
    assert(d.battery_pct==80 && d.battery_mv==4000 && d.vbus_mv==5000 && d.system_mv==4200);
    assert(d.ts_uv==500000 && d.ts_source_ua==50 && d.ntc_ohms==10000 && d.die_temperature_raw==123);
    assert(d.charge_current_setting_ma==300 && d.charge_voltage_setting_mv==4200);
    assert(!d.current_valid && !d.health_valid && !d.temperature_valid && isnan(d.temperature_c));
    assert(d.charge_current_ma==-1 && d.discharge_current_ma==-1 && d.health_pct==-1);
    assert(orb_battery_temperature(&d,10000,3435));
    assert(d.temperature_valid && fabsf(d.temperature_c-25)<0.01f);
    r.status2=0x40; r.percent=255; r.charge_current=31; r.charge_voltage=7;
    orb_battery_parse(&r,&d);
    assert(d.discharging && !d.charging && d.battery_pct==-1);
    assert(d.charge_current_setting_ma==-1 && d.charge_voltage_setting_mv==-1);
    r.status1=0; r.adc_enable=0; r.ts_control=0x10;
    orb_battery_parse(&r,&d);
    assert(!d.present && !d.charging && !d.discharging && d.battery_pct==-1 && d.battery_mv==-1);
    assert(d.ts_uv==-1 && d.vbus_mv==-1 && !orb_battery_temperature(&d,10000,3435));
    for (unsigned n=0;n<=16;n++) {
        r.charge_current=n;
        orb_battery_parse(&r,&d);
        assert(d.charge_current_setting_ma==(n<=8?(int)n*25:200+((int)n-8)*100));
    }
}

static void test_desk(void)
{
    const uint64_t two_hours=7200000;
    orb_battery_desk_t s={.enabled=true};
    assert(!orb_battery_desk_update(&s,true,true,80,0));
    assert(!orb_battery_desk_update(&s,true,true,80,two_hours-1));
    assert(orb_battery_desk_update(&s,true,true,80,two_hours));
    assert(orb_battery_desk_update(&s,true,true,76,two_hours+1));
    assert(!orb_battery_desk_update(&s,true,true,75,two_hours+2));
    assert(orb_battery_desk_update(&s,true,true,80,two_hours+3));
    assert(!orb_battery_desk_update(&s,false,true,80,two_hours+4));
    assert(!s.active && !s.timing);
    assert(!orb_battery_desk_update(&s,true,true,100,two_hours+5));
    assert(!orb_battery_desk_update(&s,true,false,100,2*two_hours+5));
    assert(!orb_battery_desk_update(&s,true,true,100,3*two_hours));
    assert(!orb_battery_desk_update(&s,true,true,-1,4*two_hours));
    s.enabled=false;
    assert(!orb_battery_desk_update(&s,true,true,100,5*two_hours));
}

int main(void)
{
    test_parse(); test_desk();
    printf("orb_battery: ADC masks, direction, charge settings, unavailable data, calibrated NTC and 2h/80%% desk limit passed\n");
    return 0;
}
