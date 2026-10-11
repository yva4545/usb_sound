# STM32F103 full-duplex USB audio card

USB Audio Class 1.0 capture/playback device for the STM32F103CBTx configured in
`sound_card.ioc`. It exposes independent **stereo capture and stereo playback**
streams, both signed 16-bit little-endian PCM at **48,000 Hz**. Capture uses the
12-bit ADC; playback uses 10-bit sample targets and **192 kHz PWM on PA8/PA9**.
Either direction can run alone, or both can run simultaneously.

## Build on Debian

The firmware builds with Debian's standard `arm-none-eabi-gcc`; STM32CubeIDE
is not required. Check the packages and compiler with:

```sh
dpkg-query -W gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi
arm-none-eabi-gcc --version
```

If these packages or the build tools are missing, install them with:

```sh
sudo apt update
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi cmake ninja-build
```

From this directory:

```sh
cmake --preset Debug
cmake --build --preset Debug
```

For the size-optimized firmware:

```sh
cmake --preset Release
cmake --build --preset Release
```

Each build produces `sound_card.elf`, `sound_card.bin`, `sound_card.hex`, and
`sound_card.map` under `build/Debug/` or `build/Release/`. The flash origin is
`0x08000000`. The linker script targets **128 KiB flash and 20 KiB RAM**, as
specified for the F103CB; adjust the flash size if using an F103C8 instead.

The build uses C11, Cortex-M3 Thumb instructions, software floating point,
and Newlib Nano. CMake 3.22 or newer is required. This has been built with
Debian GCC ARM 14.2.1 and binutils 2.44.

## Capture path

- **PA1 / ADC1_IN1:** channel 1 (left), ADC scan rank 1.
- **PA3 / ADC1_IN3:** channel 2 (right), ADC scan rank 2.
- **TIM3:** 72 MHz / 1500 = 48 kHz scan trigger; ADC1 scans both channels.
- **DMA1 channel 1:** circular buffer, with one millisecond of stereo samples
  in each half (96 halfwords per half; 192 halfwords total).
- **USB endpoint `0x81`:** asynchronous isochronous IN, scheduled every
  one-millisecond USB frame. Normal packets contain 192 bytes. Occasional
  188- or 196-byte packets accommodate differences between the ADC oscillator
  and the host's USB clock without discarding or duplicating samples.

`Core/Src/audio_capture.c` calibrates the ADC, starts DMA before the timer, and
converts unsigned ADC codes to signed PCM. ADC code 2048 maps to PCM zero.
A bounded ring buffer decouples DMA interrupts from USB transfers. Capture
starts with approximately three milliseconds of silent prefill; mute sends
silence while continuing to consume samples. A missed host poll is recovered
on the next USB frame. Both USB interrupt vectors are enabled for the F103.

At the configured 9 MHz ADC clock and 1.5-cycle acquisition time, adjacent
channels are sampled `(1.5 + 12.5) / 9 MHz = 1.556 µs` apart. Relative to PA1,
PA3 is sampled 1.556 µs later. The scan takes approximately 3.111 µs within the
20.833 µs sample-frame period.

## Playback path

- **PA8 / TIM1_CH1:** left PWM output.
- **PA9 / TIM1_CH2:** right PWM output.
- **TIM1:** edge-aligned PWM, prescaler 0, ARR 374, repetition counter 0;
  72 MHz / 375 = **192 kHz**, four carrier periods per PCM frame.
- **DMA1 channel 5:** TIM1 update request, circular halfword transfers to DMAR;
  two-register bursts write CCR1 and CCR2 preloads together. The buffer has two
  one-millisecond halves, 384 halfwords per half (1536 bytes total).
- **USB endpoint `0x02`:** asynchronous isochronous OUT, up to 49 stereo frames
  per USB millisecond (196 bytes).
- **USB endpoint `0x83`:** explicit feedback IN, three-byte little-endian Q10.14
  samples per USB frame. Nominal feedback is `00 00 0c` (48 samples/frame).

`Core/Src/audio_playback.c` converts S16_LE to unsigned 10-bit targets. Separate
left/right fractional-duty accumulators error-diffuse the target onto the
375-tick PWM grid. Each sample occupies four PWM periods; the accumulated pulse
area remains accurate to less than one timer tick. For silence, duty alternates
187/188 ticks, giving an exact average 50%. These are 10-bit digital targets;
actual analog SNR/ENOB depends on the filter, power supply, GPIO behavior, and
load, and still needs measurement.

A 512-frame ring supplies the PWM DMA refills. Playback waits for 144 frames
(approximately 3 ms) before consuming samples, emits midscale while idle/muted/underrunning, consumes
muted samples, and re-primes after an underrun or overflow. Already prepared PWM
halves drain before mute/stop takes full effect (up to approximately 2 ms).
The PWM carrier runs continuously, including when capture or playback is idle.
DMA refill interrupts occur twice per two-millisecond buffer, not at 192 kHz.

Feedback measures SOF-to-SOF time with the Cortex-M3 DWT cycle counter, clocked
from the same 72 MHz source as TIM1. A filtered clock estimate plus gentle ring
occupancy correction tracks crystal/host drift without inserting or deleting
playback samples. Counter wrap and long gaps between SOFs are handled. Playback
does not require an active capture stream to supply feedback. TIM2 and TIM4
remain free.

Both DMA engines are started before USB initialization. USB/ADC interrupts use
priority 0; PWM refill uses priority 1. Critical sections cover ring operations,
while PWM duty generation permits the higher-priority USB handlers to run.

## USB interfaces and packet memory

