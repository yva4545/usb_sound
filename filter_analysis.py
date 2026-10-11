#!/usr/bin/env python3
"""Analyze loaded passive reconstruction ladders for the STM32 PWM audio output.

Requires NumPy; Matplotlib is optional for --plot. See Filtering.md and --help.
The sample-hold approximation describes the wanted audio fundamental, not the
actual PWM waveform, quantization/noise shaping, or the GPIO's nonlinear drive.
"""

import argparse
import csv
import itertools
import math
import sys
from dataclasses import dataclass, replace
from pathlib import Path

try:
    import numpy as np
except ImportError as exc:
    print("NumPy is required: sudo apt install python3-numpy", file=sys.stderr)
    raise SystemExit(2) from exc


@dataclass(frozen=True)
class Element:
    # R and L are series elements; C is a shunt capacitor to ground.
    kind: str
    value: float                 # ohms, henries, or farads
    dcr: float = 0.0             # inductor series winding resistance, ohms
    srf: float = 0.0             # optional inductor self-resonance, Hz


@dataclass(frozen=True)
class Design:
    name: str
    series_r: float
    termination_r: float
    elements: tuple[Element, ...]


@dataclass(frozen=True)
class Settings:
    gpio_r: float = 40.0
    receiver_r: float = 10000.0
    bleed_r: float = 100000.0
    coupling_f: float = 4.7e-6
    sample_hz: float = 48000.0
    pwm_hz: float = 192000.0
    vdd: float = 3.3


def lc_design(name, inductors, capacitors, series_r, termination_r, dcr=None, srf=None):
    """Construct an alternating series-L / shunt-C ladder, ending in L."""
    if len(inductors) != len(capacitors) + 1 or not inductors:
        raise ValueError("LC ladders require one more inductor than shunt capacitors")
    dcr = [0.0] * len(inductors) if dcr is None else dcr
    srf = [0.0] * len(inductors) if srf is None else srf
    if len(dcr) != len(inductors) or len(srf) != len(inductors):
        raise ValueError("DCR and SRF lists must have one entry per inductor")
    elements = []
    for index, value in enumerate(inductors):
        elements.append(Element("L", value, dcr[index], srf[index]))
        if index < len(capacitors):
            elements.append(Element("C", capacitors[index]))
    return Design(name, series_r, termination_r, tuple(elements))


PRESETS = {
    "lc3-4.7": lc_design("lc3-4.7", [4.7e-3, 4.7e-3], [32e-9],
                          91.0, 560.0, [10.0, 10.0]),
    "lc3-6.8": lc_design("lc3-6.8", [6.8e-3, 6.8e-3], [22e-9],
                          150.0, 820.0, [15.2, 15.2]),
    # Ideal winding losses for the original five-pole component-value study.
    "lc5": lc_design("lc5", [1.8e-3, 6.2e-3, 1.8e-3], [22e-9, 22e-9],
                      470.0, 470.0),
    # An unbuffered RC baseline; stage loading is included, not independent poles.
    "rc3": Design("rc3", 0.0, math.inf,
                  tuple(element for _ in range(3)
                        for element in (Element("R", 1000.0), Element("C", 1e-9)))),
}


def transfer(design, settings, frequencies):
    """Complex V_OUT / ideal GPIO-source voltage, including load and DC coupling.

    Cascaded ABCD matrices: series Z = [[1,Z],[0,1]], shunt Y = [[1,0],[Y,1]].
    Only their first row is needed for voltage gain: H_pre = 1/(A + B/Z_load).
    No assumption that stages are buffered or independently terminated is made.
    """
    frequencies = np.asarray(frequencies, dtype=float)
    if np.any(~np.isfinite(frequencies)) or np.any(frequencies <= 0):
        raise ValueError("analysis frequencies must be finite and greater than zero")
    s = 2j * np.pi * frequencies
    a = np.ones_like(s)
    b = np.full_like(s, design.series_r + settings.gpio_r)
    open_circuit = np.zeros_like(frequencies, dtype=bool)
    for element in design.elements:
        if element.kind == "C":
            a = a + b * s * element.value
            continue
        if element.kind == "R":
            z = element.value
        elif element.kind == "L":
            z = element.dcr + s * element.value
            if element.srf:
                # Winding capacitance across the series R-L branch. SRF is an
                # approximate lossless resonance parameter, not a complete model.
                cp = 1.0 / ((2 * np.pi * element.srf) ** 2 * element.value)
                admittance = 1.0 / z + s * cp
                opened = np.abs(admittance) < 1e-20
                if element.dcr == 0:
                    opened |= frequencies == element.srf
                open_circuit |= opened
                z = np.divide(1.0, admittance, out=np.zeros_like(s), where=~opened)
        else:
            raise ValueError(f"unknown element kind: {element.kind}")
        b = b + a * z

    receiver_g = 1.0 / settings.receiver_r + 1.0 / settings.bleed_r
    receiver = 1.0 / receiver_g if receiver_g else math.inf
    z_coupling = 1.0 / (s * settings.coupling_f) if settings.coupling_f else 0.0
    load_g = 1.0 / design.termination_r + 1.0 / (receiver + z_coupling)
    coupling_gain = receiver / (receiver + z_coupling) if math.isfinite(receiver) else 1.0
    output = 1.0 / (a + b * load_g) * coupling_gain
    return np.where(open_circuit, 0j, output)


