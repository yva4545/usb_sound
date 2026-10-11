# Passive reconstruction filtering for the PWM audio outputs

## Requirements and assumptions

- Stereo outputs: PA8 / TIM1_CH1 (left), PA9 / TIM1_CH2 (right).
- PWM carrier: **192 kHz**, 375 timer ticks per carrier period.
- USB PCM: **48 kHz**, S16_LE. The firmware converts to 10-bit targets and
  repeats each target for four PWM periods with fractional-duty error diffusion.
- Use passive components only, with a **maximum third-order reconstruction
  low-pass**. A separate DC-blocking capacitor adds a sub-audio high-pass pole.
- Target the **wanted output fundamental**, relative to 1 kHz: within approximately
  **1 dB through 16 kHz**, accepting approximately **5–6 dB loss at 20 kHz**.
- The receiving device is a **line input of at least 10 kΩ**, referenced to ground.
  This circuit is not sized for low-impedance headphones or a loudspeaker.

The script's default acceptance test is slightly stronger: ±1 dB across
20 Hz–16 kHz, and **no more than 6 dB attenuation** at 20 kHz. Less edge attenuation
is acceptable; 5 dB is not enforced as a minimum. All figures below are calculated,
not hardware measurements.

## Why the sample-hold response matters

The current firmware holds each 48 kHz PCM target over four carrier periods;
it does not interpolate the PCM waveform to a new 192 kHz sample stream.
The wanted-signal approximation therefore includes the zero-order-hold response:

```text
H_hold(f) = sinc(f / Fs) × exp(-j π f / Fs)
sinc(x) = sin(π x) / (π x),  Fs = 48,000 Hz
H_audio(f) ≈ H_analog(f) × H_hold(f)
```

Sample holding alone contributes about **1.65 dB loss at 16 kHz** and
**2.64 dB at 20 kHz**. A flat passive analog passband would already miss the
complete-output 16 kHz requirement. The recommended LC network deliberately
has modest analog peaking around 14–15 kHz to offset this loss.

This is still passive: the peaking recovers some low-frequency insertion loss.
The actual analog gain in the passband stays below unity for the nominal circuit.
Three ordinary RC stages roll off too gradually to produce this compensated knee.

## Recommended design using available 4.7 mH inductors

Build a separate, identical chain for each output pin:

```text
             Rs          L1                 L2             Cout
PA8/PA9 ─── 91Ω ─────── 4.7mH ──●───────── 4.7mH ──●───── 4.7µF ── OUT
                                │                  │                  │
                             C1 32nF            Rt 560Ω          Rb 100kΩ
                                │                  │                  │
                               GND                GND                GND
```

The receiving input connects between OUT and GND. **Rt is before Cout** and
must remain present: it damps and terminates the ladder even when the receiver
is unplugged. Rb is after Cout and gives the output a DC reference.

| Component | Value | Selection |
|---|---:|---|
| Rs | **91 Ω** | 1%, 1/8 W or greater |
| Rt | **560 Ω** | 1%, 1/8 W or greater |
| L1, L2 | **4.7 mH each** | Prefer measured/selected values within ±2% |
| C1 | **32 nF** | **22 nF + 10 nF in parallel**, preferably 1% C0G/NP0 or film |
| Cout | 4.7 µF | Film or bipolar electrolytic |
| Rb | 100 kΩ | Output bleed/reference resistor |

The parallel capacitors share the same node and behave as one capacitance, so
the low-pass remains third order. Cout may be omitted if the receiver already
provides suitable AC coupling. This changes DC behavior and the low-frequency
response; represent it with `--coupling-uf 0` when analyzing the circuit.

Use **32 nF**, rather than rounding to 33 nF. With the other values unchanged,
33 nF moves the nominal 20 kHz output from about −5.43 dB to −5.87 dB, leaving
less tolerance margin against the 6 dB budget. Its nominal case passes, but the
documented corner sweep reaches approximately −6.32 dB and fails; the 33 nF
`--check` example below is expected to return exit status 1.

### Nominal calculated performance

Assumptions: 40 Ω GPIO output resistance, 10 Ω winding resistance per inductor,
10 kΩ receiving input, and the shown coupling/bleed components. The 100 kΩ
bleed is included in parallel with the receiver: the effective post-coupling
load is approximately 9.09 kΩ.

