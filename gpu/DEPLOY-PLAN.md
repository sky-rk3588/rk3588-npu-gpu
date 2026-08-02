# GPU 850 → 1000 MHz — DEPLOY PLAN (02.08.2026)

**Grana:** `gpu-scmi-1ghz` (Pi: `75c435686`, NucBox: `1311e4028` — isti sadržaj)
**Kernel:** `7.0.0-rk3588-gpu1g+` — POSEBNO ime, stari `hdr-egpu+` ostaje netaknut kao padobran.
**Sadržaj povrh tvog dnevnog kernela:** 4× Karlman (bus clk + SError guard + binding + DT) i
1× lokalni audio commit (hdmiin čvorovi u živom obliku — ALSA ostaje `rockchiphdmiin`).

**ZLATNO PRAVILO:** stari kernel + novi DTB = **CIGLA BEZ PORUKE**.
Novi kernel + stari DTB = bezbedno (ponaša se kao danas). Zato: **kernel PRVO, DTB DRUGO,
i dok je override aktivan NIKAD ne birati stari kernel u GRUB-u.**

---

## FAZA A — kernel (bezbedno, DTB se još ne dira)

**A1.** Instalacija (iz `~/Downloads/gpu-1ghz/`):
```
sudo dpkg -i linux-image-7.0.0-rk3588-gpu1g+_*.deb linux-headers-7.0.0-rk3588-gpu1g+_*.deb
```
Očekivano: postinst vrti update-grub/initramfs bez grešaka; `grep gpu1g /boot/grub/grub.cfg` nađe unos.

**A1b. ⚠️ VP9 P2 živi kao DKMS, ne u stablu!** (`rkvdec-vp9/1.0` u `/usr/src/`, beryllium 7.0.y izvor
sa profilom max=2; stablo ima stock max=0). Headers postinst bi trebalo da ga sam izgradi — PROVERITI:
```
dkms status | grep gpu1g
```
Očekivano: `rkvdec-vp9/1.0, 7.0.0-rk3588-gpu1g+ ... installed` + isto za `v4l2loopback/0.15.3`.
AKO NEMA, ručno:
```
sudo dkms install rkvdec-vp9/1.0 -k 7.0.0-rk3588-gpu1g+
```
```
sudo dkms install v4l2loopback/0.15.3 -k 7.0.0-rk3588-gpu1g+
```

**A2.** Reboot → u GRUB-u **Advanced options** → izabrati `7.0.0-rk3588-gpu1g+`.
(Default NE menjati — default ostaje stari = padobran.)

**A3.** Provere (sve mora biti kao danas, GPU i dalje 850):
```
uname -r                                  # 7.0.0-rk3588-gpu1g+
cat /sys/class/devfreq/fb000000.gpu/governor      # simple_ondemand (panthor default!)
cat /proc/asound/cards | grep hdmiin       # rockchiphdmiin postoji
glmark2-es2-wayland -b build:use-vbo=true  # brojevi ~ kao danas
v4l2-ctl -d /dev/video1 -L | grep -A1 vp9_profile  # max=2 → VP9 P2 preživeo (DKMS)
modinfo rockchip-vdec | grep filename      # .../gpu1g+/updates/dkms/... (ne kernel/)
```
NPU test (modul, Teflon) + HDMI-IN zvuk + eGPU — sve mora raditi pre Faze B.

---

## FAZA B — DTB (rizični korak — UART OBAVEZAN)

**B0.** Prvo pogledati šta je trenutno na ESP-u (očekujemo audio overlay, možda i base):
```
sudo ls -R /boot/efi/dtb
```
→ IZLAZ MI JAVITI pre nastavka. Ako postoji audio overlay (`.dtbo`), skloniti ga
(novi base DTB nosi iste čvorove; overlay preko base-a bez `__symbols__` može da zabode).

**B1.** UART crna kutija (NucBox):
```
ssh user@ip "screen -dmS uart bash -c 'stty -F /dev/ttyUSB0 1500000 raw -echo; cat /dev/ttyUSB0 > ~/opi-uart.log'"
```
Na Pi-ju: `echo 8 | sudo tee /proc/sys/kernel/printk`

**B2.** Novi DTB na ESP:
```
sudo mkdir -p /boot/efi/dtb/base
sudo cp ~/Downloads/gpu-1ghz/rk3588-orangepi-5-plus-gpu1g.dtb /boot/efi/dtb/base/rk3588-orangepi-5-plus.dtb
```

**B3.** Reboot → po potrebi u EDK2 setup proveriti da je **Rockchip Platform Configuration →
ACPI / Device Tree → Support DTB override & overlays** UKLJUČENO (čist flash 17.07 je mogao
da resetuje!) → GRUB → **OPET IZABRATI `gpu1g` KERNEL** (ne stari!).

**B4. AKO VISI** (bez slike, bez poruke — očekivani oblik kvara):
1. Struja OFF/ON → odmah u EDK2 setup → isključiti DTB override → boot stari kernel = sve kao pre.
2. UART log na NucBox-u: `tail -50 ~/opi-uart.log` — nosi trag za forenziku.

---

## FAZA C — verifikacija (posle uspešnog boota sa novim DTB-om)

```
xxd /proc/device-tree/gpu@fb000000/clock-names     # core.coregroup.stacks.bus
cat /sys/class/devfreq/fb000000.gpu/governor       # simple_ondemand — NE menjati na performance!
cat /proc/asound/cards | grep hdmiin               # zvuk preživeo
sudo grep -i gpu /sys/kernel/debug/clk/clk_summary # scmi_clk_gpu SAD vozi fb000000.gpu; clk_gpu = "bus"
```
Pod opterećenjem (glmark2 u petlji):
```
watch -n1 cat /sys/class/devfreq/fb000000.gpu/cur_freq   # 1000000000 pod teretom, 200000000 idle
```
Trajni governor (tmpfiles, ne udev):
```
printf 'w- /sys/class/devfreq/fb000000.gpu/governor - - - - simple_ondemand\n' | sudo tee /etc/tmpfiles.d/panthor-devfreq.conf
```
BONUS provera: NPU thermal sad ima cooling-maps → `ls /sys/class/thermal/ | grep cooling` +
`cat /sys/class/thermal/thermal_zone*/type` (npu-thermal dobija pasivni trip na 85 °C).

---

## FAZA D — merenje (kućni stil: A-B-A, back-to-back, ista sesija)

Pin preko devfreq (radi pod bilo kojim governorom, kao npu-sweep):
```
echo 850000000 | sudo tee /sys/class/devfreq/fb000000.gpu/min_freq /sys/class/devfreq/fb000000.gpu/max_freq
```
… glmark2/vkmark … pa isto sa 1000000000, pa NAZAD na 850 (A-B-A).
Očekivanje POŠTENO: čist dobitak serije ~2.6 % na punom glmark2 (memorijski zid);
više na shader-bound scenama. Ne jurimo procente — jurimo podržan put i pravih 1000 MHz.
Posle merenja vratiti opseg: min 200000000 (ne 300 — sad postoji i 200 OPP!), max 1000000000.

## Posle svega
- Reboot bez biranja = stari kernel + novi DTB? NE! Dok je override aktivan, ili
  (a) postaviti GRUB default na gpu1g unos, ili (b) uvek ručno birati. Odlučiti u Fazi C.
- Stara zamka NE VAŽI za novi kernel: novi kernel + STARI DTB radi (dokazano u Fazi A),
  pa je rollback uvek: EDK2 override OFF → bilo koji kernel.