def sample_hold(frequencies, sample_hz):
    """Normalized zero-order-hold transfer: sinc(f/Fs) * exp(-j*pi*f/Fs)."""
    ratio = np.asarray(frequencies, dtype=float) / sample_hz
    return np.sinc(ratio) * np.exp(-1j * np.pi * ratio)


def db(value):
    return 20.0 * np.log10(np.maximum(np.abs(value), 1e-300))


def curves(design, settings, frequencies, reference_hz):
    analog = transfer(design, settings, frequencies)
    reference = transfer(design, settings, reference_hz)
    if not np.isfinite(reference) or abs(reference) < 1e-300:
        raise ValueError("reference-frequency gain must be finite and nonzero")
    analog_relative = db(analog / reference)
    total = db(analog * sample_hold(frequencies, settings.sample_hz) /
               (reference * sample_hold(reference_hz, settings.sample_hz)))
    # Audio baseband approximation is not a model for the PWM carrier. In
    # particular, sinc zeros at 48/96/192 kHz must not be claimed as PWM rejection.
    total = np.where(np.asarray(frequencies) < settings.sample_hz / 2, total, np.nan)
    return db(analog), analog_relative, total


def metrics(design, settings, args):
    frequencies = np.r_[np.linspace(args.fmin, args.passband_hz, 2001), args.edge_hz]
    _, _, total = curves(design, settings, frequencies, args.reference_hz)
    result = float(total[:-1].min()), float(total[:-1].max()), float(total[-1])
    passed = (result[0] >= -args.passband_db and result[1] <= args.passband_db and
              result[2] >= -args.edge_drop_db)
    return result, passed


def report(design, settings, args):
    print(f"\n{design.name}: Rs={design.series_r:g} ohm, Rt={design.termination_r:g} ohm")
    for index, element in enumerate(design.elements, 1):
        if element.kind == "L":
            extra = f", SRF={element.srf / 1000:g} kHz" if element.srf else ""
            print(f"  {index}: series L={element.value * 1000:g} mH, DCR={element.dcr:g} ohm{extra}")
        elif element.kind == "C":
            print(f"  {index}: shunt C={element.value * 1e9:g} nF")
        else:
            print(f"  {index}: series R={element.value:g} ohm")
    frequencies = args.frequencies or sorted(set([
        20.0, args.reference_hz, 10000.0, 14000.0, args.passband_hz, 18000.0,
        args.edge_hz, settings.sample_hz - args.edge_hz, settings.pwm_hz / 2,
        settings.pwm_hz - args.edge_hz, settings.pwm_hz,
    ]))
    absolute, relative, total = curves(design, settings, frequencies, args.reference_hz)
    print("       Hz    analog abs dB    analog rel dB    audio + hold rel dB")
    for frequency, ad, rd, td in zip(frequencies, absolute, relative, total):
        audio = f"{td:21.3f}" if np.isfinite(td) else f"{'--':>21}"
        print(f"{frequency:9.1f}{ad:17.3f}{rd:17.3f}{audio}")
    values, passed = metrics(design, settings, args)
    print(f"  {'PASS' if passed else 'FAIL'}: audio band {args.fmin:g}..{args.passband_hz:g} Hz "
          f"{values[0]:+.3f}..{values[1]:+.3f} dB; {args.edge_hz:g} Hz {values[2]:+.3f} dB")
    reference = transfer(design, settings, args.reference_hz)
    rms = settings.vdd / (2 * math.sqrt(2)) * abs(
        reference * sample_hold(args.reference_hz, settings.sample_hz))
    carrier = 2 * settings.vdd / math.pi * abs(transfer(design, settings, settings.pwm_hz))
    image_hz = settings.sample_hz - args.edge_hz
    image = (args.edge_hz / image_hz * abs(transfer(design, settings, image_hz) /
             transfer(design, settings, args.edge_hz)))
    print(f"  Full-scale {args.reference_hz:g} Hz sine: {float(rms):.4f} V RMS")
    print(f"  50%-duty PWM fundamental at OUT: {float(carrier) * 1000:.4f} mV peak")
    print(f"  {args.edge_hz:g} Hz tone's {image_hz:g} Hz first image: {float(db(image)):.2f} dBc")
    return passed


