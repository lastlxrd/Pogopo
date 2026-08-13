# Intro and Power Outro animation assets

`../startup_frames.bin` contains 25 full-screen 400 x 240 monochrome frames.
Every frame is 12,000 bytes in the Sharp display's native LSB-first, white=1
framebuffer format.

`../outro_frames.bin` contains 25 transparent overlays. Each 24,000-byte frame
stores a 12,000-byte color plane followed by a 12,000-byte opacity mask. This
preserves the live menu/game image wherever the Aseprite frame is transparent.

They are packed directly from the supplied GIFs with:

```powershell
python tools\pack_sharpbit_gif.py `
  path\to\pogopoINTRO.gif `
  components\pogopo_startup\startup_frames.bin

python tools\pack_aseprite_overlay.py `
  path\to\pogopoOUTRO.aseprite `
  components\pogopo_startup\outro_frames.bin
```

The binary intentionally lives at the component root. This keeps the generated
ESP-IDF linker symbols stable on Windows and across ESP-IDF versions.

Both packers require exact black/white pixels. The Outro packer reads the true
Aseprite alpha channel instead of flattening transparent pixels to GIF black.
