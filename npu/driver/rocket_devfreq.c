// SPDX-License-Identifier: GPL-2.0-only
/*
 * devfreq za rocket NPU (RK3588) — eksperiment, LOKALNO.
 *
 * ====================== ZASTO OVAKO ======================
 * Hardverska cinjenica placena sa 4 pada 31.07.2026:
 *
 *   NPU power domen se NE MOZE upaliti dok je compute klok iznad boot
 *   vrednosti (200 MHz). PVTPLL koji pravi taj klok zivi UNUTAR domena,
 *   pa gasenje domena ubije njegovo stanje; sledeci power-on ne dobije
 *   ack ("failed to get ack on domain 'nputop'" -> -110), domen ostane
 *   zaglavljen, i PRVI sledeci MMIO u njega die asinhroni SError ->
 *   kernel panic.
 *
 * Recenzija je pokazala da ovo ne vazi samo za nputop: pd_npu1 i pd_npu2
 * (jezgra 1 i 2) TAKODJE imaju <&cru CLK_NPU_DSU0> medju svojim handshake
 * klokovima. Dakle nije dovoljno spustiti takt pre gasenja — nijedan
 * domen ne sme ni da se PALI dok je klok gore.
 *
 * Zato: dok je takt podignut, DRZIMO SVA TRI JEZGRA BUDNIM (pm_runtime
 * reference). Tada nema nijedne power tranzicije, pa nema ni prilike za
 * kvar. To je tacno ono sto je radio npu_clk_probe.ko modul koji je bez
 * ijednog incidenta presao cele merdevine 300-700 MHz.
 *
 * Cena: dok je NPU "boostovan", ne spava. To je svesna razmena —
 * bezbednost pre stednje — i vidljiva je korisniku: takt se dize samo
 * kad ga on izricito trazi kroz userspace governor.
 *
 * PM kukica ispod je jos jedan sloj odbrane (pojas i tregeri): ako se
 * ijedno jezgro ipak uspava, takt se BEZUSLOVNO vraca na bezbedan.
 * =========================================================
 */
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/devfreq.h>
#include <linux/devfreq_cooling.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/spinlock.h>
#include <linux/thermal.h>
#include <linux/pm_domain.h>
#include <linux/pm_opp.h>
#include <linux/pm_runtime.h>

#include "rocket_core.h"
#include "rocket_device.h"
#include "rocket_devfreq.h"

/*
 * Bezbedan takt NIJE nesto sto se uzorkuje sa zive masine — to je
 * vrednost iz device tree-ja (assigned-clock-rates). Da se uzorkuje,
 * jedan unbind/bind dok je NPU boostovan bi "bezbednim" proglasio 600.
 */
#define ROCKET_SAFE_RATE	200000000UL

/*
 * ============ NAPONSKA BLOKADA (Igorov predlog, 01.08) ============
 * Vendor OPP tabela (dekodirana iz RK3588 vendor DTB-a): 300-700 MHz traze
 * 700 mV, 800 MHz 750 mV, 900 MHz 800 mV, 1000 MHz 850 mV — a 850 mV je i
 * fabricki PLAFON te sine. Mi radimo na 800 mV, sto pokriva sve do 900.
 *
 * Napon i takt ce dizati DVA RAZLICITA modula (napon zaseban probe modul,
 * takt ovaj drajver). Zato drajver NE SME da pretpostavi da su uskladjeni:
 * svaki takt iznad ROCKET_HIGH_RATE se odbija dok se sa regulatora ne
 * PROCITA najmanje ROCKET_VOLT_FOR_HIGH. Pada zatvoreno — ako regulator
 * nije dostupan ili citanje ne uspe, visok takt je zabranjen.
 */
#define ROCKET_HIGH_RATE	700000000UL	/* iznad ovoga treba vise napona */
#define ROCKET_VOLT_FOR_HIGH	850000		/* uV — vendor cap, pokriva 1000 MHz */

static struct {
	struct mutex lock;		/* stanje: core, req_rate, refs_held */
	struct mutex rate_lock;		/* SAMO oko upisa u klok */
	struct rocket_core *core;	/* jezgro koje nosi devfreq (core 0) */
	unsigned long req_rate;		/* takt koji je korisnik trazio */
	bool refs_held;			/* drzimo li boost reference */
	bool unsafe;			/* neki domen nije pod nadzorom */
	struct regulator *reg;		/* SAMO za citanje napona, nikad enable/disable */
	struct thermal_cooling_device *cdev;
} rocket_df = {
	.lock = __MUTEX_INITIALIZER(rocket_df.lock),
	.rate_lock = __MUTEX_INITIALIZER(rocket_df.rate_lock),
	.req_rate = ROCKET_SAFE_RATE,
};