@dataclass(frozen=True)
class Variation:
    field: str
    index: int
    lower: float
    upper: float


def variations(design, settings, args):
    result = []

    def add(field, index, lower, upper):
        if lower != upper:
            result.append(Variation(field, index, lower, upper))

    def tolerance(field, index, value, percent):
        if math.isfinite(value):
            add(field, index, value * (1 - percent / 100), value * (1 + percent / 100))

    tolerance("series_r", -1, design.series_r, args.r_tol)
    tolerance("termination_r", -1, design.termination_r, args.r_tol)
    for index, element in enumerate(design.elements):
        percent = {"L": args.l_tol, "C": args.c_tol, "R": args.r_tol}[element.kind]
        tolerance("value", index, element.value, percent)
        if element.kind == "L":
            if args.dcr_range:
                add("dcr", index, *args.dcr_range)
            else:
                tolerance("dcr", index, element.dcr, args.dcr_tol)
    add("gpio_r", -1, *(args.gpio_range or (settings.gpio_r * 0.5, settings.gpio_r * 1.5)))
    add("receiver_r", -1, *(args.load_range or (settings.receiver_r, math.inf)))
    return result


def apply_variations(design, settings, parameters, values):
    elements = list(design.elements)
    for parameter, value in zip(parameters, values):
        if parameter.index >= 0:
            elements[parameter.index] = replace(elements[parameter.index], **{parameter.field: value})
        elif parameter.field in ("series_r", "termination_r"):
            design = replace(design, **{parameter.field: value})
        else:
            settings = replace(settings, **{parameter.field: value})
    return replace(design, elements=tuple(elements)), settings


def sensitivity(design, settings, args):
    parameters = variations(design, settings, args)
    if args.corners:
        count = 2 ** len(parameters)
        if count > 8192:
            raise ValueError(f"{count} corners exceeds the 8192-case limit; use --monte-carlo")
        cases = itertools.product(*[(p.lower, p.upper) for p in parameters])
        mode = "endpoint corners"
    else:
        count = args.monte_carlo
        rng = np.random.default_rng(args.seed)

        def draws():
            for _ in range(count):
                values = []
                for parameter in parameters:
                    if math.isinf(parameter.upper):
                        # A uniform conductance is meaningful for an open-ended
                        # receiving resistance range; uniform infinity is not.
                        conductance = rng.uniform(0, 1 / parameter.lower)
                        values.append(1 / conductance if conductance else math.inf)
                    else:
                        values.append(rng.uniform(parameter.lower, parameter.upper))
                yield values

        cases = draws()
        mode = f"Monte Carlo samples, seed {args.seed}"
    lower, upper, edge_lower, edge_upper = math.inf, -math.inf, math.inf, -math.inf
    failures = 0
    for values in cases:
        changed, environment = apply_variations(design, settings, parameters, values)
        (band_min, band_max, edge), passed = metrics(changed, environment, args)
        lower, upper = min(lower, band_min), max(upper, band_max)
        edge_lower, edge_upper = min(edge_lower, edge), max(edge_upper, edge)
        failures += not passed
    print(f"  Sensitivity: {count} {mode}; {failures} failed cases")
    print(f"    Audio band extrema: {lower:+.3f}..{upper:+.3f} dB")
    print(f"    {args.edge_hz:g} Hz range: {edge_lower:+.3f}..{edge_upper:+.3f} dB")
    print("    Sampled/end-point results are not a continuous worst-case or hardware guarantee.")
    return failures == 0