| Frequency | Analog gain relative to 1 kHz | Audio + sample hold, relative to 1 kHz |
|---:|---:|---:|
| 1 kHz | 0 dB | 0 dB |
| 10 kHz | +0.63 dB | +0.01 dB |
| 14 kHz | +1.51 dB | +0.27 dB |
| **16 kHz** | **+1.24 dB** | **−0.40 dB** |
| 18 kHz | −0.29 dB | −2.39 dB |
| **20 kHz** | **−2.80 dB** | **−5.43 dB** |
| 96 kHz | −47.03 dB | Not a baseband audio calculation |
| 172 kHz | −62.33 dB | Not a baseband audio calculation |
| **192 kHz** | **−65.21 dB** | Not a baseband audio calculation |

- Nominal low-frequency full-scale sine: approximately **0.906 V RMS** at 1 kHz.
- Idle DC before Cout: approximately **1.30 V**; OUT is centered near zero after
  coupling. The MCU pad's average idle source voltage is 1.65 V; the termination
  divider explains the lower filter-node voltage.
- Approximate 192 kHz fundamental remaining at OUT for ideal 3.3 V, 50%-duty
  PWM: **0.897 mV peak**. This is only that harmonic, not total waveform ripple.

A 512-case endpoint sweep with independent ±2% inductance, ±1% capacitance and
resistor values, GPIO resistance 20–60 Ω, each coil's DCR 6–16 Ω, and receiver
resistance from 10 kΩ to open predicted:

- Band extrema of approximately **−0.867 to +0.892 dB** through 16 kHz.
- 20 kHz output approximately **−5.903 to −4.968 dB**.

These sampled corners support the design choice but are not a proof of a
continuous worst case. Unselected ±10% coils may require retuning. Actual coil
DCR, AC losses, and inductance at operating frequency should be checked.

## Other designs considered

| Script preset | Low-pass elements | Rs / Rt | Nominal coil DCR | Purpose |
|---|---|---|---|---|
| `lc3-4.7` | 4.7 mH – 32 nF – 4.7 mH | 91 / 560 Ω | 10 Ω each | Recommended available-value design |
| `lc3-6.8` | 6.8 mH – 22 nF – 6.8 mH | 150 / 820 Ω | 15.2 Ω each | Earlier compensated three-pole design |
| `lc5` | 1.8 mH – 22 nF – 6.2 mH – 22 nF – 1.8 mH | 470 / 470 Ω | Zero, ideal windings | Earlier approximately 25 kHz Butterworth design |
| `rc3` | Three unbuffered 1 kΩ / 1 nF RC stages | 0 / open | Not applicable | RC comparison baseline, not a recommendation |

The five-pole circuit gives approximately **89 dB carrier rejection**, versus
approximately **65 dB** for the third-order circuits. It has higher insertion
loss and, without compensation, misses the complete-output 16 kHz droop target.
The script applies 40 Ω GPIO resistance by default to all presets; reproduce
the original ideal-source five-pole calculation with `--design lc5 --gpio-ohm 0`.

Available 3.3 mH and 10 mH inductors can also be used after retuning the capacitor
and terminations. Arbitrary substitutions into the 4.7 mH circuit do not preserve
its response. The script supports unequal input/output inductances; their order
matters because the source and load terminations are unequal.

## Reconstruction limits and component/layout considerations

- **Carrier rejection and sampling-image rejection are different.** A 20 kHz
  tone has a first image at 48−20 = **28 kHz**. For the recommended circuit it is
  approximately **13.17 dB below the wanted tone**, despite strong rejection of
  the 192 kHz carrier. This design accepts significant ultrasonic image content.
- For a held PCM tone, the first-image/wanted-amplitude ratio is approximately
  `f / (Fs − f) × |H_analog(Fs − f) / H_analog(f)|`.
- **Never multiply the PWM carrier by the PCM sinc response** to claim extra
  rejection. The sinc zeros at multiples of 48 kHz describe held PCM spectra,
  not the independently generated switching waveform.
- Prefer low-DCR inductors rated for at least 20 mA, with self-resonance
  **≥500 kHz** where available. A part specified only by its nominal mH value
  is insufficient to predict high-frequency behavior. Ferrite/core and winding
  losses may differ from the simple DCR model.
- C0G/NP0 or film capacitors provide stable capacitance and low voltage-dependent
  distortion. Resistor termination values are part of the filter design.
- Keep the inductors magnetically uncoupled: separation, orthogonal axes, or
  shielded parts help. Mutual coupling changes the response and may bypass the
  intended carrier attenuation.
