# FeatherPDF

A very small, fast PDF reader for Windows. It's a single native `.exe` with no installer framework, no .NET, no Electron, no browser engine, and no background services. It uses about 15–40 MB of RAM for typical documents. A Chromium-based viewer like Edge often uses several hundred.

Builds are native for **Windows on ARM** (Snapdragon, e.g. Surface Laptop 7) and for **x64** (Intel/AMD).

## Install

1. Go to the [Releases page](../../releases/latest) and download:
   - **`FeatherPDF-arm64.zip`** for ARM laptops (Surface Laptop 7, Snapdragon X Elite/Plus),
   - **`FeatherPDF-x64.zip`** for Intel/AMD PCs.
2. Unzip it, then double-click **`Install.cmd`**.
   - Windows SmartScreen may say "Windows protected your PC" because the app isn't code-signed. Click **More info → Run anyway**.
3. The Settings page opens when the install finishes. Pick **FeatherPDF** as the default app for `.pdf`. Windows requires you to make this choice yourself.

The installer is per-user and needs no admin rights. It puts the app in `%LOCALAPPDATA%\Programs\FeatherPDF` and adds a Start menu shortcut. To remove it, uninstall it from **Settings → Apps → Installed apps**.

**Portable use:** you can also run `FeatherPDF.exe` directly from any folder. If you put an empty `FeatherPDF.ini` next to the exe, settings are stored there instead of `%APPDATA%`.

## Features

- Continuous scrolling with smooth high-DPI rendering (per-monitor DPI aware)
- Fit width, fit page, actual size, zoom from 10% to 3200% (Ctrl+wheel zooms at the cursor)
- Rotate pages and use full-screen mode
- Table-of-contents sidebar (bookmarks)
- Clickable links, with Back/Forward history
- Find text, with all matches highlighted
- Text selection (drag, or double-click a word) and copy
- Printing, which auto-rotates and shrinks pages to fit the paper
- Password-protected PDFs
- Dark pages mode (inverts page colours while keeping images natural)
- Recent files, and it reopens each file on the page where you left off
- Drag a file onto the window to open it
- Document properties (title, author, producer…)

## Keyboard shortcuts

| Action | Keys |
|---|---|
| Open / Close / Reload | Ctrl+O / Ctrl+W / F5 |
| Print | Ctrl+P |
| Next / previous page | Page Down / Page Up, Space, N / P, ← / → |
| First / last page | Home / End |
| Go to page | Ctrl+G |
| Back / forward | Alt+← / Alt+→ |
| Zoom in / out | Ctrl+Plus / Ctrl+Minus, Ctrl+wheel |
| Fit page / actual size / fit width | Ctrl+0 / Ctrl+1 / Ctrl+2 |
| Rotate right / left | Ctrl+R / Ctrl+Shift+R |
| Find, next, previous | Ctrl+F, F3, Shift+F3 |
| Copy selection / select page text | Ctrl+C / Ctrl+A |
| Hand (pan) tool | H (middle-drag always pans) |
| Contents sidebar | F9 |
| Full screen | F11 (Esc to leave) |
| Dark pages | Ctrl+D |
| Shortcuts help | F1 |

## How it stays light

- It's plain Win32 and GDI written in C. The only engine is [MuPDF](https://mupdf.com), which is also behind SumatraPDF. MuPDF is trimmed to a PDF-only build, without HTML/EPUB/XPS, JavaScript, OCR or barcode modules.
- It renders only what's on screen. At high zoom it renders just the visible part of a page instead of the whole page.
- Rendered pages go in a small 24-bit bitmap cache capped at about three screens' worth. MuPDF's font and image cache is capped at 32 MB.
- It draws without a back-buffer, uses `ScrollWindowEx` to scroll, and has no GPU or DirectX context.
- Closing a document returns its memory to Windows.
- Each window is its own small process, so nothing stays resident after you close it.

## Building from source

The app is cross-compiled on Linux with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw):

```bash
./build.sh            # both architectures -> dist/arm64 and dist/x64
./build.sh arm64      # only Windows on ARM
```

The script downloads the pinned toolchain and the MuPDF source and checks their hashes. Every push to GitHub builds both architectures, smoke-tests them on real Windows x64 and ARM64 machines, and publishes the zips on the Releases page.

## License

FeatherPDF is licensed under the GNU AGPL v3 (see `LICENSE`), because it links MuPDF © Artifex Software, which uses the AGPL.