def write_csv(path, designs, settings, frequencies, reference_hz):
    with Path(path).open("w", newline="", encoding="utf-8") as output:
        writer = csv.writer(output)
        writer.writerow(["design", "frequency_hz", "analog_absolute_db", "analog_relative_db",
                         "audio_hold_relative_db"])
        for design in designs:
            absolute, relative, total = curves(design, settings, frequencies, reference_hz)
            for f, a, r, t in zip(frequencies, absolute, relative, total):
                writer.writerow([design.name, f, a, r, t if np.isfinite(t) else ""])


def write_plot(path, designs, settings, frequencies, args):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError as exc:
        raise ValueError("--plot requires Matplotlib: sudo apt install python3-matplotlib") from exc
    figure, (audio_axis, switching_axis) = plt.subplots(2, 1, figsize=(10, 9))
    audio_f = np.linspace(args.fmin, settings.sample_hz / 2 * 0.999, 2400)
    for design in designs:
        _, analog, total = curves(design, settings, audio_f, args.reference_hz)
        line, = audio_axis.plot(audio_f / 1000, total, label=f"{design.name}: audio + hold")
        audio_axis.plot(audio_f / 1000, analog, "--", color=line.get_color(),
                        label=f"{design.name}: analog only")
        _, switching, _ = curves(design, settings, frequencies, args.reference_hz)
        switching_axis.semilogx(frequencies, switching, label=design.name)
    audio_axis.fill_between([args.fmin / 1000, args.passband_hz / 1000],
                            -args.passband_db, args.passband_db, color="gray", alpha=0.12)
    audio_axis.plot(args.edge_hz / 1000, -args.edge_drop_db, "kx", label="edge droop budget")
    audio_axis.set(xlabel="Frequency (kHz)", ylabel="Gain relative to reference (dB)",
                   title="Wanted audio fundamental; solid lines include sample holding")
    for f, label in [(settings.pwm_hz / 2, "PWM/2"), (settings.pwm_hz, "PWM")]:
        switching_axis.axvline(f, color="gray", linestyle=":")
        switching_axis.text(f, 0, label, va="top")
    switching_axis.set(xlabel="Frequency (Hz)", ylabel="Analog relative gain (dB)",
                       title="Analog switching-frequency rejection (no sample-hold multiplier)")
    for axis in (audio_axis, switching_axis):
        axis.grid(True, which="both", alpha=0.3)
        axis.legend(fontsize="small")
    figure.tight_layout()
    figure.savefig(path)
    plt.close(figure)


def positive(value, label, allow_zero=False, allow_inf=False):
    if math.isnan(value) or value < 0 or (value == 0 and not allow_zero) or (
            not math.isfinite(value) and not allow_inf):
        raise ValueError(f"{label} must be {'nonnegative' if allow_zero else 'positive'}"
                         + (" (inf allowed)" if allow_inf else " and finite"))


def select_designs(args):
    custom = args.inductors_mh is not None or args.series_resistors_ohm is not None
    if args.design == "all" and (custom or args.caps_nf or args.series_ohm is not None or
                                 args.termination_ohm is not None or args.coil_dcr_ohm or args.coil_srf_khz):
        raise ValueError("component overrides require a single design, not --design all")
    if custom:
        if args.caps_nf is None or args.series_ohm is None or args.termination_ohm is None:
            raise ValueError("custom ladders require --caps-nf, --series-ohm and --termination-ohm")
        caps = [value * 1e-9 for value in args.caps_nf]
        if args.inductors_mh is not None:
            designs = [lc_design("custom-lc", [value * 1e-3 for value in args.inductors_mh],
                                 caps, args.series_ohm, args.termination_ohm)]
        else:
            if len(args.series_resistors_ohm) != len(caps):
                raise ValueError("custom RC ladders need one shunt capacitor per series resistor")
            elements = tuple(e for r, c in zip(args.series_resistors_ohm, caps)
                             for e in (Element("R", r), Element("C", c)))
            designs = [Design("custom-rc", args.series_ohm, args.termination_ohm, elements)]
    else:
        designs = list(PRESETS.values()) if args.design == "all" else [PRESETS[args.design]]
        if args.caps_nf is not None:
            elements = list(designs[0].elements)
            positions = [i for i, e in enumerate(elements) if e.kind == "C"]
            if len(positions) != len(args.caps_nf):
                raise ValueError("--caps-nf must have one value per shunt capacitor")
            for index, value in zip(positions, args.caps_nf):
                elements[index] = replace(elements[index], value=value * 1e-9)
            designs[0] = replace(designs[0], elements=tuple(elements))
        if args.series_ohm is not None:
            designs[0] = replace(designs[0], series_r=args.series_ohm)
        if args.termination_ohm is not None:
            designs[0] = replace(designs[0], termination_r=args.termination_ohm)
    for index, design in enumerate(designs):
        elements = list(design.elements)
        positions = [i for i, e in enumerate(elements) if e.kind == "L"]
        for supplied, field, scale in [(args.coil_dcr_ohm, "dcr", 1), (args.coil_srf_khz, "srf", 1000)]:
            if supplied is not None:
                if len(supplied) != len(positions):
                    raise ValueError(f"{field} list must have one value per inductor")
                for position, value in zip(positions, supplied):
                    elements[position] = replace(elements[position], **{field: value * scale})
        designs[index] = replace(design, elements=tuple(elements))
    return designs


