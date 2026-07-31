# pogopoOS2.0 STEP6 — Native audio engine

This step keeps the working previous step GUI/application framework and adds a native ESP-IDF I2S audio component for the MAX98357A.

## Hardware

- MAX98357A DIN / ESP32 DOUT: GPIO38
- BCLK: GPIO39
- LRCK / WS: GPIO40
- 32768 Hz
- 16-bit stereo Philips I2S
- No MCLK

## New component

`components/pogopo_audio`

- `pogopo::audio::Audio`
- independent FreeRTOS audio task on Core 0
- continuous DMA-backed I2S stream
- four-voice software mixer
- sine, square, triangle and noise waveforms
- attack/release envelope to reduce clicks
- non-blocking command queue
- master volume 0–100%
- generated UI effects and raw tones
- write/error/queue statistics

## Audio Lab controls

- TOP / DOWN: select sound
- A: play selected sound
- LEFT / RIGHT: volume -/+ 5%
- START: startup melody
- B: return to launcher
- MENU: system overlay

## Included sounds

- UI tick
- click
- confirm
- back
- error
- startup melody
- coin
- 440 Hz sine
- 880 Hz square
- noise burst

## Haptics tuning

The motor is still GPIO3 active-high. Because the driver is binary on/off, perceived strength is tuned by pulse length:

- Tick: 18 ms -> 24 ms
- other effects: approximately 5% longer

## Expected boot log

```
pogopoOS2.0 AUDIO ENGINE STEP6
Audio ready: 32768 Hz, 16-bit stereo Philips, DOUT=38 BCLK=39 LRCK=40
STEP6 ready: native I2S audio mixer + GUI audio lab + louder haptics
```
