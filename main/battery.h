#pragma once

// The battery's charge, if a sense wire is fitted (options().bat_pin: two
// equal resistors from BAT+ to the pin and the pin to GND; see
// battery_rules.h). The pin is set up at boot, so a wiring change applies
// after a restart.
void battery_init(); // after options_init()
void battery_tick(); // from loop()'s housekeeping: reads every few seconds
// "none" (not wired), "missing" (wired, no battery), "ok" or "low"; mv and
// pct are the last reading
const char *battery_state(int *mv, int *pct);
