# STM32F103 USB audio input

USB Audio Class 1.0 recording device for the STM32F103CBTx configured in
`sound_card.ioc`. It exposes one three-channel capture stream with signed 16-bit,
little-endian PCM at **48,000 Hz**. The MCU's ADC supplies 12-bit samples,
scaled into the 16-bit USB format.

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
- **PA2 / ADC1_IN2:** channel 3 (center), ADC scan rank 3.
- **TIM3:** 72 MHz / 1500 = 48 kHz scan trigger; ADC1 scans all three channels.
- **DMA1 channel 1:** circular buffer, with one millisecond of three-channel
  samples in each half (144 halfwords per half; 288 halfwords total).
- **USB endpoint `0x81`:** asynchronous isochronous IN, scheduled every
  one-millisecond USB frame. Normal packets contain 288 bytes. Occasional
  282- or 294-byte packets accommodate differences between the ADC oscillator
  and the host's USB clock without discarding or duplicating samples.

The USB terminal graph is three-channel line input → master mute → USB streaming.
The descriptor identifies the channels as front left, front right, and front
center (`wChannelConfig = 0x0007`). Each PCM frame contains PA1, PA3, then PA2,
with two little-endian bytes per channel (six bytes total). Endpoint `0x81`
occupies 294 bytes at PMA address `0x00c0`, leaving 26 bytes of the F103's
512-byte packet memory unused.

AudioControl is interface 0. AudioStreaming is interface 1: alternate setting 0
is idle, and alternate setting 1 enables capture. The device reports itself as
bus-powered and has no advertised volume or sample-rate controls.

`Core/Src/audio_capture.c` calibrates the ADC, starts DMA before the timer, and
converts unsigned ADC codes to signed PCM. ADC code 2048 maps to PCM zero.
A bounded ring buffer decouples DMA interrupts from USB transfers. Capture
starts with approximately three milliseconds of silent prefill; mute sends
silence while continuing to consume samples. A missed host poll is recovered
on the next USB frame. Both USB interrupt vectors are enabled for the F103.

At the configured 9 MHz ADC clock and 1.5-cycle acquisition time, adjacent
channels are sampled `(1.5 + 12.5) / 9 MHz = 1.556 µs` apart. Relative to PA1,
PA3 is sampled 1.556 µs later and PA2 3.111 µs later. The complete scan takes
approximately 4.667 µs within the 20.833 µs sample-frame period.

The existing clock configuration assumes an **8 MHz external crystal**, a
72 MHz system clock, and a 48 MHz USB clock. Analog inputs are expected to be
conditioned for the ADC's 0–VDDA range and biased near VDDA/2 for bipolar audio.
The firmware does not provide analog amplification, biasing, or anti-aliasing.
The board must also provide the F103's external USB D+ pull-up.

## Verification

Host-side tests exercise the actual capture and USB class code with hardware
stubs. They check descriptor lengths and terminal routing, alternate settings,
master mute, malformed control requests, missed transfers, three-channel order,
PCM endpoints and midpoint, ring wrapping, underrun/overflow recovery, and 60-second simulations
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
arecord -D hw:N,0 -f S16_LE -r 48000 -c 3 -d 10 capture.wav
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
It also configures the three ADC inputs and their PA1 → PA3 → PA2 scan order.