/*
 * RAMPA NA PRUZI (lekcija 6): genpd zove ove obavestioce iz
 * _genpd_power_on()/_genpd_power_off() — najdubljih funkcija kroz koje
 * MORAJU proci svi putevi (runtime PM, sistemsko uspavljivanje, attach
 * pri probe-u, detach posle unbind-a). Zato nas ovde niko ne moze
 * zaobici, a PRE_ON/PRE_OFF smeju i da ODBIJU tranziciju.
 *
 * Reference na jezgra (boost) su i dalje tu — ali samo zbog brzine.
 * BEZBEDNOST od sada garantuju iskljucivo ova obavestenja.
 */
#define ROCKET_DF_MAX_NB	4

static struct rocket_df_nb {
	struct notifier_block nb;
	struct rocket_core *core;
	bool armed;
} rocket_df_nb[ROCKET_DF_MAX_NB];

/*
 * ================= BUSY-TIME BROJAC (lekcija 5) =================
 * simple_ondemand pita samo jedno: "koliko je NPU bio zauzet od proslog
 * puta". Merimo to ISKLJUCIVO softverski (ktime), NIKAD citanjem registara:
 * get_dev_status se zove iz anketnog posla, bez ijedne PM reference, dakle i
 * kad je domen ugasen — a MMIO u ugasen domen je asinhroni SError.
 *
 * NPU je zauzet tacno dok core->in_flight_job nije NULL. To polje se pise na
 * TRI mesta u rocket_job.c (submit, kraj posla u IRQ niti, i prekid u reset
 * putanji); sva tri zovu ove funkcije. Ako se zaboravi ono u reset putanji,
 * brojac ostaje "zauvek zauzet" -> governor vidi 100% -> zakuca se na maksimum.
 *
 * Jedna brava za sva jezgra: po jedan par poziva na posao (~100/s po jezgru),
 * takmicenje je zanemarljivo, a jedna brava se lakse dokazuje nego tri.
 */
static DEFINE_SPINLOCK(rocket_df_busy_lock);

static struct {
	ktime_t start;			/* 0 = jezgro trenutno nije zauzeto */
	u64 busy_ns;			/* akumulirano od poslednjeg citanja */
} rocket_df_busy[ROCKET_DF_MAX_NB];

static ktime_t rocket_df_busy_last_read;

void rocket_devfreq_record_busy(struct rocket_core *core)
{
	unsigned long flags;
	unsigned int i;

	if (!core || core->index >= ROCKET_DF_MAX_NB)
		return;
	i = core->index;

	spin_lock_irqsave(&rocket_df_busy_lock, flags);
	if (!rocket_df_busy[i].start)		/* idempotentno */
		rocket_df_busy[i].start = ktime_get();
	spin_unlock_irqrestore(&rocket_df_busy_lock, flags);
}

void rocket_devfreq_record_idle(struct rocket_core *core)
{
	unsigned long flags;
	unsigned int i;
	ktime_t now;

	if (!core || core->index >= ROCKET_DF_MAX_NB)
		return;
	i = core->index;

	now = ktime_get();

	spin_lock_irqsave(&rocket_df_busy_lock, flags);
	if (rocket_df_busy[i].start) {		/* idempotentno: reset put sme dvaput */
		rocket_df_busy[i].busy_ns +=
			ktime_to_ns(ktime_sub(now, rocket_df_busy[i].start));
		rocket_df_busy[i].start = 0;
	}
	spin_unlock_irqrestore(&rocket_df_busy_lock, flags);
}

/*
 * Zauzetost koju prijavljujemo je MAKSIMUM po jezgrima, ne prosek.
 * Izmereno na ovoj ploci: MobileNetV1 daje IRQ potpis 41/1/0 — jezgro 0
 * odradi 41 chunk, jezgro 1 jedan, jezgro 2 nista. Prosek bi bio ~34% i
 * governor bi drzao nizak takt dok je jezgro 0 zakucano na 100%. Klok je
 * deljen, pa je pitanje "da li je IJEDNO jezgro zasiceno".
 */
static int rocket_devfreq_get_dev_status(struct device *dev,
					 struct devfreq_dev_status *status)
{
	struct rocket_device *rdev;
	unsigned long flags;
	ktime_t now = ktime_get();
	u64 window_ns, max_busy = 0;
	unsigned int i, n;

	mutex_lock(&rocket_df.lock);
	rdev = rocket_df.core ? rocket_df.core->rdev : NULL;
	status->current_frequency = rocket_df.req_rate;
	mutex_unlock(&rocket_df.lock);

	n = rdev ? rdev->max_cores : 0;
	if (n > ROCKET_DF_MAX_NB)
		n = ROCKET_DF_MAX_NB;

