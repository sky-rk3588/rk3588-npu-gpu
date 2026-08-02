#!/bin/bash
# npu-fast-boot.sh — NPU brzi start posle boota (Igor + Claude, 02.08.2026)
# Radi: swap rocket na devfreq build, sina 850 mV, IRQ->cpu6, cpu-sleep off,
#       NPU simple_ondemand 200-1000, CPU governori performance.
# Bezbednost: ne dira disk/DTB/GRUB; pad bilo kog koraka = sistem ostaje stock.

KO=/home/sky/Downloads/rocket-clks-fix/build/rocket.ko
VOLT=/home/sky/Downloads/npu-volt/npu_volt_probe.ko
DEV=/sys/class/devfreq/fdab0000.npu

log() { echo "npu-fast: $*"; }

# 0) sacekaj da udev ucita in-tree rocket (obicno 1-2 s, max 30)
for i in $(seq 1 30); do
    [ -d /sys/module/rocket ] && break
    sleep 1
done

# 1) vermagic provera — posle promene kernela bez rebuild-a ostajemo na stock
if ! modinfo -F vermagic "$KO" 2>/dev/null | grep -q "^$(uname -r) "; then
    log "vermagic se ne slaze sa $(uname -r) — ostajem na stock rocketu"
    exit 1
fi

# 2) swap modula; ako nas insmod pukne, vrati stock da /dev/accel postoji
rmmod rocket 2>/dev/null
if ! insmod "$KO"; then
    log "insmod naseg rocket.ko PAO — vracam stock modul"
    modprobe rocket
    exit 1
fi
log "rocket devfreq ucitan (srcversion $(cat /sys/module/rocket/srcversion))"

# 3) sina na 850 mV (vendor plafon; naponska blokada u modulu i dalje cuva)
rmmod npu_volt_probe 2>/dev/null
if insmod "$VOLT" uv=850000; then
    log "vdd_npu_s0 -> 850 mV"
else
    log "npu_volt PAO — sina ostaje 800 mV (guard tada pusta do 700 MHz)"
fi

# 4) IRQ na cpu6 (A76) — broj linije se menja po bootu, trazi se PO IMENU
IRQ=$(grep fdab0000.npu /proc/interrupts | cut -d: -f1 | tr -d ' ')
if [ -n "$IRQ" ]; then
    echo 6 > "/proc/irq/$IRQ/smp_affinity_list" && log "IRQ $IRQ -> cpu6"
else
    log "UPOZORENJE: ne nalazim NPU IRQ liniju"
fi

# 5) cpu-sleep (state1) off na cpu6 — kazna budjenja je 220 us
echo 1 > /sys/devices/system/cpu/cpu6/cpuidle/state1/disable \
    && log "cpu6 cpu-sleep OFF"

# 6) NPU automatika: simple_ondemand 200-1000
#    (za zakucan maksimum: echo performance > governor — rucno, po zelji)
echo 200000000  > "$DEV/min_freq"
echo 1000000000 > "$DEV/max_freq"
echo simple_ondemand > "$DEV/governor" \
    && log "NPU governor simple_ondemand (200-1000 MHz)"

# 7) CPU governori na performance (Igorov izbor; izmereno +23% na NPU)
for p in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor; do
    echo performance > "$p"
done
log "CPU governori -> performance"

exit 0
