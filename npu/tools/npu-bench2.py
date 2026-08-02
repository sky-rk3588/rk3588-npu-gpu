#!/usr/bin/env python3
"""
NPU benchmark v2 — A/B-posten, za poredjenje NPU taktova (RK3588 / rocket / Teflon).

Zasto v2: npu-bench.py je imao 4 mane zbog kojih A/B poredjenje nije bilo posteno:
  1. Mesao NPU i CPU vreme. MobileNetV1 se deli na 4 dela: 27 op na NPU ->
     AVGPOOL na CPU -> 1 op na NPU -> SPATIALSQUEEZE/SOFTMAX na CPU. Plus
     Teflon-ova re-layout petlja i mmap/munmap po tenzoru = jos CPU-a.
     Ako je CPU deo p ukupnog vremena, Amdahl kaze da je maksimalno ubrzanje
     1/p BEZ OBZIRA koliko dignes takt. -> v2 meri CPU vreme (getrusage) i
     racuna predikciju, pa znamo sta DA OCEKUJEMO pre nego sto dignemo takt.
  2. Merilo mu je bio TEFLON_DEBUG=verbose (celobrojne ms) — nije instrument.
  3. Orakl "military uniform" je preslab: argmax uint8, samo poslednja iteracija,
     iza AVGPOOL+SOFTMAX koji gusi greske. -> v2 radi bit-exact sha256.
  4. Okruzenje nekontrolisano (governori, loadavg) i sirov izlaz se nije cuvao.
     -> v2 snima ceo JSON u ~/npu-bench-results/ i odbija da radi na zauzetoj masini.

ORAKL SE IZVODI AUTOMATSKI, ne hardkoduje se:
  - dve razlicite slike -> tenzori koji se MENJAJU = stvarno se racunaju
    (57 od 64 citljiva tenzora su tezine/konstante — beskorisne kao orakl)
  - tri ista runa -> tenzori koji su STABILNI = deterministicki
  - orakl = presek. Time se sam od sebe izbacuje tenzor idx=89 (1,112,112,27),
    delegate-ov scratch bafer sa zaostalim smecem koji se razlikuje u svakom runu
    i pravio bi lazne alarme na svakom merenju.
  NPU je deterministican integer datapath -> NULTA tolerancija. Bilo koja promena
  hesa pri OC-u = racun je pokvaren, takt je previsok.

Primer:
  source ~/teflon-venv/bin/activate
  python ~/npu-bench2.py --label baseline-200mhz
  # posle OC-a:
  python ~/npu-bench2.py --label npu-700mhz --compare ~/npu-bench-results/<baseline>.json
"""

import argparse
import glob
import hashlib
import json
import os
import platform
import resource
import statistics
import subprocess
import sys
import time
from datetime import datetime

import numpy as np
from PIL import Image
import tflite_runtime.interpreter as tflite

HOME = os.path.expanduser("~")
DEF_MODEL = f"{HOME}/mesa/src/gallium/targets/teflon/tests/models/mobilenetv1/mobilenet_v1_1_224_quant.tflite"
DEF_LABELS = f"{HOME}/mesa/src/gallium/frontends/teflon/tests/labels_mobilenet_quant_v1_224.txt"
DEF_IMAGE = f"{HOME}/grace_hopper.bmp"
DEF_DELEGATE = "/usr/local/lib/aarch64-linux-gnu/libteflon.so"
RESULTS_DIR = f"{HOME}/npu-bench-results"


# ----------------------------------------------------------------------------
# okruzenje: sve citljivo kao obican korisnik (bez sudo)
# ----------------------------------------------------------------------------

def _read(path, default=None):
    try:
        with open(path) as f:
            return f.read().strip()
    except Exception:
        return default


def _thermal_zones():
    zones = {}
    for p in sorted(glob.glob("/sys/class/thermal/thermal_zone*")):
        t = _read(f"{p}/type")
        v = _read(f"{p}/temp")
        if t and v:
            try:
                zones[t] = int(v) / 1000.0
            except ValueError:
                pass
    return zones


def _vdd_npu_uv():
    """Napon NPU sine PO IMENU — indeksi regulatora se menjaju izmedju bootova!"""
    import glob as _glob
    for r in _glob.glob("/sys/class/regulator/regulator.*"):
        if _read(r + "/name", "").strip() == "vdd_npu_s0":
            return int(_read(r + "/microvolts", "0") or 0)
    return 0