	spin_lock_irqsave(&rocket_df_busy_lock, flags);
	for (i = 0; i < n; i++) {
		u64 busy = rocket_df_busy[i].busy_ns;

		/* jezgro koje je BAS SAD zauzeto: uracunaj i deo u toku */
		if (rocket_df_busy[i].start) {
			busy += ktime_to_ns(ktime_sub(now,
						      rocket_df_busy[i].start));
			rocket_df_busy[i].start = now;	/* nastavi da meri odavde */
		}
		rocket_df_busy[i].busy_ns = 0;

		if (busy > max_busy)
			max_busy = busy;
	}
	window_ns = ktime_to_ns(ktime_sub(now, rocket_df_busy_last_read));
	rocket_df_busy_last_read = now;
	spin_unlock_irqrestore(&rocket_df_busy_lock, flags);

	/*
	 * MINA: total_time == 0 tera simple_ondemand da ODMAH trazi
	 * DEVFREQ_MAX_FREQ (governor_simpleondemand.c). Nikad ne vracamo nulu,
	 * i nikad busy > total (racun mora ostati <= 100%).
	 */
	if (!window_ns)
		window_ns = 1;
	if (max_busy > window_ns)
		max_busy = window_ns;

	status->total_time = window_ns;
	status->busy_time = max_busy;

	return 0;
}

static void rocket_df_arm_notifiers(struct rocket_device *rdev);
static void rocket_df_disarm_notifiers(void);
static bool rocket_df_all_guarded(struct rocket_device *rdev);

static bool rocket_devfreq_owns(struct rocket_core *core)
{
	struct rocket_core *owner = READ_ONCE(rocket_df.core);

	return owner && core && core->dev && core->rdev == owner->rdev;
}

static struct clk *rocket_devfreq_clk(void)
{
	return rocket_df.core->clks[2].clk;	/* "npu" */
}

/* Probudi SVA jezgra — nijedan domen ne sme da se pali dok je klok gore. */
static int rocket_devfreq_hold_all(void)
{
	struct rocket_device *rdev = rocket_df.core->rdev;
	int ret;

	for (unsigned int i = 0; i < rdev->max_cores; i++) {
		if (!rdev->cores[i].dev)
			continue;

		ret = pm_runtime_resume_and_get(rdev->cores[i].dev);
		if (ret < 0) {
			/* vrati sve sto smo do sad uzeli */
			while (i--) {
				if (rdev->cores[i].dev)
					pm_runtime_put(rdev->cores[i].dev);
			}
			return ret;
		}
	}

	return 0;
}

static void rocket_devfreq_release_all(void)
{
	struct rocket_device *rdev = rocket_df.core->rdev;

	for (unsigned int i = 0; i < rdev->max_cores; i++) {
		if (!rdev->cores[i].dev)
			continue;
		pm_runtime_mark_last_busy(rdev->cores[i].dev);
		pm_runtime_put_autosuspend(rdev->cores[i].dev);
	}
}

