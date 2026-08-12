# Startup animation asset

`../startup_frames.bin` contains 25 full-screen 400 x 240 monochrome frames.
Each frame is 12,000 bytes in the Sharp display's native LSB-first, white=1
framebuffer format.

It was packed from the monolithic SharpBit export `pogopointro.c` with:

```powershell
python tools\pack_sharpbit_animation.py `
  path\to\pogopointro.c `
  components\pogopo_startup\startup_frames.bin
```

The binary intentionally lives at the component root. This keeps the generated
ESP-IDF linker symbols stable on Windows and across ESP-IDF versions.

SharpBit's current default packing is MSB-first with black=1. The packer
converts that representation once so playback only needs one 12 KiB copy per
frame.