- Interface 0: AudioControl, independent master mute units for capture/playback.
- Interface 1: capture, alternate 0 idle / alternate 1 streaming.
- Interface 2: playback, alternate 0 idle / alternate 1 streaming with feedback.

Both stereo streams identify front left/right (`wChannelConfig = 0x0003`) and
use four bytes per PCM frame. Terminal graphs are line input → capture mute →
USB, and USB → playback mute → line output. The device is bus-powered; volume
and sample-rate controls are not advertised.

The F103 has only 512 bytes of USB PMA, separate from normal RAM:

| Area | PMA offset | Bytes |
|---|---:|---:|
| Endpoint table, EP0–EP3 | `0x0000` | 32 |
| EP0 OUT | `0x0020` | 16 |
| EP0 IN | `0x0030` | 16 |
| Capture IN | `0x0040` | 196 |
| Playback OUT | `0x0104` | 224 |
| Feedback IN | `0x01e4` | 4 |
| Unused | `0x01e8` | 24 |

EP0 advertises a legal 16-byte maximum packet size. The playback allocation is
rounded to 224 bytes because F103 receive buffers above 62 bytes use 32-byte
blocks; the software receive buffer also includes that padding before validating
packet size and whole stereo frames. Compile-time assertions check the layout.

F103 isochronous hardware switches between two logical buffer descriptors and
requires separate endpoint registers for IN/OUT. Both logical slots of each
audio/feedback endpoint point to the same physical PMA buffer. The HAL's
DTOG-aware ISO path selects the correct slot and count; IN completion clears the
consumed slot, and SOF retires unpolled IN packets. This saves PMA but requires
prompt copy/rearm before the next USB frame. Do not replace the alias setup with
ordinary single-buffer ISO handling or allocate distinct double buffers: the
latter does not fit. Hardware timing/continuous streaming needs verification.

## Analog connections

The existing clock configuration assumes an **8 MHz external crystal**, a
72 MHz system clock, and a 48 MHz USB clock. Analog inputs are expected to be
conditioned for the ADC's 0–VDDA range and biased near VDDA/2 for bipolar audio.
The firmware does not provide analog amplification, biasing, or anti-aliasing.
The board must also provide the F103's external USB D+ pull-up.

PA8/PA9 are digital PWM outputs. Connect each to an analog reconstruction
low-pass filter and then a suitable line-output buffer or audio amplifier.
For wideband audio, use a multi-pole filter that passes the intended audio band
and rejects the carrier and its sidebands; a single RC does not establish
10-bit audio quality. The idle PWM source averages VDD/2 (about 1.65 V at 3.3 V);
filter termination changes the filtered DC level. AC coupling or midpoint removal
must suit the following stage.

See [Filtering.md](Filtering.md) for the passive reconstruction-filter design,
component/load assumptions, and the `filter_analysis.py` calculation tool.

## Verification

Host-side tests exercise production capture, playback, and USB class code with
hardware stubs. They check descriptors/terminal routing/PMA capacity, independent
alternate settings and mute controls, endpoint error rollback, malformed OUT
packets, missed IN transfers, PCM endpoints and midpoint, all 1024 PWM targets,
stereo ordering across DMA/ring/counter wraps, underrun/overflow recovery, DWT
wrap/resume, and 60-second simulations of ±1000 ppm capture and playback drift.
The playback simulations model a host following explicit feedback.

With a native C compiler installed (`sudo apt install build-essential`):

```sh
cmake -S tests -B build/host-tests -G Ninja
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

Optional address/undefined-behavior sanitizer configuration:

```sh
cmake -S tests -B build/host-tests -G Ninja \
  -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer -g'
```

USB enumeration, PMA slot behavior/timing, PWM frequency, and actual simultaneous
analog recording/playback still need verification on hardware. After flashing
and reconnecting, Linux's standard `snd-usb-audio` driver should expose both
directions. For example, with `alsa-utils`:

```sh
arecord -l
aplay -l
# Substitute the card number shown above for N.
arecord -D hw:N,0 -f S16_LE -r 48000 -c 2 -d 10 duplex-capture.wav
# In another terminal, play a 48-kHz stereo S16_LE WAV file:
aplay -D hw:N,0 playback.wav
# Or generate left/right test tones through ALSA:
speaker-test -D hw:N,0 -F S16_LE -r 48000 -c 2 -t sine
```

## STM32CubeMX regeneration

The stock ST Audio middleware generates a playback/speaker class. This project
customizes `Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Inc/usbd_audio.h`
and `Src/usbd_audio.c` for full duplex, and adds capture, playback, and feedback
callbacks to the audio interface. These middleware changes are not represented
by the `.ioc` file.

After regeneration, preserve/reapply the full-duplex class and extended callback
table in `USB_DEVICE/App/usbd_audio_if.c`, audio-card strings in `usbd_desc.c`,
`usbd_conf.h` settings (48 kHz, three interfaces, bus-powered), and
`USB_MAX_EP0_SIZE = 16` in the middleware's `Core/Inc/usbd_def.h`.
Preserve `audio_usb_pma.h`, alias PMA configuration, four used endpoint registers,
and IN logical-slot count initialization in `usbd_conf.c`. Its static allocator
must return an aligned `USBD_AUDIO_HandleTypeDef` with a size check.

Preserve TIM1 paired-channel DMA burst setup, PWM DMA callbacks/IRQ priority,
and application startup before USB initialization. The top-level CMake file
includes `audio_capture.c` and `audio_playback.c`. The `.ioc` records PA1/PA3
capture, TIM3 timing, TIM1 PA8/PA9 PWM with ARR 374, DMA1 channel 5, and both USB
interrupt vectors; it cannot express the custom PCM/PWM and USB class logic.