static int rocket_devfreq_target(struct device *dev, unsigned long *freq,
				 u32 flags)
{
	struct dev_pm_opp *opp;
	unsigned long old;
	bool took = false;
	int ret = 0;

	opp = devfreq_recommended_opp(dev, freq, flags);
	if (IS_ERR(opp))
		return PTR_ERR(opp);
	dev_pm_opp_put(opp);

	mutex_lock(&rocket_df.lock);

	if (!rocket_df.core) {
		ret = -ENODEV;
		goto out;
	}

	/*
	 * RANI IZLAZ — uslov bez kojeg simple_ondemand nije upotrebljiv.
	 *
	 * devfreq_set_target() nas zove na SVAKI otkucaj governora, i onda kad
	 * se nista ne menja (nema provere "isti takt" u devfreq jezgru). Bez
	 * ovoga bi svaki otkucaj radio hold_all(), tj. palio sva tri NPU
	 * domena — deset puta u sekundi, doveka, i kad NPU nema nikakav posao.
	 *
	 * ALI ne smemo da verujemo samo svom knjigovodstvu: 1. krug recenzije
	 * je oborio bas takav rani izlaz ("nikad ne TVRDIMO bezbedan takt, pa
	 * tudja promena — npr. npu_clk_probe.ko — ostaje"). Zato PROVERAVAMO
	 * klok pre preskakanja. clk_get_rate nad SCMI klokom ide u firmware
	 * kroz mailbox, ne u NPU domen, pa je bezbedan i dok domen spava
	 * (empirijski dokazano u R0 i u probnom modulu).
	 *
	 * IZNAD SAFE preskacemo samo ako nismo u 'unsafe' stanju: inace bi
	 * jedna neuspela rampa (unsafe=true dok je takt vec gore) ostala
	 * nevidljiva jer bi svaki sledeci zahtev za istim taktom izasao ovde.
	 * Na SAFE taktu preskacemo uvek — klok je proveren, nema sta da se
	 * popravi, a 'unsafe' je trajna zasuna pa bismo inace mleli doveka.
	 */
	if (*freq == rocket_df.req_rate &&
	    (*freq == ROCKET_SAFE_RATE || !READ_ONCE(rocket_df.unsafe)) &&
	    clk_get_rate(rocket_devfreq_clk()) == *freq)
		goto out;			/* ret je 0 */

	/*
	 * Boost je dozvoljen SAMO ako je svaki domen pod rampom: ako neko
	 * jezgro jos nije vezano (ili mu rampa nije postavljena), njegov
	 * domen bi se upalio na visokom taktu bez ijedne kontrole.
	 */
	if (*freq > ROCKET_SAFE_RATE) {
		struct rocket_device *rdev = rocket_df.core->rdev;

		if (READ_ONCE(rocket_df.unsafe) ||
		    !rocket_df_all_guarded(rdev)) {
			dev_warn(dev, "devfreq: not all NPU domains guarded - refusing %lu Hz\n",
				 *freq);
			ret = -EAGAIN;
			goto out;
		}
	}

	/*
	 * NAPONSKA BLOKADA: iznad 700 MHz trazimo dokaz sa regulatora, ne
	 * pretpostavku. Napon dize drugi modul; ako ga nije digao (ili ga je
	 * spustio), ovde stajemo. Pada zatvoreno i kad regulatora nema.
	 */
	if (*freq > ROCKET_HIGH_RATE) {
		int uv = rocket_df.reg ? regulator_get_voltage(rocket_df.reg) : -ENODEV;

		if (uv < ROCKET_VOLT_FOR_HIGH) {
			dev_warn(dev, "devfreq: %lu Hz needs >= %d uV, rail reads %d - refusing\n",
				 *freq, ROCKET_VOLT_FOR_HIGH, uv);
			ret = -EPERM;
			goto out;
		}
	}

	/*
	 * UVEK probudi sva jezgra, i kad mislimo da ih vec drzimo: nasa
	 * zastavica moze da zaostane za stvarnoscu (sistemsko uspavljivanje
	 * ignorise reference), a ovo je idempotentno i jeftino.
	 */
	ret = rocket_devfreq_hold_all();
	if (ret < 0) {
		dev_err(dev, "devfreq: cannot power up for rate change: %d\n",
			ret);
		goto out;
	}
	took = true;

	old = rocket_df.req_rate;
	rocket_df.req_rate = *freq;	/* objavi PRE promene */

	/*
	 * NE preko OPP sloja: nasi direktni clk_set_rate(SAFE) pozivi (rampe,
	 * suspend kukica, remove) NE azuriraju OPP kes, pa bi ponovni zahtev
	 * za ISTI takt postao tih no-op — devfreq bi prijavljivao 600 dok
	 * hardver radi 200, i bench bi merio pogresan broj pod pogresnim
	 * imenom. Tabela je bez napona/bandwidth-a, pa OPP sloj ovde i ne
	 * donosi nista; devfreq_recommended_opp() je vec proverio da je
	 * trazeni takt iz tabele.
	 */
	mutex_lock(&rocket_df.rate_lock);
	ret = clk_set_rate(rocket_devfreq_clk(), *freq);
	if (!ret) {
		unsigned long got = clk_get_rate(rocket_devfreq_clk());

		if (got != *freq) {
			dev_warn(dev, "devfreq: asked %lu Hz, got %lu Hz\n",
				 *freq, got);
			*freq = got;		/* prijavi ISTINU, ne zelju */
		}
	}
	mutex_unlock(&rocket_df.rate_lock);

	if (ret) {
		rocket_df.req_rate = old;
		dev_err(dev, "devfreq: set_rate %lu failed: %d\n", *freq, ret);
	}

	if (rocket_df.req_rate > ROCKET_SAFE_RATE) {
		/* novi set referenci postaje boost set; stari (ako ga je bilo) pusti */
		if (rocket_df.refs_held)
			rocket_devfreq_release_all();
		rocket_df.refs_held = true;
		took = false;			/* zadrzavamo ih */
	} else {
		if (rocket_df.refs_held) {
			rocket_df.refs_held = false;
			rocket_devfreq_release_all();	/* stari boost set */
		}
	}

	if (took)
		rocket_devfreq_release_all();		/* prolazni set */

out:
	mutex_unlock(&rocket_df.lock);
	return ret;
}

/*
 * NE pitaj firmware za takt — devfreq zove ovo i iz sysfs-a, bez ikakve
 * PM reference, pa i kad je domen ugasen. Vracamo softversko stanje.
 */
