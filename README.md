# RK3588 NPU DVFS + GPU @ 1 GHz (Orange Pi 5 Plus)

Everything needed to take the RK3588's NPU from its mainline-default
**200 MHz to 1 GHz** with proper DVFS, thermal protection and a voltage
guard — plus the recipe that moves the Mali-G610 GPU from 850 MHz to
**1 GHz** over the supported SCMI/PVTPLL path.

Measured on an Orange Pi 5 Plus (LPDDR4X-2112), kernel 7.0, mainline
`rocket` NPU driver + Teflon (Mesa) TFLite delegate.

**MobileNetV1 throughput, one A76 inference thread, bit-exact oracle:**

| | inf/s | vs. stock |
|---|---|---|
| Mainline default (200 MHz, stock IRQ config) | 68.5 | 1.0× |
| 200 MHz, tuned IRQ/cpuidle config only | 91 | 1.33× |
| Daily driver (simple_ondemand 200–1000, 850 mV) | ~236 | **3.4×** |
| Peak measured (1000 MHz @ 950 mV, bench only) | 245 | 3.58× |

GPU: glmark2-es2-wayland **3121**, vkmark **~4090**, clpeak FP16
**887 GFLOPS** at 1 GHz.

## Status / upstream

The devfreq work is an out-of-tree preview of what is being discussed
upstream — see the RFC and the follow-up with the full measurement
corpus on dri-devel/linux-rockchip:

- RFC: `https://lore.kernel.org/linux-rockchip/20260801131656.58450-1-royalnet026@gmail.com/`
- The clocks-by-name fix (`accel/rocket: request the core clocks by
  name`) is on the list with Reviewed-by and Tested-by tags.

Until that lands, this repo makes any RK3588 board fast **today**.

## Layout

```
npu/driver/      rocket driver + devfreq, out-of-tree buildable module
npu/dt-patches/  #cooling-cells binding, NPU passive thermal trip, opp-v2 binding
npu/volt/        npu_volt_probe.ko — set the NPU rail (cap 950 mV, warns >850)
npu/tools/       npu-bench2.py (bit-exact oracle bench), npu-sweep.sh (freq ladder + λ)
npu/boot/        systemd unit: full fast-NPU setup on every boot
npu/data/        measurement corpus (3 rails × 9 clocks) + clk_summary snapshots
gpu/             Jonas Karlman's 4 GPU clk patches + tested deploy plan
```

## NPU quick start

```sh
# 1. build the driver against your running kernel
cd npu/driver && make -C /lib/modules/$(uname -r)/build M=$PWD modules

# 2. swap it in (survives until reboot; see npu/boot for persistence)
sudo rmmod rocket
sudo insmod ./rocket.ko

# 3. it shows up as a devfreq device
cat /sys/class/devfreq/fdab0000.npu/available_frequencies
echo simple_ondemand | sudo tee /sys/class/devfreq/fdab0000.npu/governor
```

For the full daily setup (module swap + 850 mV rail + IRQ affinity +
cpuidle + governors) install the systemd unit from `npu/boot/` —
**adjust the hardcoded paths first**.

## Things this driver knows that cost us four hard hangs

1. **The NPU power domains cannot be switched on/off while the SCMI
   compute clock is above its boot rate (200 MHz).** The domain
   handshake never acks, genpd is left wedged, and the next MMIO access
   raises an asynchronous SError → panic. The driver hooks
   `dev_pm_genpd_add_notifier()` on all three cores and forces the safe
   rate around every domain transition. Measured, reproducible; the
   handshake-side clocks (`clk_npu_dsu0`, aclk/hclk/pclk) are *not* the
   trigger — they stay parked at boot rates the whole time.
2. **The clock is PVT-controlled.** The rail voltage decides what the
   PLL actually delivers for a given nominal request: 800→950 mV is
   worth +14% at 300 MHz nominal. The driver refuses OPPs whose voltage
   requirement exceeds the current rail (e.g. 1000 MHz needs ≥850 mV).
3. **`clk_summary` lies about SCMI clocks** — it prints the cached
   rate (200 MHz even mid-benchmark at 1000). Use `clk_get_rate()` or
   measure.
4. **Thermals:** mainline gives the NPU only a 115 °C critical trip.
   The DT patches add a passive trip at 85 °C + cooling map, and the
   driver registers a devfreq cooling device — the chain binds
   automatically.

## Benchmarking this hardware honestly

The biggest error sources we quantified, in order:

| source | cost |
|---|---|
| all NPU IRQs landing on a sleeping A55 (stock) | −25% at stock clock |
| a busy desktop session sharing the SoC | up to −18% |
| die temperature (PVT loop) | ~−0.5% / °C |
| an open browser | ~−1% |

The silicon itself reproduces to <1% — five independent runs of the
same point across one day spread 0.24%. `npu-bench2.py` pins the
thread, hashes intermediate tensors (sha256, zero tolerance) and
records the environment; `npu-sweep.sh` adds per-IRQ-thread λ
accounting from `/proc/<tid>/schedstat`.

The plateau above ~700 MHz is the board's memory (LPDDR4X-2112) plus
the voltage effect — an LPDDR5 board should keep scaling. A
compute-dense model (InceptionV1) flattens at the same knee, so it is
the platform, not the model.

## Safety notes

- 850 mV is the vendor's own voltage for the top OPPs — fine for daily
  use. 950 mV is above vendor spec: measurement only.
- The voltage guard and the genpd notifier are not optional decoration;
  removing them reproduces the crash class described above.
- Nothing here touches the DTB, GRUB or `/lib/modules` — remove the
  systemd unit and you are back to stock.

## Credits

- **Tomeu Vizoso** — the mainline `rocket` driver this builds on.
- **Jiaxing Hu** — reviews, the RK3576 perspective, and the push to
  verify the clock tree properly.
- **Jonas Karlman** — the four GPU clk patches in `gpu/patches/`
  (hashes in the filenames).
- **Sidong Yang** — independent testing of the clocks fix on ROCK 5B+.
- **dongioia / rock5bplus-rkvdec2** — the trail to the GPU SCMI path.
- **Diederik de Haas** — the lore breadcrumbs on domain-ack failures.

## License

GPL-2.0 (see `LICENSE`) — the driver is a derivative of the in-kernel
`rocket` driver. Tools and scripts: GPL-2.0 as well.

*Built and measured over many cups of tea in Serbia. Merenje pre
tvrdnje — measure before you claim.*