def _npu_irqs():
    """Broj prekida po NPU jezgru. Klok-nezavisna invarijanta:
    IRQ po inferenci MORA ostati isti kad se takt promeni.
    Ako se promeni -> menja se KOLICINA posla, poredjenje ne vazi."""
    out = {}
    txt = _read("/proc/interrupts", "")
    for line in txt.splitlines():
        if "npu" not in line.lower():
            continue
        head, _, rest = line.partition(":")
        nums = []
        for tok in rest.split():
            if tok.isdigit():
                nums.append(int(tok))
            else:
                break
        out[head.strip()] = sum(nums)
    return out


def _cpufreq():
    out = {}
    for cpu in (0, 4, 6):
        base = f"/sys/devices/system/cpu/cpu{cpu}/cpufreq"
        gov = _read(f"{base}/scaling_governor")
        if gov is None:
            continue
        out[f"cpu{cpu}"] = {
            "governor": gov,
            "cur_khz": int(_read(f"{base}/scaling_cur_freq", "0") or 0),
            "max_khz": int(_read(f"{base}/scaling_max_freq", "0") or 0),
        }
    return out


def _npu_state():
    st = {}
    for node in ("fdab0000", "fdac0000", "fdad0000"):
        v = _read(f"/sys/devices/platform/{node}.npu/power/runtime_status")
        if v:
            st[node] = v
    return st


def _devfreq():
    """Ako Path B jednom uspe, NPU ce se pojaviti ovde. Do tada prazno."""
    out = {}
    for p in sorted(glob.glob("/sys/class/devfreq/*")):
        name = os.path.basename(p)
        out[name] = {
            "cur_freq": _read(f"{p}/cur_freq"),
            "governor": _read(f"{p}/governor"),
        }
    return out


def snapshot():
    la = (_read("/proc/loadavg", "0 0 0") or "0 0 0").split()
    return {
        "loadavg": [float(x) for x in la[:3]],
        "thermal_c": _thermal_zones(),
        "npu_irq": _npu_irqs(),
        "cpufreq": _cpufreq(),
        "npu_runtime_status": _npu_state(),
        "vdd_npu_s0_uv": _vdd_npu_uv(),
        "devfreq": _devfreq(),
    }


# ----------------------------------------------------------------------------
# orakl: izvodi se iz modela, ne hardkoduje
# ----------------------------------------------------------------------------

def _temp(zones, needle):
    for k, v in zones.items():
        if needle in k:
            return v
    return float("nan")


def derive_oracle(interp, in_idx, verbose=True):
    """Vrati listu indeksa tenzora koji su (a) racunati, (b) deterministicki
    i (c) NISU ulazni tenzor.

    (c) je bitno: ulaz se trivijalno "menja sa ulazom" pa upadne u skup
    racunatih, a najveci je (150 KB) pa bi bio izabran za duboku proveru —
    a hesiranje ulaza ne dokazuje NISTA o tome sta je NPU izracunao."""
    def run(seed):
        a = np.random.default_rng(seed).integers(0, 256, (1, 224, 224, 3), dtype=np.uint8)
        interp.set_tensor(in_idx, a)
        interp.invoke()
        snap = {}
        for d in interp.get_tensor_details():
            try:
                t = interp.get_tensor(d["index"])
                if t.size > 1:
                    snap[d["index"]] = t.copy()
            except Exception:
                pass
        return snap

    a1, a2 = run(7), run(99)
    computed = {i for i in a1 if i in a2 and not np.array_equal(a1[i], a2[i])}
    computed.discard(in_idx)

    # tri identicna runa -> odbaci nedeterministicke (scratch baferi)
    reps = [run(7) for _ in range(3)]
    stable = set()
    for i in computed:
        if all(i in r for r in reps) and all(np.array_equal(reps[0][i], r[i]) for r in reps[1:]):
            stable.add(i)

    dropped = computed - stable
    names = {d["index"]: (d["name"] or "(bez imena)") for d in interp.get_tensor_details()}
    sizes = {i: int(a1[i].nbytes) for i in computed}

    if verbose:
        print(f"  orakl: {len(computed)} racunatih tenzora, "
              f"{len(stable)} determinstickih -> u orakl")
        for i in sorted(dropped, key=lambda x: -sizes[x]):
            print(f"    ODBACEN idx={i} ({sizes[i]} B, {names[i][:40]}) "
                  f"— nije deterministican (scratch bafer)")
        for i in sorted(stable, key=lambda x: -sizes[x]):
            print(f"    orakl   idx={i} ({sizes[i]} B, {names[i][:40]})")

    # najveci = "duboki" (skup), najmanji = "brzi" (svaka iteracija)
    ordered = sorted(stable, key=lambda x: -sizes[x])
    return ordered, {i: sizes[i] for i in stable}, {i: names[i] for i in stable}


