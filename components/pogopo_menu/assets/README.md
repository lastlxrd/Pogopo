# Menu assets

`../menu_assets.bin` contains the four supplied 1-bit menu animations and four
bitmap-font faces generated from `pogofont-Regular.otf` and
`pogofont-RegularItalic.otf`.

The binary uses stable `_binary_menu_assets_bin_start/end` linker symbols and
is already generated; Pillow is not required for a normal ESP-IDF build.

To regenerate it from the original `docs` directory:

```powershell
python tools\pack_menu_assets.py `
  path\to\docs `
  components\pogopo_menu\menu_assets.bin `
  components\pogopo_menu\src\menu_data_generated.h
```
