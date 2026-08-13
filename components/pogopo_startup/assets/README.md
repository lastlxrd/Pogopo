# Intro and Power Outro animation assets

`../startup_frames.bin` and `../outro_frames.bin` each contain 25 full-screen
400 x 240 monochrome frames. Every frame is 12,000 bytes in the Sharp display's
native LSB-first, white=1 framebuffer format.

They are packed directly from the supplied GIFs with:

```powershell
python tools\pack_sharpbit_gif.py `
  path\to\pogopoINTRO.gif `
  components\pogopo_startup\startup_frames.bin

python tools\pack_sharpbit_gif.py `
  path\to\pogopoOUTRO.gif `
  components\pogopo_startup\outro_frames.bin
```

The binary intentionally lives at the component root. This keeps the generated
ESP-IDF linker symbols stable on Windows and across ESP-IDF versions.

The packer composites GIF disposal/subframes, requires exact black/white pixels
and converts them once so playback only needs one 12 KiB copy per frame.