def sha(t):
    return hashlib.sha256(np.ascontiguousarray(t).tobytes()).hexdigest()


# ----------------------------------------------------------------------------
def main():
    p = argparse.ArgumentParser(description="NPU benchmark v2 (A/B posten)")
    p.add_argument("--label", required=True,
                   help='ime runa, npr. "baseline-200mhz" ili "npu-700mhz"')
    p.add_argument("-i", "--image", default=DEF_IMAGE)
    p.add_argument("-m", "--model_file", default=DEF_MODEL)
    p.add_argument("-l", "--label_file", default=DEF_LABELS)
    p.add_argument("-e", "--ext_delegate", default=DEF_DELEGATE)
    p.add_argument("-d", "--duration", type=float, default=20.0,
                   help="sekundi po bloku (default 20)")
    p.add_argument("-r", "--repeat", type=int, default=3,
                   help="broj blokova, za varijansu (default 3)")
    p.add_argument("-w", "--warmup", type=int, default=10)
    p.add_argument("--threads", type=int, default=1,
                   help="tflite num_threads (default 1 — OBAVEZNO za CPU/NPU podelu)")
    p.add_argument("--pin", type=int, default=4,
                   help="zakuj proces na jezgro (default 4 = veliki A76; -1 iskljuci)")
    p.add_argument("--deep-every", type=int, default=50,
                   help="na koliko iteracija hesirati i veliki tenzor (default 50)")
    p.add_argument("--max-load", type=float, default=8.0,
                   help="odbij da radis ako je loadavg iznad ovoga (default 8.0 = samo patoloski slucajevi; "
                        "--force zaobilazi). "
                        "Nije nula namerno: proces je zakovan na jedno jezgro sa num_threads=1, "
                        "a NPU je ~85%% vremena i ne mari za CPU guzvu (izmerena varijansa 1.7%% "
                        "i pri loadavg 7). Bitno je da su okolnosti ISTE u baseline-u i OC runu — "
                        "to --compare proverava sam.")
    p.add_argument("--force", action="store_true")
    p.add_argument("--compare", help="putanja do ranijeg JSON rezultata za poredjenje")
    p.add_argument("--clock-ratio", type=float, default=4.5,
                   help="hipoteticni odnos taktova za Amdahl predikciju (default 900/200)")
    args = p.parse_args()

    # ---------------- kapija okruzenja ----------------
    pre = snapshot()
    print("=" * 68)
    print(f"NPU BENCH v2  |  label: {args.label}")
    print(f"kernel: {platform.release()}   {datetime.now().isoformat(timespec='seconds')}")
    print("=" * 68)

    load1 = pre["loadavg"][0]
    if load1 > args.max_load and not args.force:
        print(f"\n!! ODBIJAM: loadavg[1min] = {load1:.2f} > {args.max_load}")
        print("   Ovoliko opterecenje je patolosko cak i za zakovan, jednonitni proces.")
        print("   Sacekaj da se smiri, pa ponovi. (Ako stvarno hoces, dodaj --force.)")
        return 2
    if load1 > 2.0:
        print(f"\n   PAZNJA: loadavg[1min] = {load1:.2f} — merenje je i dalje upotrebljivo")
        print("   (proces je zakovan na jedno jezgro, NPU je ~85% vremena), ali OBAVEZNO")
        print("   uporedi sa runom pod slicnim opterecenjem. --compare to sam proverava.")

    govs = {c: v["governor"] for c, v in pre["cpufreq"].items()}
    if len(set(govs.values())) > 1 or "performance" not in set(govs.values()):
        print(f"\n   NAPOMENA: cpufreq governori = {govs}")
        print("   Za najmanju varijansu (opciono, sudo, vraca se pri rebootu):")
        print("     echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor")
        print("   Nije obavezno — bitno je da su ISTI u baseline-u i u OC runu.")

    if args.pin >= 0:
        try:
            os.sched_setaffinity(0, {args.pin})
            print(f"   proces zakovan na cpu{args.pin}")
        except Exception as e:
            print(f"   (zakivanje na cpu{args.pin} nije uspelo: {e})")

    print(f"   loadavg {pre['loadavg']}   vdd_npu_s0 {pre['vdd_npu_s0_uv']} uV   "
          f"npu {_temp(pre['thermal_c'], 'npu'):.1f} C")
    if pre["devfreq"]:
        print(f"   devfreq: {list(pre['devfreq'])}")

    # ---------------- ucitavanje ----------------
    print("\n-- ucitavanje modela + delegate --")
    delegate = [tflite.load_delegate(args.ext_delegate, {})]
    interp = tflite.Interpreter(model_path=args.model_file,
                                experimental_delegates=delegate,
                                num_threads=args.threads)
    interp.allocate_tensors()
    in_d = interp.get_input_details()[0]
    out_d = interp.get_output_details()[0]

    oracle, osizes, onames = derive_oracle(interp, in_d["index"])
    if not oracle:
        print("!! Nijedan tenzor nije prosao orakl. Prekidam — merenje bez orakla nema smisla.")
        return 3
    deep_idx = oracle[0]          # najveci deterministicki = izlaz NPU podgrafa
    fast_idx = [i for i in oracle if osizes[i] <= 4096] or [oracle[-1]]

    # prava slika za merenje
    h, w = int(in_d["shape"][1]), int(in_d["shape"][2])
    img = np.array(Image.open(args.image).resize((w, h))).astype(in_d["dtype"])
    interp.set_tensor(in_d["index"], np.expand_dims(img, axis=0))

    for _ in range(args.warmup):
        interp.invoke()

    ref = {i: sha(interp.get_tensor(i)) for i in oracle}
    print(f"  referentni hesevi uzeti posle {args.warmup} warmup invoke-ova")

    # ---------------- merenje ----------------
    blocks = []
    corruption = []
    for b in range(args.repeat):
        lat = []
        deep_checks = 0
        ru0 = resource.getrusage(resource.RUSAGE_SELF)
        irq0 = _npu_irqs()
        t_invoke = 0.0
        t0 = time.perf_counter()
        t_end = t0 + args.duration
        it = 0
        while time.perf_counter() < t_end:
            a = time.perf_counter()
            interp.invoke()
            dt = time.perf_counter() - a
            t_invoke += dt
            lat.append(dt * 1000.0)
            it += 1
            # jeftin orakl svaku iteraciju (~1 KB sha256 = mikrosekunde)
            for i in fast_idx:
                hv = sha(interp.get_tensor(i))
                if hv != ref[i]:
                    corruption.append({"block": b, "iter": it, "tensor": i,
                                       "name": onames[i], "got": hv, "want": ref[i]})
            if it % args.deep_every == 0:
                deep_checks += 1
                hv = sha(interp.get_tensor(deep_idx))
                if hv != ref[deep_idx]:
                    corruption.append({"block": b, "iter": it, "tensor": deep_idx,
                                       "name": onames[deep_idx], "got": hv,
                                       "want": ref[deep_idx]})
        wall = time.perf_counter() - t0
        ru1 = resource.getrusage(resource.RUSAGE_SELF)
        irq1 = _npu_irqs()

        cpu = (ru1.ru_utime - ru0.ru_utime) + (ru1.ru_stime - ru0.ru_stime)
        n = len(lat)
        d_irq = {k: irq1.get(k, 0) - irq0.get(k, 0) for k in irq1}
        lat_s = sorted(lat)
        blocks.append({
            "iterations": n,
            "wall_s": wall,
            "invoke_s": t_invoke,
            "cpu_s": cpu,
            "harness_overhead_s": wall - t_invoke,
            "throughput_wall": n / wall,
            "throughput_invoke": n / t_invoke,
            "cpu_fraction": cpu / wall,
            "lat_ms": {
                "avg": statistics.mean(lat),
                "min": lat_s[0],
                "med": statistics.median(lat),
                "p95": lat_s[min(int(n * 0.95), n - 1)],
                "max": lat_s[-1],
                "stdev": statistics.pstdev(lat),
            },
            "npu_irq_delta": d_irq,
            "irq_per_inference": {k: v / n for k, v in d_irq.items()},
            "deep_checks": deep_checks,
        })
        print(f"  blok {b+1}/{args.repeat}: {n / wall:7.2f} inf/s   "
              f"lat avg {statistics.mean(lat):6.2f} ms   "
              f"CPU udeo {100*cpu/wall:5.1f}%   deep-check {deep_checks}×")

    post = snapshot()

    # ---------------- analiza ----------------
    tps = [b["throughput_wall"] for b in blocks]
    tot_wall = sum(b["wall_s"] for b in blocks)
    tot_cpu = sum(b["cpu_s"] for b in blocks)
    tot_it = sum(b["iterations"] for b in blocks)
    p_cpu = tot_cpu / tot_wall
    k = args.clock_ratio
    # Amdahl: NPU deo se skalira sa taktom, CPU deo ne
    pred_speedup = 1.0 / (p_cpu + (1 - p_cpu) / k)
    ceiling = 1.0 / p_cpu if p_cpu > 0 else float("inf")

    res = np.squeeze(interp.get_tensor(out_d["index"]))
    with open(args.label_file) as f:
        labels = [x.strip() for x in f]
    top = int(res.argsort()[-1])

    print("\n" + "=" * 68)
    print("REZULTAT")
    print("=" * 68)
    print(f"  throughput   : {statistics.mean(tps):.2f} inf/s   "
          f"(blokovi: {', '.join(f'{t:.1f}' for t in tps)}; "
          f"raspon {100*(max(tps)-min(tps))/statistics.mean(tps):.1f}%)")
    print(f"  iteracija    : {tot_it} u {tot_wall:.1f} s")
    print(f"  top-1        : {res[top]/255.0:.3f}  {labels[top]}")
    print(f"  ORAKL        : {'PROSAO — bit-exact' if not corruption else f'PAO! {len(corruption)} neslaganja'}")
    if corruption:
        for c in corruption[:5]:
            print(f"     blok {c['block']} iter {c['iter']} tenzor {c['tensor']} "
                  f"({c['name'][:30]}): {c['got'][:16]} != {c['want'][:16]}")
    print()
    print("  -- podela vremena (ovo je glavni razlog za v2) --")
    print(f"  CPU vreme    : {p_cpu*100:.1f}%  ({tot_cpu:.1f} s od {tot_wall:.1f} s)")
    print(f"  NPU cekanje  : {(1-p_cpu)*100:.1f}%")
    print(f"  Amdahl plafon: {ceiling:.2f}×  (i sa beskonacno brzim NPU-om)")
    print(f"  predikcija @ {k:g}× takta: {pred_speedup:.2f}×  "
          f"= {statistics.mean(tps)*pred_speedup:.1f} inf/s")
    print("     (pretpostavka: NPU deo skalira linearno sa taktom i memorija nije usko grlo)")
    print()
    ipi = blocks[0]["irq_per_inference"]
    print(f"  IRQ/inferenci: {', '.join(f'{k2}={v:.2f}' for k2, v in ipi.items())}")
    print("     (klok-nezavisna invarijanta — MORA ostati isto posle OC-a)")
    print(f"  npu temp     : {_temp(pre['thermal_c'], 'npu'):.1f} -> "
          f"{_temp(post['thermal_c'], 'npu'):.1f} C")
    print(f"  vdd_npu_s0   : {pre['vdd_npu_s0_uv']} -> {post['vdd_npu_s0_uv']} uV")
    if _temp(post["thermal_c"], "npu") > 85:
        print("  !! NPU preko 85 C — prekini eksperiment (nema throttlinga, samo 115 C critical)")

    # ---------------- snimanje ----------------
    os.makedirs(RESULTS_DIR, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    path = f"{RESULTS_DIR}/{stamp}-{args.label}.json"
    payload = {
        "label": args.label,
        "timestamp": datetime.now().isoformat(),
        "kernel": platform.release(),
        "args": vars(args),
        "oracle": {"tensors": oracle, "sizes": osizes, "names": onames, "reference_sha256": ref},
        "env_pre": pre,
        "env_post": post,
        "blocks": blocks,
        "summary": {
            "throughput_mean": statistics.mean(tps),
            "throughput_blocks": tps,
            "spread_pct": 100 * (max(tps) - min(tps)) / statistics.mean(tps),
            "iterations": tot_it,
            "cpu_fraction": p_cpu,
            "amdahl_ceiling": ceiling,
            "clock_ratio_assumed": k,
            "predicted_speedup": pred_speedup,
            "top1_label": labels[top],
            "top1_score": float(res[top] / 255.0),
            "oracle_pass": not corruption,
            "corruption": corruption[:50],
        },
    }
    with open(path, "w") as f:
        json.dump(payload, f, indent=2)
    print(f"\n  snimljeno    : {path}")

    # ---------------- poredjenje ----------------
    if args.compare:
        with open(args.compare) as f:
            old = json.load(f)
        o, nw = old["summary"], payload["summary"]
        sp = nw["throughput_mean"] / o["throughput_mean"]
        print("\n" + "=" * 68)
        print(f"POREDJENJE sa: {old['label']}")
        print("=" * 68)
        print(f"  {o['throughput_mean']:.2f} -> {nw['throughput_mean']:.2f} inf/s   "
              f"= {sp:.2f}× ubrzanje")
        print(f"  predikcija je bila {o['predicted_speedup']:.2f}× "
              f"(uz {o['clock_ratio_assumed']:g}× takta)")
        print(f"  CPU udeo: {o['cpu_fraction']*100:.1f}% -> {nw['cpu_fraction']*100:.1f}%")
        oi = old["blocks"][0]["irq_per_inference"]
        ni = payload["blocks"][0]["irq_per_inference"]
        # IRQ BROJEVI se dodeljuju dinamicki i menjaju se izmedju bootova —
        # invarijanta je SKUP vrednosti po inferenci (npr. 41/1/0), ne broj IRQ-a
        ov = sorted(oi.values(), reverse=True)
        nv = sorted(ni.values(), reverse=True)
        same = len(ov) == len(nv) and all(abs(a - b) < 0.05 for a, b in zip(ov, nv))
        print(f"  IRQ/inferenci isti: {'DA ✓' if same else 'NE ✗ — kolicina posla se promenila, poredjenje NE VAZI'}")

        # okolnosti moraju biti UPOREDIVE — nije bitno da su nula, bitno je da su iste
        ol, nl = old["env_pre"]["loadavg"][0], pre["loadavg"][0]
        if abs(ol - nl) > max(1.0, 0.5 * max(ol, nl)):
            print(f"  !! loadavg se bitno razlikuje: {ol:.2f} (baseline) -> {nl:.2f} (sad)")
            print("     Poredjenje je oslabljeno. Ponovi oba runa pod slicnim opterecenjem.")
        else:
            print(f"  loadavg uporediv: {ol:.2f} -> {nl:.2f} ✓")
        og = {c: v["governor"] for c, v in old["env_pre"]["cpufreq"].items()}
        ng = {c: v["governor"] for c, v in pre["cpufreq"].items()}
        if og != ng:
            print(f"  !! cpufreq governori se razlikuju: {og} -> {ng}")
        if old["args"].get("pin") != args.pin or old["args"].get("threads") != args.threads:
            print(f"  !! pin/threads se razlikuju: pin {old['args'].get('pin')}->{args.pin}, "
                  f"threads {old['args'].get('threads')}->{args.threads} — poredjenje NE VAZI")
        # JSON pretvara int kljuceve u string -> normalizuj pre poredjenja,
        # inace se poredjenje UVEK razlikuje i daje lazan alarm
        oh = {str(k): v for k, v in old["oracle"]["reference_sha256"].items()}
        nh = {str(k): v for k, v in payload["oracle"]["reference_sha256"].items()}
        common = set(oh) & set(nh)
        diff = [k for k in common if oh[k] != nh[k]]
        if not common:
            print("  ?? nema zajednickih orakl tenzora izmedju runova — proveri model/delegate")
        elif diff:
            print(f"  !! RACUN NIJE ISTI: tenzori {sorted(diff)} imaju drugaciji sha256 nego u baseline-u")
            print("     -> takt je previsok ili je nesto pokvareno. Vrati na nizi takt.")
        else:
            print(f"  bit-exact isti rezultat kao baseline ✓ ({len(common)} tenzora)")
    return 0 if not corruption else 1


if __name__ == "__main__":
    sys.exit(main())
