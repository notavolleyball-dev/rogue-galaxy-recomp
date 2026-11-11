# Bug marks: reporting and reading a graphics fault

**In short:** when something on screen looks wrong, press **F8** with the
mouse over it. The game writes one folder with what is needed to find the
cause, and `tools/bugmark/bugmark.py` reads it and names every draw that
touched that pixel. This replaces screenshots, touched trigger files and
descriptions of where to look.

**Platforms:** the F8 folder is written on every platform. The frame capture
inside it, and the reader that needs it, have only been set up and tested on
**Linux**. RenderDoc itself runs on Windows, and the code looks for
`renderdoc.dll`, but nobody has built or run that path: on Windows, expect a
folder without a capture until someone tries it.

## Reporting a fault

1. Start the game under RenderDoc (Linux: see the setup below).
2. Go to the fault, put the mouse over it and press F8.
3. Wait for `[bugmark] done build/run/marks/...` in the log, about two
   seconds, before closing the game.

Other ways to make a mark, for scripted runs:

| Trigger | When |
|---|---|
| `touch build/run/mark-now` | From a shell, while the game runs |
| `RG_BUG_MARK_AT=2400,3300` | At those pad reads (see `RG_INPUT_CLOCK=pad`) |

## What the folder holds

`build/run/marks/<date>-<time>/`:

| File | Contents |
|---|---|
| `info.txt` | Commit and branch, flip, pad read, window size, mouse position in window pixels, every `RG_` and `DQ8_` variable |
| `settings.ini` | A copy of `RG_SETTINGS_FILE` |
| `frame_capture.rdc` | RenderDoc's capture of the next three flips. Only when the game runs under RenderDoc |
| `depthlog-N.txt` | Native pipeline only: the depth log from that frame on (`tools/depthlog.py` reads it) |
| `log.txt` | The part of `RG_LOG_FILE` around the mark, with the 2D piece dump. Only when `RG_LOG_FILE` names the file the game's output goes to |

Without RenderDoc the folder has everything but the capture.

## Reading a mark

```
tools/bugmark/bugmark.py [MARK] [--at X Y | --uv U V]
```

`MARK` defaults to the newest folder. `--at` takes window pixels and `--uv` a
fraction of the picture from its top left; without either, the mouse position
of the mark is used. The report is printed and saved in `MARK/report/`:

| Output | Contents |
|---|---|
| `report.txt` | The frame's passes and what made their draws; then, for the point, every draw that touched it in every target, in order: whether it wrote or what it lost to (depth test, discard, backface), colours and depths before and after, its depth and blend state and textures |
| `frame.png` | The picture of the frame |
| `flip_N.png` | The picture at each frame end of the capture, for a fault that comes and goes |
| `target_N.png` | The other targets the frame drew into |
| `tex_N.png` | The textures the reported draws read |

A draw is named by what made it: a VU1 mesh by its native program (`strip,
guard band and backfaces`), the rest as `2D piece`, `effect op` or `object
layer rectangle`. Targets are shown with the game's buffer address (`target 4
(buffer 2a00)`), and each draw gives its depth log frame and draw numbers
(`depth log frame 3238 draws 207-207`), which are the `F` and `D n=` lines of
the depth log.

## Setup on Linux

One time per machine. Nothing here is installed by the repository.

1. **RenderDoc**, for the reader: the distribution's package (`qrenderdoc`
   must be on the path). Version 1.46 is what was tested.
2. **On Wayland, a capture library built with Wayland support.** The Arch
   package is built without it, and the game's GPU device is then refused
   under RenderDoc. Running through X11 instead (`SDL_VIDEODRIVER=x11`) works
   but the picture flashed on KDE. Build only the library, same version as
   the package:

   ```
   git clone --depth 1 --branch v1.46 https://github.com/baldurk/renderdoc.git ~/.local/src/renderdoc
   cd ~/.local/src/renderdoc
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
       -DENABLE_UNSUPPORTED_EXPERIMENTAL_POSSIBLY_BROKEN_WAYLAND=ON \
       -DENABLE_QRENDERDOC=OFF -DENABLE_PYRENDERDOC=OFF -DENABLE_RENDERDOCCMD=OFF \
       -DENABLE_GL=OFF -DENABLE_GLES=OFF -DENABLE_EGL=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5
   ninja -C build
   mkdir -p ~/.local/share/renderdoc-wayland
   cp build/lib/librenderdoc.so ~/.local/share/renderdoc-wayland/
   sed "s#\"library_path\": \".*\"#\"library_path\": \"$HOME/.local/share/renderdoc-wayland/librenderdoc.so\"#" \
       build/lib/renderdoc_capture.json > ~/.local/share/renderdoc-wayland/renderdoc_capture.json
   ```

   RenderDoc calls its Wayland support experimental; capturing this game has
   worked with it.
3. **Start `qrenderdoc` once by hand** and answer its analytics question. The
   reader runs it without a window, where that first question would wait
   forever.

Then start the game with the capture layer on and its log in a file:

```
VK_ADD_IMPLICIT_LAYER_PATH=$HOME/.local/share/renderdoc-wayland \
ENABLE_VULKAN_RENDERDOC_CAPTURE=1 RG_LOG_FILE=build/run/play.log \
RG_NATIVE_RENDERER=1 RG_NATIVE_PIPELINE=1 \
build/game/src/roguegalaxy disc/SCUS_974.90 2>&1 | tee build/run/play.log
```

On X11 the first variable is not needed. The log says `[bugmark] RenderDoc is
here` when the capture will be made. Leave RenderDoc off for speed
measurements: its layer costs time.

## Limits

- The mouse is mapped to the picture as if the picture filled the window; with
  black bars at the sides the point is off. Use `--uv`.
- A GPU call that draws several meshes at once is named after the first.
- Draws are only named under the native pipeline (`RG_NATIVE_PIPELINE=1`).
- The depth log covers 30 frames and is about 80 MB a mark.
- Each capture is 5 to 80 MB. Nothing cleans `build/run/marks`.
