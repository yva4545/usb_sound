# STM32F103 USB audio input

USB Audio Class 1.0 recording device for the STM32F103CBTx configured in
`sound_card.ioc`. It exposes one six-channel capture stream with unsigned 8-bit
PCM (**U8**, USB Audio PCM8) at **48,000 Hz per channel**. ADC1 sequentially scans
PA1 through PA6; the top eight bits of each 12-bit ADC sample are transmitted.
Single-byte samples have no endianness.

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

- **PA1 / ADC1_IN1:** channel 1, ADC scan rank 1.
- **PA2 / ADC1_IN2:** channel 2, ADC scan rank 2.
- **PA3 / ADC1_IN3:** channel 3, ADC scan rank 3.
- **PA4 / ADC1_IN4:** channel 4, ADC scan rank 4.
- **PA5 / ADC1_IN5:** channel 5, ADC scan rank 5.
- **PA6 / ADC1_IN6:** channel 6, ADC scan rank 6.
- **TIM3:** 72 MHz / 1500 = 48 kHz scan trigger; ADC1 scans all six channels.
- **DMA1 channel 1:** circular buffer, with one millisecond of six-channel
  samples in each half (288 halfwords per half; 576 halfwords total). DMA retains
  16-bit ADC transfers; conversion to U8 happens when filling the capture ring.
- **USB endpoint `0x81`:** asynchronous isochronous IN, scheduled every
  one-millisecond USB frame. Normal packets contain 288 bytes. Occasional
  282- or 294-byte packets accommodate differences between the ADC oscillator
  and the host's USB clock without discarding or duplicating samples.

The USB terminal graph is six-channel line input → master mute → USB streaming.
The descriptor exposes six independent inputs without spatial assignments
(`wChannelConfig = 0x0000`). Each PCM frame contains one byte each for PA1, PA2,
PA3, PA4, PA5, then PA6 (six bytes total). Endpoint `0x81`
occupies 294 bytes at PMA address `0x00c0`, leaving 26 bytes of the F103's
512-byte packet memory unused.

AudioControl is interface 0. AudioStreaming is interface 1: alternate setting 0
is idle, and alternate setting 1 enables capture. The device reports itself as
bus-powered and has no advertised volume or sample-rate controls.

`Core/Src/audio_capture.c` calibrates the ADC, starts DMA before the timer, and
converts ADC codes to unsigned PCM8 by shifting right four bits: ADC code 0 maps
to 0, 2048 maps to 128 (silence), and 4095 maps to 255. The four low bits are
discarded, without rounding or dithering. The USB format tag is PCM8 (`0x0002`),
not signed PCM (`0x0001`); startup, underrun, and mute silence use `0x80`.
A bounded ring buffer decouples DMA interrupts from USB transfers. Capture
starts with approximately three milliseconds of silent prefill; mute sends
silence while continuing to consume samples. A missed host poll is recovered
on the next USB frame. Both USB interrupt vectors are enabled for the F103.

At the configured 9 MHz ADC clock and 1.5-cycle acquisition time, adjacent
channels are sampled `(1.5 + 12.5) / 9 MHz = 1.556 µs` apart. Relative to PA1,
PA2 through PA6 are sampled approximately 1.556, 3.111, 4.667, 6.222, and
7.778 µs later. The complete scan takes approximately **9.333 µs** within the
20.833 µs sample-frame period. The ADC remains in 12-bit mode; the F103 has no
native 8-bit conversion mode. Use low-impedance input drivers to allow settling
during the short 1.5-cycle acquisition time, especially when adjacent inputs
have different voltages.

The existing clock configuration assumes an **8 MHz external crystal**, a
72 MHz system clock, and a 48 MHz USB clock. Analog inputs are expected to be
conditioned for the ADC's 0–VDDA range and biased near VDDA/2 for bipolar audio.
The firmware does not provide analog amplification, biasing, or anti-aliasing.
The board must also provide the F103's external USB D+ pull-up.

Six U8 channels use the same USB payload bandwidth as the previous three S16_LE
channels: **288,000 bytes/s**, with an unchanged 294-byte maximum endpoint packet
and PMA layout. The ADC DMA buffer uses 1,152 bytes and the 512-frame U8 capture
ring uses 3,072 bytes.

PA0 (ADC1_IN0) and PA7 (ADC1_IN7) are unassigned in the current project. An
eight-channel version could scan PA1 → PA2 → PA3 → PA4 → PA5 → PA6 → PA7 → PA0,
but its 384-byte nominal / 392-byte maximum packets do not fit the current
320-byte PMA region. It would need a revised USB memory/control-endpoint layout
or a lower sampling rate. Physical availability depends on the board wiring.

## Verification

Host-side tests exercise the actual capture and USB class code with hardware
stubs. They check descriptor lengths and terminal routing, alternate settings,
master mute, malformed control requests, missed transfers, six-channel order,
all 12-bit-to-U8 quantization codes, PCM endpoints and midpoint, ring wrapping,
underrun/overflow recovery, and 60-second simulations
of ±1000 ppm ADC clock drift.

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

USB enumeration and actual audio recording still need verification on hardware.
After programming and connecting the board, Linux's standard `snd-usb-audio`
driver should expose it as a capture device. For example, with `alsa-utils`:

```sh
arecord -l
# Substitute the card number shown above for N.
arecord -D hw:N,0 -f U8 -r 48000 -c 6 -d 10 capture.wav
```

## STM32CubeMX regeneration

The stock ST Audio middleware generates a playback/speaker class. This project
customizes `Middlewares/ST/STM32_USB_Device_Library/Class/AUDIO/Inc/usbd_audio.h`
and `Src/usbd_audio.c` for capture, and adds a `GetPacket` callback to the audio
interface. These middleware changes are not represented by the `.ioc` file.

After regeneration, preserve/reapply that capture class, the added `GetPacket`
entry in `USB_DEVICE/App/usbd_audio_if.c`, the capture strings in `usbd_desc.c`,
and the settings in `USB_DEVICE/Target/usbd_conf.h` (48 kHz, two interfaces,
bus-powered). The endpoint PMA configuration and application hooks are in
`USER CODE` sections; retain them. In `usbd_conf.c`, the static allocator must
return a properly aligned `USBD_AUDIO_HandleTypeDef` with a size check.
`Core/Src/audio_capture.c` is included by the top-level CMake file. The `.ioc`
enables the USB high-priority interrupt as well as the low-priority interrupt.
It also configures the six ADC inputs and their PA1 → PA2 → PA3 → PA4 → PA5 → PA6
scan order. Preserve the PCM8 format tag, one-byte subframes, 8-bit resolution,
six-channel terminal/format descriptors, and the expanded 14-byte feature unit
(44-byte AudioControl total, 114-byte configuration total).