static int rocket_devfreq_get_cur_freq(struct device *dev, unsigned long *freq)
{
	mutex_lock(&rocket_df.lock);
	*freq = rocket_df.req_rate;
	mutex_unlock(&rocket_df.lock);

	return 0;
}

/*
 * polling_ms vazi SAMO pod simple_ondemand — governor_userspace uopste ne
 * pokrece anketni posao (nema devfreq_monitor_start u njegovom handleru),
 * pa je ovo bezopasno dok je userspace izabran. 100 ms je ~7-15 inferenci
 * na ovom modelu: dovoljno gust uzorak, a dovoljno redak otkucaj.
 */
static struct devfreq_dev_profile rocket_devfreq_profile = {
	.timer = DEVFREQ_TIMER_DELAYED,
	.polling_ms = 100,
	.target = rocket_devfreq_target,
	.get_dev_status = rocket_devfreq_get_dev_status,
	.get_cur_freq = rocket_devfreq_get_cur_freq,
};

/*
 * Pragovi za simple_ondemand. Prosledjuju se pri registraciji iako pocinjemo
 * sa userspace governorom — devfreq ih samo cuva u df->data, pa su spremni
 * kad Igor prebaci governor kroz sysfs.
 *
 * 50/10 je PRVA PROCENA, ne izmerena vrednost (panthor je svojih 45/5 dobio
 * eksperimentom, sto i pise u njegovom kodu). Podici takt na 50% zauzetosti
 * ima smisla za posao koji dolazi u naletima; tuning ide posle merenja.
 */
static struct devfreq_simple_ondemand_data rocket_df_gov_data = {
	.upthreshold = 50,
	.downdifferential = 10,
};

/*
 * Sigurnosna mreza: svako uspavljivanje jezgra znaci da neki domen moze
 * da se ugasi. Takt tada MORA biti bezbedan — bezuslovno, bez obzira ko
 * ga je i zasto podigao (npr. neki drugi modul).
 */
int rocket_devfreq_suspend(struct rocket_core *core)
{
	struct rocket_core *owner = READ_ONCE(rocket_df.core);
	int ret;

	if (!owner || !core || !core->dev || core->rdev != owner->rdev)
		return 0;

	/*
	 * NAMERNO BEZ rocket_df.lock — inace uklestenje: target() drzi lock
	 * i ceka pm_runtime_resume_and_get() nad jezgrom koje se bas tada
	 * uspavljuje, a njegov suspend callback bi cekao taj isti lock.
	 *
	 * Lock nam ovde i ne treba: dok je boost aktivan drzimo reference
	 * na SVIM jezgrima, pa se nijedno ne moze uspavati u trenutku
	 * promene takta — trkacki prozor je prazan po konstrukciji.
	 */
	ret = clk_set_rate(core->clks[2].clk, ROCKET_SAFE_RATE);	/* SVOJ handle */

	if (ret) {
		/*
		 * Bolje vruc NPU nego zaglavljen domen: odbij uspavljivanje,
		 * runtime PM ce ostaviti i uredjaj i domen pod naponom.
		 */
		dev_err(core->dev,
			"devfreq: cannot restore safe rate (%d), refusing suspend\n",
			ret);
		return -EBUSY;
	}

	return 0;
}

/* Zove se iz rocket_remove(), dok je domen jos zagarantovano ziv. */
void rocket_devfreq_fini(struct rocket_core *core)
{
	int ret;

	if (!rocket_devfreq_owns(core) || core != rocket_df.core)
		return;

	/*
	 * Cooling odlazi PRVI, dok devfreq uredjaj jos postoji: thermal jezgro
	 * drzi pokazivac na njega, a devfreq nestaje tek u devm ciscenju posle
	 * .remove. Obrnut redosled ostavlja mrtav pokazivac.
	 */
	if (rocket_df.cdev) {
		devfreq_cooling_unregister(rocket_df.cdev);
		rocket_df.cdev = NULL;
	}

	mutex_lock(&rocket_df.lock);

	/* od ovog trenutka nista vise ne sme da digne takt */
	rocket_df.req_rate = ROCKET_SAFE_RATE;

	if (rocket_df.refs_held) {
		rocket_df.refs_held = false;
		ret = clk_set_rate(rocket_devfreq_clk(), ROCKET_SAFE_RATE);
		if (ret)
			dev_err(core->dev, "devfreq: fini set_rate failed: %d\n",
				ret);
		rocket_devfreq_release_all();
	} else if (!pm_runtime_status_suspended(core->dev)) {
		ret = clk_set_rate(rocket_devfreq_clk(), ROCKET_SAFE_RATE);
		if (ret)
			dev_err(core->dev, "devfreq: fini set_rate failed: %d\n",
				ret);
	}

	rocket_df.core = NULL;

	mutex_unlock(&rocket_df.lock);

	rocket_df_disarm_notifiers();
}

