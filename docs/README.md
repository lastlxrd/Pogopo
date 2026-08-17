# pogopo technical documentation

This folder is a dependency-free static documentation site for GitHub Pages.
It is designed to live at `pogopoOS/docs` without changing the firmware project
layout or build.

The top-level pages are Overview, Hardware, Modularity, PogopoOS, Architecture,
Engineering, Gallery, FAQ, and Development. The former `getting-started` and
`games` URLs are retained only as compatibility redirects.

## Local preview

From the repository root, run:

```powershell
python -m http.server 8000
```

Then open `http://localhost:8000/docs/`.

## GitHub Pages

Commit the `docs` folder, open the repository **Settings → Pages**, choose
**Deploy from a branch**, select the `main` branch and the `/docs` folder, then
save. The site will be published under the repository's GitHub Pages URL.

## Editing

Every page is plain HTML. Shared appearance is in `assets/styles.css`.
`assets/theme.js` restores the light/dark preference before the page is painted,
while `assets/app.js` generates the navigation, page table of contents, search,
theme button, mobile menu, and previous/next links. The frog remains the
original transparent PNG; its light-mode outline is generated at display time.

Hardware photographs and final architecture diagrams are intentionally marked
as TODO until verified project assets are added. UI captures in
`assets/gallery` come from the current firmware project.
