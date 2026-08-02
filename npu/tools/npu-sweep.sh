#!/bin/bash
# ============================================================================
# npu-sweep.sh — kompletna slika NPU-a kroz sve frekvencije, u jednom potezu.
#
# Na SVAKOM stepeniku meri ISTOVREMENO:
#   - throughput / NPU deo / orakl  (npu-bench2.py)
#   - lambda: rad i cekanje IRQ niti po prekidu  (/proc/<tid>/schedstat)
# pa na kraju ispisuje tabelu i CSV.
#
# Zasto oba odjednom: lambda je CPU strana i sakrivena je unutar onoga sto
# bench zove "NPU cekanje" (getrusage meri samo bench proces, ne IRQ nit).
# Bez istovremenog merenja ne moze se izracunati koliki je njen udeo.
#
# Trazi: ucitan rocket sa devfreq-om i aktiviran venv. Governor je SVEJEDNO —
# pinujemo kroz min_freq/max_freq, sto radi i pod performance i pod ondemand.
# sudo se koristi SAMO za upis u min_freq/max_freq.
#
# Usage:  ./npu-sweep.sh [lista frekvencija u MHz]
#         ./npu-sweep.sh                 # default 200 300 400 500 600 700
#         ./npu-sweep.sh 200 600         # samo dva stepenika
# ============================================================================
set -u

DEV=/sys/class/devfreq/fdab0000.npu
RES=~/npu-bench-results
OUT=~/Downloads/npu-sweep
STEPS=("$@")
[ "${#STEPS[@]}" -eq 0 ] && STEPS=(200 300 400 500 600 700)

[ -d "$DEV" ] || { echo "GRESKA: nema $DEV — je li rocket ucitan?"; exit 1; }
GOV=$(cat $DEV/governor)
command -v python >/dev/null || { echo "GRESKA: nema python — aktiviraj venv"; exit 1; }
python -c "import tflite_runtime" 2>/dev/null || { echo "GRESKA: venv nije aktiviran (nema tflite_runtime)"; exit 1; }

FLOOR=$(tr " " "\n" < "$DEV/available_frequencies" | grep -E "^[0-9]+$" | sort -n | head -1)
CEILING=$(tr " " "\n" < "$DEV/available_frequencies" | grep -E "^[0-9]+$" | sort -n | tail -1)

mkdir -p "$OUT"
STAMP=$(date +%Y%m%d-%H%M%S)
CSV="$OUT/sweep-$STAMP.csv"

# IRQ niti — trazi po imenu, brojevi se menjaju pri svakom insmod-u
mapfile -t TIDS < <(ps -eLo tid,comm --no-headers | awk '/irq\/.*fda[bcd]0000/ {print $1}')
[ "${#TIDS[@]}" -gt 0 ] || { echo "GRESKA: ne nalazim IRQ niti NPU-a"; exit 1; }
echo "IRQ niti: ${TIDS[*]}"

snap() { for t in "${TIDS[@]}"; do read -r c w s < /proc/$t/schedstat; echo "$t $c $w $s"; done; }

echo "freq_mhz,inf_s,ms_inf,cpu_ms,npu_ms,us_chunk,budjenja,lambda_rad_us,lambda_cek_us,lambda_ms_inf,lambda_udeo_pct,temp_c" > "$CSV"

echo
echo "governor : $GOV   (pinujemo kroz min_freq/max_freq, radi pod svakim)"
echo "stepenici: ${STEPS[*]} MHz   (~80 s po stepeniku)"
echo

for F in "${STEPS[@]}"; do
	HZ=$((F * 1000000))
	echo "======== $F MHz ========"
	# Pinovanje kroz min_freq/max_freq radi pod SVAKIM governorom
	# (userspace/set_freq postoji samo pod userspace-om).
	# Redosled je bitan: prvo spusti min na pod, pa pomeri max, pa digni min —
	# tako min nikad ne prelazi max ni u jednom trenutku.
	echo "$FLOOR" | sudo tee "$DEV/min_freq" >/dev/null
	if ! echo "$HZ" | sudo tee "$DEV/max_freq" >/dev/null 2>&1; then
		echo "  ODBIJENO (max_freq) na $F MHz — preskacem"; continue
	fi
	if ! echo "$HZ" | sudo tee "$DEV/min_freq" >/dev/null 2>&1; then
		echo "  ODBIJENO (min_freq) na $F MHz — preskacem"; continue
	fi
	sleep 2
	CUR=$(cat $DEV/cur_freq)
	if [ "$CUR" != "$HZ" ]; then
		echo "  cur_freq=$CUR != $HZ — preskacem"
		continue
	fi

	B=$(snap)
	python ~/npu-bench2.py --label "sweep-${F}mhz" >/dev/null 2>&1 || { echo "  bench pukao"; continue; }
	A=$(snap)

	# saberi delte preko svih jezgara
	TC=0; TW=0; TS=0
	while read -r t c w s; do
		set -- $(echo "$B" | awk -v T="$t" '$1==T {print $2" "$3" "$4}')
		TC=$((TC + c - $1)); TW=$((TW + w - $2)); TS=$((TS + s - $3))
	done <<< "$A"

	J=$(ls -t $RES/*sweep-${F}mhz.json 2>/dev/null | head -1)
	[ -n "$J" ] || { echo "  nema JSON-a"; continue; }

	read -r INF MS CPUMS NPUMS USCH TEMP < <(python3 - "$J" <<'PY'
import json,sys
d=json.load(open(sys.argv[1]))
tp=d['summary']['throughput_mean']
bl=d['blocks']; cf=sum(b['cpu_fraction'] for b in bl)/len(bl)
ms=1000/tp; cpu=ms*cf; npu=ms-cpu
t=d.get('env_post',{}).get('thermal_c',{}).get('npu-thermal',0)
print(f"{tp:.2f} {ms:.4f} {cpu:.4f} {npu:.4f} {npu*1000/41:.2f} {t}")
PY
)
	LRAD=$(echo "$TC/$TS/1000" | bc -l)
	LCEK=$(echo "$TW/$TS/1000" | bc -l)
	LMS=$(echo "($TC+$TW)/1000000/($TS/42.0)" | bc -l)   # ~42 prekida po inferenci (41+1)
	LPCT=$(echo "100*$LMS/$NPUMS" | bc -l)

	printf "  %.2f inf/s | NPU deo %.3f ms | lambda rad %.2f us cek %.2f us | udeo %.1f%%\n" \
		"$INF" "$NPUMS" "$LRAD" "$LCEK" "$LPCT"
	printf "%s,%s,%s,%s,%s,%s,%s,%.2f,%.2f,%.4f,%.1f,%s\n" \
		"$F" "$INF" "$MS" "$CPUMS" "$NPUMS" "$USCH" "$TS" "$LRAD" "$LCEK" "$LMS" "$LPCT" "$TEMP" >> "$CSV"
done

echo
echo "======== KOMPLETNA SLIKA ========"
column -s, -t < "$CSV"
echo
echo "CSV: $CSV"
echo
echo "vracam opseg na $((FLOOR/1000000))-$((CEILING/1000000)) MHz"
echo "$FLOOR" | sudo tee "$DEV/min_freq" >/dev/null
echo "$CEILING" | sudo tee "$DEV/max_freq" >/dev/null