/* reboot/kexec: ostavi klok na bezbednom taktu za sledeci kernel */
void rocket_devfreq_shutdown(struct rocket_core *core)
{
	if (!rocket_devfreq_owns(core) || core != rocket_df.core)
		return;

	if (!pm_runtime_status_suspended(core->dev))
		clk_set_rate(rocket_devfreq_clk(), ROCKET_SAFE_RATE);
}

static int rocket_df_notify(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	struct rocket_df_nb *n = container_of(nb, struct rocket_df_nb, nb);
	struct rocket_core *core = READ_ONCE(n->core);
	struct rocket_core *owner = READ_ONCE(rocket_df.core);
	struct device *ldev;
	int ret = 0;

	/* orphan blok (jezgro vec otislo) ne sme da dereferencira nista */
	if (!core || !core->dev || !owner)
		return NOTIFY_OK;

	ldev = core->dev;

	switch (action) {
	case GENPD_NOTIFY_PRE_ON:
	case GENPD_NOTIFY_PRE_OFF:
		/*
		 * Domen samo sto se nije upalio ili ugasio — takt MORA biti
		 * bezbedan. rate_lock je JEDINA brava ovde: cuva samo upis u
		 * klok, pa ne moze da se uklesti sa target()-om (koji je nikad
		 * ne drzi preko pm_runtime poziva).
		 */
		mutex_lock(&rocket_df.rate_lock);

		/*
		 * SVOJ handle, ne vlasnikov: sva tri jezgra drze isti
		 * SCMI_CLK_NPU, a vlasnikovi handle-ovi nestanu sa njegovim
		 * devres-om — citanje kroz njih je use-after-free.
		 */
		if (clk_get_rate(core->clks[2].clk) != ROCKET_SAFE_RATE)
			ret = clk_set_rate(core->clks[2].clk, ROCKET_SAFE_RATE);

		/*
		 * Tranzicija se desila — znaci da nas je neki put zaobisao
		 * (sistemsko uspavljivanje, unbind, attach pri probe-u).
		 * Boost je izgubljen; recimo to naglas umesto da softver
		 * tvrdi nesto sto hardver ne radi.
		 */
		if (READ_ONCE(rocket_df.req_rate) != ROCKET_SAFE_RATE) {
			WRITE_ONCE(rocket_df.req_rate, ROCKET_SAFE_RATE);
			dev_info(ldev,
				 "devfreq: domain transition, boost dropped to %lu Hz\n",
				 ROCKET_SAFE_RATE);
		}

		mutex_unlock(&rocket_df.rate_lock);

		if (ret) {
			dev_err(ldev, "devfreq: cannot make clock safe: %d\n",
				ret);
			/*
			 * Veto vracamo za OBA smera: genpd_power_on() na
			 * runtime putu ga POSTUJE i uredno prekine paljenje.
			 * Samo genpd_sync_power_on() (noirq put sistemskog
			 * budjenja) odbacuje povratnu vrednost — tamo nam
			 * ostaje samo glasan dev_err gore.
			 */
			return notifier_from_errno(ret);
		}
		break;
	}

	return NOTIFY_OK;
}

/*
 * num_cores NIJE test "sve je pod rampom" — probe ga uveca PRE nego sto
 * rampa bude postavljena. Brojimo stvarno naoruzane.
 */
static bool rocket_df_all_guarded(struct rocket_device *rdev)
{
	unsigned int armed = 0;

	for (unsigned int i = 0; i < ROCKET_DF_MAX_NB; i++)
		if (rocket_df_nb[i].armed)
			armed++;

	return armed == rdev->max_cores;
}

static void rocket_df_arm_notifiers(struct rocket_device *rdev)
{
	for (unsigned int i = 0; i < rdev->max_cores && i < ROCKET_DF_MAX_NB; i++) {
		struct rocket_core *c = &rdev->cores[i];
		int ret;

		if (!c->dev || rocket_df_nb[i].armed)
			continue;

		rocket_df_nb[i].core = c;
		rocket_df_nb[i].nb.notifier_call = rocket_df_notify;

		ret = dev_pm_genpd_add_notifier(c->dev, &rocket_df_nb[i].nb);
		if (ret) {
			/*
			 * PADAMO ZATVORENO: domen bez rampe je domen koji se
			 * moze upaliti na visokom taktu. Bez boosta dok se
			 * modul ne ucita iznova.
			 */
			dev_err(c->dev, "devfreq: genpd notifier failed (%d) - boost disabled\n",
				ret);
			rocket_df_nb[i].core = NULL;
			WRITE_ONCE(rocket_df.unsafe, true);
			continue;
		}
		rocket_df_nb[i].armed = true;
	}
}

