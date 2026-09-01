#ifndef CLOCK_H
#define CLOCK_H

extern _Atomic bool force_clock_update;
extern _Atomic bool date_shown;
extern _Atomic bool blink_colon;
extern _Atomic bool use_vegas;
extern _Atomic bool do_vegas;

void init_clock();

void show_date(bool show);

#define CATHODE_PROTECTION_HOUR 2

#endif  // CLOCK_H
