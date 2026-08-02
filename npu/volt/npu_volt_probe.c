// SPDX-License-Identifier: GPL-2.0-only
/*
 * npu_volt_probe — procitaj / postavi / procitaj nazad napon NPU sine
 *                  (vdd_npu_s0) na RK3588.  LOKALNO, eksperiment.
 *
 * ZASTO POSTOJI
 *   Merenjem 29.07. utvrdjeno je da NPU na 800 mV saturira oko ~676 MHz
 *   stvarnih: nominal 700 daje samo +1.5% nad nominal 600.  Vendor OPP
 *   tabela (dekodirana iz RK3588 vendor DTB-a) kaze da 800 MHz trazi
 *   750 mV, 900 MHz 800 mV, a 1000 MHz 850 mV.  Dakle zid nije termicki
 *   (600 MHz, 30 min => plato 55-56 C, dok je critical 115 C) nego
 *   naponski.  Ovaj modul postoji da taj zid pomerimo — u granicama
 *   fabricke specifikacije.
 *
 * TVRDA GRANICA U KODU
 *   VOLT_CAP = 850000 uV.  To NIJE proizvoljan broj: 850 mV je vendorov
 *   maksimum za ovu sinu i istovremeno napon koji tabela trazi za 1000 MHz.
 *   Iznad toga se ne dobija nijedan megaherc po papiru, pa modul odbija da
 *   se ucita.  (DT dozvoljava do 950000 — namerno NE koristimo tu rezervu.)
 *
 *   VOLT_FLOOR = 800000 uV, tj. zatecena vrednost.  Ovaj modul sluzi SAMO
 *   za dizanje napona; spustanje ispod zateceng je zaseban eksperiment sa
 *   drugom klasom rizika (undervolt => tihi pogresan racun), pa ga ne
 *   dozvoljavamo ovde.
 *
 * ZASTO JE READBACK CELA POENTA
 *   regulator_set_voltage() bira najblizi korak koji PMIC ume (fan53555 ima
 *   diskretne korake) i ne mora da pogodi tacno ono sto smo trazili.  Zato
 *   je jedini dokaz regulator_get_voltage() posle upisa — isto pravilo kao
 *   kod clk_set_rate/clk_get_rate u npu_clk_probe.
 *
 * STA OVAJ MODUL NIKAD NE RADI
 *   Nikad regulator_enable() ni regulator_disable().  Analiza od 28.07:
 *   vdd_npu_s0 je always-on iz DT-a i ima num_users=0, pa bi disable pri
 *   rmmod-u mogao da obori magistralu dok je NPU power domen ziv.
 *   regulator_get + set_voltage + get_voltage ne diraju enable stanje.
 *
 * SPREGA SA DRAJVEROM
 *   rocket devfreq odbija svaki takt iznad 700 MHz dok sa regulatora ne
 *   PROCITA >= 850000 uV.  Napon i takt dizu dva razlicita modula, pa se
 *   drajver namerno ne oslanja na dogovor nego na merenje.
 *
 * Upotreba:
 *   insmod ./npu_volt_probe.ko              # samo procitaj
 *   insmod ./npu_volt_probe.ko uv=850000    # postavi 850 mV
 *   rmmod npu_volt_probe                    # vrati zatecenu vrednost
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>

static int uv;
module_param(uv, int, 0444);
MODULE_PARM_DESC(uv, "Target NPU rail voltage in microvolts (0 = read-only)");

#define NPU_NODE_PATH	"/npu@fdab0000"
#define NPU_SUPPLY	"npu"
#define VOLT_FLOOR	800000		/* nikad ispod zatecenog */
#define VOLT_VENDOR_MAX	850000		/* Rockchip spec za NPU sinu */
#define VOLT_CAP	950000		/* regulator-max-microvolt iz DT-a = granica PLOCE */

static struct regulator *npu_reg;
static struct platform_device *npu_pdev;
static int orig_uv;