def parser():
    result = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    result.add_argument("--design", choices=[*PRESETS, "all"], default="lc3-4.7")
    core = result.add_mutually_exclusive_group()
    core.add_argument("--inductors-mh", type=float, nargs="+", help="custom series inductors, input to output")
    core.add_argument("--series-resistors-ohm", type=float, nargs="+", help="custom RC stage resistors")
    result.add_argument("--caps-nf", type=float, nargs="+", help="shunt capacitances, input to output")
    result.add_argument("--series-ohm", type=float, help="external resistor before the ladder, excluding GPIO/DCR")
    result.add_argument("--termination-ohm", type=float, help="termination before coupling; inf to omit")
    result.add_argument("--coil-dcr-ohm", type=float, nargs="+", help="measured resistance per inductor")
    result.add_argument("--coil-srf-khz", type=float, nargs="+", help="optional SRF per inductor; 0 disables")
    result.add_argument("--gpio-ohm", type=float, default=40)
    result.add_argument("--load-ohm", type=float, default=10000, help="receiver resistance; inf for unplugged")
    result.add_argument("--bleed-ohm", type=float, default=100000, help="resistance after coupling; inf to omit")
    result.add_argument("--coupling-uf", type=float, default=4.7, help="0 omits the coupling capacitor")
    result.add_argument("--sample-hz", type=float, default=48000)
    result.add_argument("--pwm-hz", type=float, default=192000)
    result.add_argument("--vdd", type=float, default=3.3)
    result.add_argument("--reference-hz", type=float, default=1000)
    result.add_argument("--passband-hz", type=float, default=16000)
    result.add_argument("--passband-db", type=float, default=1, help="symmetric audio ripple/droop budget")
    result.add_argument("--edge-hz", type=float, default=20000)
    result.add_argument("--edge-drop-db", type=float, default=6, help="maximum edge attenuation; less is acceptable")
    result.add_argument("--frequencies", type=float, nargs="+", help="override printed frequency points, Hz")
    result.add_argument("--fmin", type=float, default=20, help="lower checked audio-band / export frequency")
    result.add_argument("--fmax", type=float, default=1000000, help="upper CSV/plot frequency")
    result.add_argument("--points", type=int, default=2400, help="log-spaced CSV/plot points")
    sweep = result.add_mutually_exclusive_group()
    sweep.add_argument("--corners", action="store_true", help="test all configured variation endpoints")
    sweep.add_argument("--monte-carlo", type=int, default=0, metavar="N", help="sample variation ranges")
    result.add_argument("--seed", type=int, default=1)
    result.add_argument("--l-tol", type=float, default=2, help="inductance tolerance, percent")
    result.add_argument("--c-tol", type=float, default=1, help="shunt capacitance tolerance, percent")
    result.add_argument("--r-tol", type=float, default=1, help="ladder/source/termination resistor tolerance, percent")
    result.add_argument("--dcr-tol", type=float, default=20, help="DCR tolerance, percent, unless --dcr-range supplied")
    result.add_argument("--dcr-range", type=float, nargs=2, metavar=("MIN", "MAX"), help="DCR range applied to each coil")
    result.add_argument("--gpio-range", type=float, nargs=2, metavar=("MIN", "MAX"), help="default 0.5..1.5 times --gpio-ohm")
    result.add_argument("--load-range", type=float, nargs=2, metavar=("MIN", "MAX"), help="default --load-ohm..inf")
    result.add_argument("--csv", type=Path, help="write nominal response data; parent directory must exist")
    result.add_argument("--plot", type=Path, help="write nominal response plot; requires Matplotlib")
    result.add_argument("--check", action="store_true", help="exit 1 if any selected nominal or sweep case fails")
    return result


