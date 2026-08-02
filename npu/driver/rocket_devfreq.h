/* SPDX-License-Identifier: GPL-2.0-only */
/* devfreq eksperiment — LOKALNO, ne za slanje */
#ifndef __ROCKET_DEVFREQ_H__
#define __ROCKET_DEVFREQ_H__

struct rocket_core;

void rocket_devfreq_init(struct rocket_core *core);
/* jezgra 1/2 se vezuju posle core0 — naoruzaj i njihove genpd obavestioce */
void rocket_devfreq_core_added(struct rocket_core *core);
/* svako jezgro skida SVOJU genpd rampu, dok je jos zakaceno za domen */
void rocket_devfreq_core_removed(struct rocket_core *core);
void rocket_devfreq_fini(struct rocket_core *core);
void rocket_devfreq_shutdown(struct rocket_core *core);

/* iz runtime_suspend: vraca -EBUSY ako se takt NE MOZE vratiti na bezbedan */
int rocket_devfreq_suspend(struct rocket_core *core);

/*
 * Busy-time brojac za simple_ondemand (lekcija 5).
 * Zovu se iz rocket_job.c, sa SVA TRI mesta koja diraju core->in_flight_job.
 * Oba su idempotentna, pa dupli poziv ne kvari racun.
 */
void rocket_devfreq_record_busy(struct rocket_core *core);
void rocket_devfreq_record_idle(struct rocket_core *core);

#endif