- Keep unfiltered PWM wiring away from OUT and the PA1/PA3 analog inputs. Use
  short capacitor-ground returns and separate left/right filter chains.
- Ten-bit digital targets do not establish ten-bit analog ENOB. Supply noise,
  GPIO high/low asymmetry, edge timing, PWM modulation, quantization/noise shaping,
  magnetic coupling, and parasitic feedthrough need hardware characterization.
- Measure the **wanted-tone fundamental** with FFT/selective measurements;
  broadband RMS also includes sampling images and switching residuals.

## Python analysis tool

`filter_analysis.py` requires Python 3.10+ and NumPy. Matplotlib is optional for
plots. On Debian:

```sh
sudo apt install python3-numpy
# Optional:
sudo apt install python3-matplotlib
```

Run commands from the project root:

```sh
# Recommended nominal circuit; exit 1 if its response fails the configured limits.
python3 filter_analysis.py --check

# Compare all presets. FAIL on a comparison baseline is informative here.
python3 filter_analysis.py --design all

# Reproduce the documented 512 endpoint corners, including independently varied DCR.
python3 filter_analysis.py --corners --dcr-range 6 16 --check

# Reproducible sampling of the same ranges; this is not a worst-case proof.
python3 filter_analysis.py --monte-carlo 1000 --seed 42 --dcr-range 6 16 --check
```

Export nominal response data and plots into an existing directory. The firmware
build creates `build/`; create it first if necessary:

```sh
mkdir -p build
python3 filter_analysis.py --design all \
  --csv build/filter-response.csv --plot build/filter-response.svg
```

CSV contains analog absolute gain, analog gain relative to the reference, and
audio-plus-hold relative gain. The audio column is blank at/above Nyquist, since
there it should not be interpreted as wanted baseband audio. Plots and CSV are
nominal curves; sensitivity extrema are printed separately.

### Change components, load, or parasitics

```sh
# Quantify the effect of using 33 nF instead of 32 nF.
python3 filter_analysis.py --caps-nf 33 --corners --dcr-range 6 16 --check

# Replace estimated winding resistance with measurements.
python3 filter_analysis.py --coil-dcr-ohm 9.2 11.0 --load-ohm 20000 --check

# Approximate winding capacitance from each coil's SRF, in kHz.
python3 filter_analysis.py --coil-srf-khz 500 500

# Custom third-order design using other available inductors.
python3 filter_analysis.py --inductors-mh 3.3 10 --caps-nf 33 \
  --series-ohm 140 --termination-ohm 1000 --coil-dcr-ohm 8 20 --check

# Custom unbuffered RC ladder; inf omits the local termination resistor.
python3 filter_analysis.py --series-resistors-ohm 330 1000 3300 \
  --caps-nf 15 4.7 1.5 --series-ohm 0 --termination-ohm inf

python3 filter_analysis.py --help
```

`--check` returns **0** for passing selected nominal/sweep cases, **1** for a
response failure, and **2** for invalid arguments, dependencies, or output-path
errors. Normal exploratory runs without `--check` return 0 even when a design
fails the response targets. `--design all --check` will fail because the comparison
circuits do not all meet the compensated audio-band requirement.

The script models the loaded ladder with cascaded ABCD matrices, including
stage interactions, GPIO/source resistance, coil DCR, termination, coupling,
bleed, and receiver resistance. Optional SRF uses a parallel winding-capacitance
approximation `Cp = 1 / ((2π SRF)² L)`; it does not model magnetic coupling or a
manufacturer's complete frequency-dependent loss network. Nominal low-pass
order counts exclude this added parasitic capacitance and the coupling pole.

Sensitivity defaults: L ±2%, C ±1%, R ±1%, DCR ±20% of each nominal winding,
GPIO resistance 0.5–1.5 times nominal, receiver resistance from nominal to open.
`--dcr-range`, `--gpio-range`, and `--load-range` override these environmental
ranges. For an open-ended Monte Carlo load range, receiver **conductance** is
sampled uniformly. SRF, VDD, bleed, and coupling are held fixed in sweeps.
Endpoint sweeps are limited to 8192 cases; use Monte Carlo for larger ladders.

### Verification of the analysis model

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tests -p 'test_filter_analysis.py' -v
```

The tests compare the ABCD implementation with an independent nodal/KCL solver,
check loaded RC and coupling-capacitor closed forms, reproduce an exact fifth-order
Butterworth prototype, verify optional coil parasitics, check the recommended
design/corner sweep, and exercise custom values, validation, and CSV output.