/*
 * Rampa se MORA skinuti dok je jezgro jos zakaceno za svoj domen.
 * genpd_remove_device() NE cisti power_notifiers, a dev->pm_domain
 * nestane pri detach-u — posle toga je blok zauvek zarobljen u lancu
 * domena i pri sledecem modprobe-u kernel skace u oslobodjenu memoriju.
 * Zato: svako jezgro skida SVOJU rampu, iz svog remove-a.
 */
void rocket_devfreq_core_removed(struct rocket_core *core)
{
	if (!core || !core->dev)
		return;

	/*
	 * REDOSLED JE SVE: prvo odustani od boosta, pa tek onda skini rampu.
	 * Obrnuto je rupa — jezgro bez rampe a klok jos gore, pa genpd ugasi
	 * njegov domen iz radne niti cim .remove vrati => zid. (I ne cekaj
	 * vlasnika: driver_detach odvezuje OBRNUTIM redom, on ide poslednji.)
	 */
	mutex_lock(&rocket_df.lock);
	if (rocket_df.core) {
		int cret;

		rocket_df.req_rate = ROCKET_SAFE_RATE;

		mutex_lock(&rocket_df.rate_lock);
		if (clk_get_rate(core->clks[2].clk) != ROCKET_SAFE_RATE) {
			cret = clk_set_rate(core->clks[2].clk, ROCKET_SAFE_RATE);
			if (cret) {
				dev_err(core->dev,
					"devfreq: cannot make clock safe on remove: %d\n",
					cret);
				WRITE_ONCE(rocket_df.unsafe, true);
			}
		}
		mutex_unlock(&rocket_df.rate_lock);

		if (rocket_df.refs_held) {
			rocket_df.refs_held = false;
			rocket_devfreq_release_all();
		}
	}
	mutex_unlock(&rocket_df.lock);

	for (unsigned int i = 0; i < ROCKET_DF_MAX_NB; i++) {
		int ret;

		if (!rocket_df_nb[i].armed || rocket_df_nb[i].core != core)
			continue;

		ret = dev_pm_genpd_remove_notifier(core->dev);
		if (ret) {
			/* blok ostaje u lancu — dalji boost je neprihvatljiv */
			dev_err(core->dev, "devfreq: genpd notifier removal failed: %d\n",
				ret);
			WRITE_ONCE(rocket_df.unsafe, true);
			return;
		}

		rocket_df_nb[i].armed = false;
		WRITE_ONCE(rocket_df_nb[i].core, NULL);
		return;
	}
}

static void rocket_df_disarm_notifiers(void)
{
	/* poslednja mreza: sve sto je ostalo naoruzano posle svih remove-ova */
	for (unsigned int i = 0; i < ROCKET_DF_MAX_NB; i++) {
		if (!rocket_df_nb[i].armed || !rocket_df_nb[i].core ||
		    !rocket_df_nb[i].core->dev)
			continue;
		if (dev_pm_genpd_remove_notifier(rocket_df_nb[i].core->dev))
			continue;		/* NE brisi bookkeeping ako nije skinut */
		rocket_df_nb[i].armed = false;
		WRITE_ONCE(rocket_df_nb[i].core, NULL);
	}
}

void rocket_devfreq_core_added(struct rocket_core *core)
{
	if (!rocket_devfreq_owns(core))
		return;

	rocket_df_arm_notifiers(core->rdev);
}

