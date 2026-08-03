# PogopoOS

<p align="center">
  <strong>Pogopo is a custom ESP32-S3 handheld console powered by PogopoOS.</strong>
</p>

<p align="center">
  <img src="docs/images/pogopo-main.jpg" alt="Pogopo handheld console" width="900">
</p>

<p align="center">
  <img src="docs/images/pogopo.jpg" alt="Pogopo top" width="900">
</p>

<p align="center">
  <img src="docs/images/pogopo-detail.jpg" alt="Pogopo hardware detail" width="900">
</p>

## Getting Started

PogopoOS is still under active development, so the public documentation is not
ready yet. A short FAQ, setup guide, and tutorials will be added as the project
matures.

**FAQ:** coming soon  
**Tutorials:** coming soon

## Project Summary

PogopoOS is a custom firmware and application platform for a Pogopo - hand-built
ESP32-S3 handheld console. The project combines a low-power monochrome display,
physical controls, audio, motion sensing, removable storage, haptic feedback,
and custom power management in one compact prototype.

The goal is to build a small standalone device that can run native applications,
utilities, and games through a simple custom interface. Both the hardware and
software are still evolving, and the current console is a working development
prototype rather than a finished product.

## Hardware

- ESP32-S3 with 16 MB flash and 8 MB PSRAM
- Sharp LS027B7DH01 400 × 240 Memory LCD
- BMI270 motion sensor
- MAX98357A I2S audio amplifier
- microSD card over 4-bit SDMMC
- TCA9555 GPIO and button expander
- D-pad, A, B, Menu, Start, and Power controls
- Vibration motor
- Rechargeable battery
- Onboard charging and power management
- Custom PCB and 3D-printed enclosure

## Software

- ESP-IDF-based firmware
- Graphical launcher and application framework
- Reusable display, input, audio, storage, and sensor drivers
- Native I2S audio and WAV playback
- Motion sensing and visualization tools
- microSD file access
- Persistent settings
- Power quick menu and clean LCD shutdown
- Haptic feedback
- Native Game Boy emulation
- Full-speed Game Boy CPU execution
- Approximately 30 FPS LCD presentation
- Four-shade ordered dithering
- Fixed-rate Game Boy audio
- ROM loading from microSD
- Experimental PDX and Lua game runtime

Game ROMs and third-party game files are not included in this repository.

## Development

The project is developed through small tested milestones. Commit messages
contain short change summaries, while longer technical notes are kept in
`docs/history/`.