static int __init npu_volt_probe_init(void)
{
	struct device_node *np;
	int ret, got;

	np = of_find_node_by_path(NPU_NODE_PATH);
	if (!np) {
		pr_err("device-tree node %s not found\n", NPU_NODE_PATH);
		return -ENODEV;
	}

	npu_pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!npu_pdev) {
		pr_err("no platform device for %s\n", NPU_NODE_PATH);
		return -ENODEV;
	}

	npu_reg = regulator_get(&npu_pdev->dev, NPU_SUPPLY);
	if (IS_ERR(npu_reg)) {
		ret = PTR_ERR(npu_reg);
		pr_err("regulator_get(\"%s\") failed: %d\n", NPU_SUPPLY, ret);
		npu_reg = NULL;
		goto err_pdev;
	}

	orig_uv = regulator_get_voltage(npu_reg);
	if (orig_uv < 0) {
		ret = orig_uv;
		pr_err("cannot read rail voltage: %d\n", ret);
		goto err_reg;
	}
	pr_info("current rail voltage: %d uV\n", orig_uv);

	if (!uv) {
		pr_info("read-only mode, not touching anything\n");
		return 0;
	}

	if (uv < VOLT_FLOOR || uv > VOLT_CAP) {
		pr_err("uv=%d outside [%d, %d], refusing to load\n",
		       uv, VOLT_FLOOR, VOLT_CAP);
		ret = -EINVAL;
		goto err_reg;
	}

	/*
	 * Dve razlicite granice, namerno razdvojene:
	 *   VOLT_VENDOR_MAX (850 mV) = sta Rockchip kaze za ovu sinu
	 *   VOLT_CAP        (950 mV) = sta ploca elektricno sme (DT constraint)
	 * Izmedju njih je legalno ali van specifikacije — modul to pusti, ali
	 * nikad tiho.  Izmereno na ovoj ploci: 800->850 mV dalo je TACNO 0%,
	 * jer zid nije naponski nego memorijski.
	 */
	if (uv > VOLT_VENDOR_MAX)
		pr_warn("uv=%d is ABOVE the vendor maximum of %d for this rail - out of spec, expect no gain\n",
			uv, VOLT_VENDOR_MAX);

	if (uv < orig_uv) {
		pr_err("uv=%d is below current %d — this module only raises, refusing\n",
		       uv, orig_uv);
		ret = -EINVAL;
		goto err_reg;
	}

	/*
	 * Uzak prozor [uv, uv]: ne dozvoljavamo jezgru da "pomogne" tako sto
	 * izabere neki visi korak. Ako PMIC nema tacno taj korak, neka radije
	 * ne uspe nego da nas tiho odvede iznad plafona.
	 */
	ret = regulator_set_voltage(npu_reg, uv, uv);
	pr_info("regulator_set_voltage(%d) returned %d (NOT proof - see readback)\n",
		uv, ret);
	if (ret)
		goto err_reg;

	got = regulator_get_voltage(npu_reg);
	if (got < 0) {
		pr_err("readback failed: %d\n", got);
		ret = got;
		goto err_restore;
	}

	pr_info("READBACK: %d uV (asked %d, was %d)\n", got, uv, orig_uv);

	if (got > VOLT_CAP) {
		pr_err("readback %d ABOVE cap %d — restoring and refusing\n",
		       got, VOLT_CAP);
		ret = -ERANGE;
		goto err_restore;
	}
	if (got != uv)
		pr_warn("rail settled on %d uV, not %d — PMIC step granularity\n",
			got, uv);

	return 0;

err_restore:
	regulator_set_voltage(npu_reg, orig_uv, orig_uv);
err_reg:
	regulator_put(npu_reg);
	npu_reg = NULL;
err_pdev:
	platform_device_put(npu_pdev);
	npu_pdev = NULL;
	return ret;
}

static void __exit npu_volt_probe_exit(void)
{
	if (npu_reg) {
		if (uv && orig_uv > 0) {
			int ret = regulator_set_voltage(npu_reg, orig_uv, orig_uv);
			int got = regulator_get_voltage(npu_reg);

			pr_info("restore to %d uV: ret=%d, readback %d uV\n",
				orig_uv, ret, got);
		}
		regulator_put(npu_reg);	/* NIKAD regulator_disable */
	}
	if (npu_pdev)
		platform_device_put(npu_pdev);

	pr_info("unloaded\n");
}

module_init(npu_volt_probe_init);
module_exit(npu_volt_probe_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Igor Paunovic <royalnet026@gmail.com>");
MODULE_DESCRIPTION("Probe: read/set/readback the RK3588 NPU rail voltage");