def validate(args, designs):
    for field in ("sample_hz", "pwm_hz", "vdd", "reference_hz", "passband_hz", "edge_hz", "fmin", "fmax"):
        positive(getattr(args, field), field)
    for field in ("gpio_ohm", "coupling_uf", "passband_db", "edge_drop_db"):
        positive(getattr(args, field), field, allow_zero=True)
    for field in ("load_ohm", "bleed_ohm"):
        positive(getattr(args, field), field, allow_inf=True)
    if not (args.fmin < args.reference_hz <= args.passband_hz < args.edge_hz < args.sample_hz / 2):
        raise ValueError("require fmin < reference <= passband < edge < sample_hz/2")
    if args.fmax <= args.fmin or args.points < 2 or args.monte_carlo < 0 or args.seed < 0:
        raise ValueError("require fmax > fmin, points >= 2, and nonnegative Monte Carlo count/seed")
    if args.coupling_uf and math.isinf(args.load_ohm) and math.isinf(args.bleed_ohm):
        raise ValueError("receiver or bleed resistance must be finite to define OUT")
    if args.coupling_uf and math.isinf(args.bleed_ohm) and (
            args.corners or args.monte_carlo) and (
            args.load_range is None or math.isinf(args.load_range[1])):
        raise ValueError("a finite bleed resistance is needed for an open-receiver coupling sweep")
    for field in ("l_tol", "c_tol", "r_tol", "dcr_tol"):
        value = getattr(args, field)
        positive(value, field, allow_zero=True)
        if value >= 100:
            raise ValueError(f"{field} must be less than 100 percent")
    for field in ("dcr_range", "gpio_range", "load_range"):
        values = getattr(args, field)
        if values:
            for value in values:
                positive(value, field, allow_zero=field != "load_range", allow_inf=field == "load_range")
            if values[0] > values[1]:
                raise ValueError(f"{field} requires MIN <= MAX")
    for f in args.frequencies or []:
        positive(f, "printed frequency")
    for design in designs:
        positive(design.series_r, "series resistance", allow_zero=True)
        positive(design.termination_r, "termination resistance", allow_inf=True)
        for element in design.elements:
            positive(element.value, "element value")
            positive(element.dcr, "coil DCR", allow_zero=True)
            positive(element.srf, "coil SRF", allow_zero=True)


def main(argv=None):
    cli = parser()
    args = cli.parse_args(argv)
    try:
        designs = select_designs(args)
        validate(args, designs)
        settings = Settings(args.gpio_ohm, args.load_ohm, args.bleed_ohm,
                            args.coupling_uf * 1e-6, args.sample_hz, args.pwm_hz, args.vdd)
        print(f"GPIO={settings.gpio_r:g} ohm; receiver={settings.receiver_r:g} ohm; "
              f"bleed={settings.bleed_r:g} ohm; coupling={args.coupling_uf:g} uF")
        print(f"PCM={settings.sample_hz:g} Hz; PWM={settings.pwm_hz:g} Hz; reference={args.reference_hz:g} Hz")
        print(f"Targets: audio band within +/-{args.passband_db:g} dB; "
              f"edge droop <= {args.edge_drop_db:g} dB (all relative to reference)")
        passed = True
        for design in designs:
            passed = report(design, settings, args) and passed
            if args.corners or args.monte_carlo:
                passed = sensitivity(design, settings, args) and passed
        frequencies = np.geomspace(args.fmin, args.fmax, args.points)
        if args.csv:
            write_csv(args.csv, designs, settings, frequencies, args.reference_hz)
            print(f"\nCSV: {args.csv}")
        if args.plot:
            write_plot(args.plot, designs, settings, frequencies, args)
            print(f"Plot: {args.plot}")
        return 1 if args.check and not passed else 0
    except (ValueError, OSError) as exc:
        cli.error(str(exc))


if __name__ == "__main__":
    raise SystemExit(main())
