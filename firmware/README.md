# AudioLink firmware

AudioLink is a USB and Bluetooth LE Audio receiver with line outputs. It plays audio from a
computer over USB, or from a phone or an Auracast broadcast over Bluetooth, on two 6.35 mm jacks.

| Part          | Component                     |
| ------------- | ----------------------------- |
| Module        | MinewSemi MS45SF1 (nRF5340)   |
| DAC           | TI PCM5242                    |
| Output buffer | TI OPA1678                    |
| LEDs          | 4 x WS2812                    |
| Schematic/PCB | [`../pcb/`](../pcb/)          |

**Contents**

- Using the board: [Powering](#powering), [Outputs](#outputs),
  [Computer (USB)](#playing-from-a-computer-usb), [Phone](#playing-from-a-phone-bluetooth-le-audio),
  [Auracast](#auracast-broadcasts), [Source priority](#source-priority), [Equalizer](#equalizer),
  [LEDs](#leds)
- Updating: [over Bluetooth](#updating-the-firmware-over-bluetooth),
  [over USB](#updating-the-firmware-over-usb), [Release files](#release-files)
- [Development](#development)

## Powering

Plug a USB-C cable into J3. The board is always powered from USB, also when playing over
Bluetooth. A phone charger works when USB audio is not needed.

## Outputs

The connectors are not labelled on the board. Both jacks and the USB-C socket are on the same
edge. Looking straight at that edge, component side up:

```
   +----------------------------------------------+
   |      J2              USB-C           J1      |
   |  right channel       (J3)       left channel |
   +----------------------------------------------+
```

J1 is the main output. It carries left, or stereo when J2 is empty. In the board map in
[LEDs](#leds), J1 is the upper jack and J2 the lower one.

The output mode is chosen automatically from what is plugged in:

| Plugged in        | Mode                   | J1                           | J2                            |
| ----------------- | ---------------------- | ---------------------------- | ----------------------------- |
| J1 only           | Unbalanced stereo      | Tip = left, ring = right     | -                             |
| J1 and J2         | Balanced               | Left, tip = hot, ring = cold | Right, tip = hot, ring = cold |
| Mono plug in each | Unbalanced, two cables | Tip = left                   | Tip = right                   |

- **Stereo cable** (TRS, e.g. to an amplifier or powered speakers): use J1 and leave J2 empty.
  Anything in J2 switches the board to balanced mode.
- **Mono (TS) plugs** work in balanced mode, but the level is 6 dB lower and the unused cold
  output is shorted to ground through 100 Ohm. Fine for testing, prefer TRS cables for
  permanent use.
- The output mutes for about 20 ms while switching modes.

> [!NOTE]
> These are line outputs, headphones will sound quiet.

## Playing from a computer (USB)

The board shows up as a USB sound card named "AudioLink" (48 kHz, 16-bit stereo). No driver is
needed on Linux, macOS or Windows.

1. Connect USB-C to the computer.
2. Select "AudioLink" as the output device in the sound settings.
3. Set the volume on the computer. The board has no USB volume control.

## Playing from a phone (Bluetooth LE Audio)

Only Bluetooth LE Audio is supported, not classic Bluetooth audio (A2DP). This needs a phone with
LE Audio, for example Android 13 or newer on Pixel 7 or Samsung S23 and later. iPhones do not
support it.

1. On Android, make sure LE Audio is enabled (Pixel: Developer options, "Bluetooth LE audio").
2. In the Bluetooth settings, pair a new device and select "AudioLink". There is no PIN.
3. Play music. The phone's volume buttons control the board's volume.

The board accepts one phone at a time. To move it to another phone, disconnect the first one.
If pairing fails, forget "AudioLink" on the phone and pair again.

## Auracast broadcasts

While no phone is connected, the board joins the first Auracast broadcast it finds.

A phone that supports Auracast (for example "Audio sharing" on Android) can also tell a connected
board which broadcast to join. Encrypted broadcasts need their code to be sent from the phone.

## Source priority

When several sources play at once, the board picks one:

1. USB, while the computer has the AudioLink output open
2. A connected phone
3. An Auracast broadcast

Some computers keep the USB output open for a few seconds after playback stops. Switch the
computer to another output device to hand over to Bluetooth immediately.

## Equalizer

A 5-band equalizer applies to all sources. Each band is set from -6 to +6 dB in 1 dB steps.
All bands at 0 dB turns the equalizer off.

| Band | Frequency | Type       |
| ---- | --------- | ---------- |
| 1    | 60 Hz     | Low shelf  |
| 2    | 250 Hz    | Peak       |
| 3    | 1 kHz     | Peak       |
| 4    | 4 kHz     | Peak       |
| 5    | 12 kHz    | High shelf |

Boosted bands get louder, there is no automatic level reduction. Tracks that are already at full
scale can clip on boosted bands, lower the source volume if that happens.

The settings are kept in flash and survive a power cycle.

> [!NOTE]
> Changes to the equalizer and to the LED brightness and theme are saved 2 s after the last one,
> because writing flash interrupts the audio for a moment. A change made right before unplugging
> the board can be lost.

### Setting it without the app

Use nRF Connect for Mobile (Android, iOS) on a phone that is paired with the board:

1. Connect to "AudioLink" and open the service `6b1e0000-7a3c-4f5e-9d2a-3c8f1b6e4a20`.
2. Write 5 bytes to the characteristic `6b1e0001-...`, one signed byte per band, band 1 first:
   - `0400000002` is +4 dB at 60 Hz and +2 dB at 12 kHz
   - `0000000000` is flat
   - negative values: `FF` is -1 dB, `FE` is -2 dB, down to `FA` for -6 dB
3. Read the characteristic to see the current settings.

If the service does not show up on a phone that was paired before this feature was added, refresh
the services in nRF Connect, or forget the board and pair again.

## LEDs

The four LEDs change with the dynamics of the music: they light up at once on a louder sound,
fade over about a second in quiet parts, and are off when no audio is playing.

The LEDs are not labelled on the board. Hold it component side up with the jacks and USB-C on
the right, so J1 is the upper jack:

```
      LED 0                      LED 1
   +------------------------------------------------+
   | J4 (SWD)                                   J1  |=
   |        D5 (power)                          J3  |=  USB-C
   | U1 (module)            U2 (DAC)            J2  |=
   +------------------------------------------------+
      LED 2                      LED 3
```

D5, the small blue LED near the module, is the power indicator and is not controlled by the
firmware.

### Brightness

All four LEDs follow the sound level, from off when silent to the maximum brightness at full
scale. The brightness grows exponentially with the level in dB, so changes look even across the
range. The level is measured on the digital audio after the equalizer and before the volume, so
turning the volume down does not dim them.

The maximum brightness is 50% of full drive by default and can be set from 0% (LEDs off) to
100%. It is kept in flash like the equalizer. To change it:

- In the AudioLink app, under LED BRIGHTNESS.
- Over Bluetooth, by writing one byte (the percentage, `00` to `64` in hex) to the
  characteristic `6b1e0002-...` in the same service as the equalizer, for example `32` for 50%.

### Theme

The colors come from the LED theme and do not show the source or the output mode. Only the
brightness changes with the music.

| Theme | LED 0 and 2 | LED 1 and 3 |
| ----- | ----------- | ----------- |
| 0     | Amber       | Magenta     |
| 1     | Blue        | Cyan        |
| 2     | Green       | Lime        |
| 3     | Amber       | Yellow      |

Theme 0 is the default. The theme is kept in flash. To change it:

- In the AudioLink app, under LED THEME.
- Over Bluetooth, by writing one byte (the theme number, `00` to `03`) to the characteristic
  `6b1e0003-...` in the same service as the equalizer.

## Updating the firmware over Bluetooth

Both the application and the Bluetooth controller can be updated over Bluetooth.

### With nRF Connect Device Manager

1. Pair the phone with the board first, the update service only accepts paired phones.
2. Copy `build/app/dfu_application.zip` to the phone.
3. In nRF Connect Device Manager (Android, iOS), select "AudioLink", open Image, and upload the
   zip.
4. The board restarts and installs the update, which takes a few seconds.

### With the AudioLink app

The app installs the firmware bundled into it: tap the update button in the app bar.

1. Run `just app-firmware` to build the firmware and copy it into `app/assets/`.
2. Build the app.

The button only shows when the bundled firmware is newer than the one on the board, so raise
the version in `VERSION` for every release.

### Version check and rollback

- The bootloader refuses to install an application with a lower version than the one running,
  over Bluetooth or from the app, and keeps the current firmware instead.
- The same version can be installed again.
- USB recovery mode (`just usb-update`) and SWD (`just flash`) are not checked.

> [!WARNING]
> Updates are not rolled back automatically. A broken image has to be fixed over USB (see below)
> or SWD, so only upload images that have been tested on a board first.

## Updating the firmware over USB

Both cores can also be updated from a computer over the USB-C cable. Next to the sound card, the
board shows up as a USB serial port (for example `/dev/ttyACM0`). The update restarts the board
into the bootloader, which shows up as its own serial port named "AudioLink bootloader".

1. Connect USB-C to the computer.
2. Run `just usb-update`, it uploads `build/app/dfu_application.zip`. Needs `uv`.
3. The board restarts with the new firmware. Audio stops during the update.

Notes:

- The bootloader also starts by itself when the application image is missing or damaged, for
  example after an interrupted update. Run `just usb-update` again in that case.
- The bootloader cannot be reached over USB when the application starts but then hangs or
  crashes. That still needs SWD.
- On Linux, the user needs access to the serial port (the `dialout` or `uucp` group, depending
  on the distribution).
- Boards that were flashed before USB updates were added need one more update over SWD
  (`just flash`), because the bootloader itself can only be replaced over SWD.

## Release files

Built firmware is kept in [`assets/`](../assets/) at the top of the repository, next to the app.
Current release: **0.1.2**.

| File                                                                   | Use                                                                   |
| ---------------------------------------------------------------------- | --------------------------------------------------------------------- |
| [AudioLink-0.1.2-app-core.hex](../assets/AudioLink-0.1.2-app-core.hex) | New board over SWD, application core: MCUboot and app                 |
| [AudioLink-0.1.2-net-core.hex](../assets/AudioLink-0.1.2-net-core.hex) | New board over SWD, network core: bootloader and radio                |
| [AudioLink-0.1.2-dfu.zip](../assets/AudioLink-0.1.2-dfu.zip)           | Update over Bluetooth or USB, same as `build/app/dfu_application.zip` |

The commands below write the version as `<ver>`.

### Flashing a new board

Connect a J-Link to J4 and program the network core first, because the recovery erases both
cores:

```
nrfutil device recover
nrfutil device program --firmware AudioLink-<ver>-net-core.hex --core Network \
    --options chip_erase_mode=ERASE_ALL
nrfutil device program --firmware AudioLink-<ver>-app-core.hex --core Application \
    --options chip_erase_mode=ERASE_ALL
nrfutil device reset
```

### Updating a board

Upload `AudioLink-<ver>-dfu.zip` with nRF Connect Device Manager (see
[above](#updating-the-firmware-over-bluetooth)), or over USB, from the top of the repository:

```
uv run --script firmware/scripts/usb_update.py assets/AudioLink-<ver>-dfu.zip
```

### Making the hex files

After `just build`, merge the images that `just flash` writes with `mergehex.py` from the NCS
toolchain (run from `build/app`):

```
python3 ~/ncs/v<x.y.z>/zephyr/scripts/build/mergehex.py -o AudioLink-<ver>-app-core.hex \
    mcuboot/zephyr/zephyr.hex firmware/zephyr/zephyr.signed.hex
python3 ~/ncs/v<x.y.z>/zephyr/scripts/build/mergehex.py -o AudioLink-<ver>-net-core.hex \
    b0n_provision_merged.hex signed_by_b0_ipc_radio.hex
```

## Development

Requirements:

- nRF Connect SDK, installed with `nrfutil sdk-manager`
- `just`
- a J-Link probe on the 10-pin SWD header J4
- `nc`, for logs

The `just` commands read the SDK version from `.env` (not in git):

```
NCS_VERSION=v<x.y.z>
# Optional, $HOME/ncs/$NCS_VERSION by default
NCS_DIR=$HOME/ncs/$NCS_VERSION
```

| Command             | What it does                                                    |
| ------------------- | --------------------------------------------------------------- |
| `just keys`         | Creates the image signing keys in `keys/`, never overwrites     |
| `just build`        | Builds all images into `build/app`                              |
| `just app-firmware` | Builds and bundles the firmware into the app (`app/assets/`)    |
| `just flash`        | Flashes both cores and both bootloaders over SWD                |
| `just usb-update`   | Updates both cores over USB                                     |
| `just rtt`          | Shows the log over SWD, Ctrl-C to stop                          |
| `just clean`        | Removes all build directories                                   |

> [!WARNING]
> The signing keys in `keys/` are not in git. Keep a backup: without them, boards in the field
> can only be updated over SWD.

### Source layout

| Path                      | Contents                                                   |
| ------------------------- | ---------------------------------------------------------- |
| `boards/magik/audiolink`  | Board definition, pin map and flash layout                 |
| `src/audio_out.c`         | I2S output, source selection, clock tracking               |
| `src/dac.c`               | PCM5242 setup, analog power, mute and volume               |
| `src/usb_audio.c`         | USB Audio Class 2 sink                                     |
| `src/le_audio.c`          | Bluetooth: pairing, LE Audio unicast and broadcast, volume |
| `src/control.c`           | Settings service: EQ, LED brightness and theme             |
| `src/lc3_rx.c`            | LC3 decoding and left/right assembly                       |
| `src/leds.c`              | WS2812 status LEDs                                         |
| `src/main.c`              | Startup, jack detection, output mode, LED brightness       |
| `sysbuild/`               | Network core and bootloader configuration                  |
| `sysbuild.cmake`          | Applies the bootloader's RAM slot size after the NCS one   |
| `scripts/usb_update.py`   | Firmware update over USB                                   |
