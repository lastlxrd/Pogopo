# pogopoOS2.0 STEP8 — Streaming Audio + Persistent Settings

This project is based on the fully tested STEP7.1 build.

## 1. Streaming WAV engine

The old WAV test loaded the whole file into PSRAM and intentionally rejected files above 4 MiB. STEP8 replaces the WAV Player path with a streaming engine inside `pogopo_audio`.

Architecture:

```text
SD card / FILE reader task (Core 1)
        ↓
32768-frame mono ring buffer in PSRAM
        ↓
linear sample-rate conversion
        ↓
I2S mixer task (Core 0)
        ↓
MAX98357A at 32768 Hz
```

The generated system voices remain active while music is playing, so menu clicks and alerts are mixed over the track.

Supported stream format:

- RIFF/WAVE PCM, format code 1
- 8-bit or 16-bit
- mono or stereo (stereo is downmixed to mono)
- 8–96 kHz
- large files are supported; the file is not copied into RAM

Compressed WAV, MP3, AAC and FLAC are not supported in this step.

## 2. WAV Player controls

```text
TOP / DOWN   select a WAV file
A            start or restart selected file
START        pause / resume
LEFT         seek back 5 seconds
RIGHT        seek forward 5 seconds
B            return home; music keeps playing
MENU         open the system overlay
```

The screen shows:

- playback state
- position and duration
- buffered milliseconds
- source sample rate / channels / bit depth
- underrun and SD read-error counters

Copy `COPY_TO_SD/pogopo` to the root of the SD card. A generated `stream_long_70s.wav` file is included; it is larger than the old 4 MiB loader limit and is the main streaming test.

## 3. Persistent `pogopo_settings`

A new NVS-backed component stores:

```text
master volume
Audio output ON/OFF
UI sounds ON/OFF
Haptics ON/OFF
Motion sensitivity LOW/NORMAL/HIGH
```

Open **Settings** in the Launcher. Changes are applied immediately and committed to NVS after a short delay. Pressing B also saves before leaving. START restores defaults.

The volume changed in Audio Lab is also written to the same persistent setting.

## 4. Haptic tuning

Requested tuning is included:

```text
Tick   45 ms   (same feel as the previous Click)
Click  55 ms   (slightly more pronounced)
```

All other haptic patterns remain unchanged. Disabling Haptics in Settings suppresses all patterns at the component level.

## 5. Motion setting

Motion Lab keeps the smooth STEP7.1 visual filter. The saved motion-sensitivity setting changes only the visual response:

```text
LOW      ×0.65
NORMAL   ×1.00
HIGH     ×1.45
```

The numeric BMI270 values remain immediate and unfiltered.

## Recommended test order

1. Copy the included `pogopo` folder to the SD-card root.
2. Boot and open **Settings**. Change volume and motion sensitivity, reboot, and verify that the values remain saved.
3. Open **WAV Player** and play `stream_long_70s.wav`.
4. While it plays, seek with LEFT/RIGHT and pause/resume with START.
5. Press B and navigate the Launcher while the music continues underneath the UI sounds.
6. Open Motion Lab during playback and verify that graphics, BMI270 sampling, audio and input remain responsive.
7. Open Haptics Lab and compare the new Tick and Click.

## Notes

- Do not physically remove the SD card while a stream is playing. Stop/reboot first.
- An unsupported or damaged WAV enters the visible ERROR state and plays the system error feedback when UI sounds are enabled.
- SD and BMI270 initialization remain non-fatal. Power/BQ24295 initialization remains required.
- The old small-file `Storage::loadWav()` API is still present for future short assets, but the WAV Player uses the new streaming path.
