# Startup animation asset

`startup_frames.bin` contains 111 full-screen 400 x 240 monochrome frames.
Each frame is 12,000 bytes in the Sharp display's native LSB-first, white=1
framebuffer format.

It was packed from SharpBit assets named `pogopo_scenee1` through
`pogopo_scenee111` with:

```powershell
python tools\pack_sharpbit_frames.py `
  path\to\output\generated `
  components\pogopo_startup\assets\startup_frames.bin
```

SharpBit's current default packing is MSB-first with black=1. The packer
converts that representation once so playback only needs one 12 KiB copy per
frame.