void rocket_devfreq_init(struct rocket_core *core)
{
	struct dev_pm_opp_config cfg = {
		.clk_names = (const char *[]){ "npu", NULL },
	};
	/*
	 * Ceo vendor opseg. Stepenici iznad ROCKET_HIGH_RATE postoje u tabeli
	 * ali ih naponska blokada u target() odbija dok sina ne bude na 850 mV
	 * — tabela opisuje sta hardver UME, blokada cuva sta sme SADA.
	 */
	static const unsigned long steps[] = {
		300000000, 400000000, 500000000, 600000000, 700000000,
		800000000, 900000000, 1000000000,
	};
	struct device *dev = core->dev;
	struct devfreq *df;
	unsigned long cur;
	int ret;

	if (rocket_df.core)		/* vec registrovan na nekom jezgru */
		return;

	/* procitaj takt sa upaljenim domenom */
	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0) {
		dev_warn(dev, "devfreq: cannot power up for baseline: %d\n", ret);
		return;
	}
	cur = clk_get_rate(core->clks[2].clk);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	/*
	 * Registrujemo se SAMO ako je klok tacno na bezbednom taktu. Ako
	 * nije, nesto ga je vec podiglo — tada ne znamo sta je bezbedno.
	 */
	if (cur != ROCKET_SAFE_RATE) {
		dev_warn(dev, "devfreq: NPU clock at %lu Hz, expected %lu - not registering\n",
			 cur, ROCKET_SAFE_RATE);
		return;
	}

	/*
	 * Regulator uzimamo ISKLJUCIVO da bismo mogli da PROCITAMO napon za
	 * blokadu iznad 700 MHz. Nikad regulator_enable/disable i nikad ga ne
	 * dajemo OPP sloju — analiza od 28.07 je pokazala da bi disable pri
	 * rmmod-u nad ovom sinom (num_users=0, always-on iz DT-a) mogao da
	 * obori magistralu. regulator_get + regulator_get_voltage ne diraju
	 * stanje sine.
	 */
	rocket_df.reg = devm_regulator_get_optional(dev, "npu");
	if (IS_ERR(rocket_df.reg)) {
		dev_info(dev, "devfreq: no npu-supply (%ld) - rates above %lu Hz stay blocked\n",
			 PTR_ERR(rocket_df.reg), ROCKET_HIGH_RATE);
		rocket_df.reg = NULL;
	}

	dev_pm_opp_remove_all_dynamic(dev);

	ret = devm_pm_opp_set_config(dev, &cfg);
	if (ret) {
		dev_warn(dev, "devfreq: opp set_config failed: %d\n", ret);
		return;
	}

	ret = dev_pm_opp_add(dev, ROCKET_SAFE_RATE, 0);
	if (ret) {
		dev_warn(dev, "devfreq: opp add failed: %d\n", ret);
		return;
	}

	for (unsigned int i = 0; i < ARRAY_SIZE(steps); i++) {
		ret = dev_pm_opp_add(dev, steps[i], 0);
		if (ret)
			dev_warn(dev, "devfreq: opp %lu failed: %d\n",
				 steps[i], ret);
	}

	rocket_devfreq_profile.initial_freq = ROCKET_SAFE_RATE;

	/* pocetak prvog mernog prozora za busy-time */
	rocket_df_busy_last_read = ktime_get();

	mutex_lock(&rocket_df.lock);
	rocket_df.core = core;
	rocket_df.req_rate = ROCKET_SAFE_RATE;
	rocket_df.refs_held = false;
	mutex_unlock(&rocket_df.lock);

	/*
	 * NAMERNO userspace, ne simple_ondemand: modul se uvek ucita u stanju
	 * koje je hardverski dokazano (soak 30 min, 01.08). Automatika se pali
	 * jednim upisom u sysfs, i gasi istim takvim — pa lose podesen governor
	 * ne moze da pokvari ni ucitavanje modula ni povratak u ispravno stanje.
	 */
	df = devm_devfreq_add_device(dev, &rocket_devfreq_profile,
				     DEVFREQ_GOV_USERSPACE,
				     &rocket_df_gov_data);
	if (IS_ERR(df)) {
		dev_warn(dev, "devfreq: add_device failed: %ld\n", PTR_ERR(df));
		mutex_lock(&rocket_df.lock);
		rocket_df.core = NULL;
		mutex_unlock(&rocket_df.lock);
		return;
	}

	rocket_df_arm_notifiers(core->rdev);

	/*
	 * Cooling uredjaj. Registrujemo ga i kad ga nijedna termalna zona ne
	 * vozi: mainline npu-thermal ima SAMO critical trip na 115 C i nema
	 * cooling-maps (za razliku od gpu-thermal, 25 linija iznad u istom
	 * dtsi-ju). Dok se ta DT rupa ne zakrpi, zastitu radi userspace cuvar
	 * preko max_freq — ali uredjaj mora da postoji da bi DT patch imao sta
	 * da veze, i da bismo videli da kod radi.
	 *
	 * NIJE devm: mi se odjavljujemo iz rocket_devfreq_fini(). panthor ga
	 * nikad ne odjavljuje jer se ne skida iz kernela; nas modul se skida
	 * stalno, pa bi neodjavljen cdev ostavio mrtav pokazivac u thermal
	 * jezgru.
	 */
	rocket_df.cdev = devfreq_cooling_em_register(df, NULL);
	if (IS_ERR(rocket_df.cdev)) {
		dev_info(dev, "devfreq: no cooling device (%ld)\n",
			 PTR_ERR(rocket_df.cdev));
		rocket_df.cdev = NULL;
	}

	dev_info(dev, "devfreq registered at %lu Hz (userspace governor, genpd-notifier guarded, cooling %s)\n",
		 ROCKET_SAFE_RATE, rocket_df.cdev ? "yes" : "no");
}
