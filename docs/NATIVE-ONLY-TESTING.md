# Testing the native-only branch

How to play-test `native-only/m4b-2d` (milestones M1 to M6 of
docs/NATIVE-ONLY.md). Everything on it compiles; none of it has been run.

## Setup

1. Pull the branch.
2. Check the submodule out again, so the runtime patches the branch deleted
   (0002, 0013, 0015, 0019-0021, 0028, 0030, 0031, 0034) leave the checkout:
   ```
   git -C thirdparty/PS2Recomp checkout -- .
   rm thirdparty/PS2Recomp/.rg-patches-applied
   cmake -S . -B build/game
   ```
3. Build as usual and run with stderr kept (the log lines below go there).

## The route

Each step names what changed under it, so a breakage points at a
milestone.

| Where | Look at | Mostly |
|---|---|---|
| Title screen | logo, menu text, fades | M4 (2D blocks), M3 (fades) |
| Opening movie | the picture, its box on a wide screen | M6 (movie upload), M4 (widescreen) |
| Salgin, first scene | characters, scenery, shadows under characters, glare/bloom | M5 (models), M3 (passes) |
| Field | HUD, minimap and compass, text boxes, glyph borders | M4 (blocks, glyphs) |
| HUD size and widescreen options | both as on the `gs-path-final` tag | M4 |
| First battles | effects, particles, outlines, character glow and blur (object layers) | M5, M6 (captured streams, composites) |
| A boss | the same under load; frame time | M6 (lists on the game thread) |
| Menus | every page, item icons, text | M4, uploads |
| Save, then load | the screens, the captured frame behind them | M3 (captures), uploads |
| A texture pack (if you have one) | whole textures and 2D regions replaced | region packs |

## Log lines

The lines most likely to point at a breakage. Paste them with any report.

- `[native-blocks]`: 2D blocks drawn; **lost should stay 0**.
- `[gs-packets]`: packets that still draw, by draw task (2D left outside
  the builder; each one named is not drawn).
- `[model-emit]`: model calls from records; calls the game still writes,
  by why; captured streams and **captures that failed**; view/camera/light
  packets taken out.
- `[vu1native]`: native chunks; **not drawn**, by reason; chunks outside a
  record or stream (should be 0).
- `[uploads]`: the list's uploads by path and draw task.
- `[composite]`, `[nativefx]` (passes drawn and not), `[native-env]`.
- The stats line: frame time and `lists busy`.

## When something breaks

- Note where, what, and the log lines around it; a screenshot helps.
- Bisect over the branch's commits. **Skip 906c33d** (it does not build;
  f3bee42 fixes it). The branches stack m0, m1, m2, m3, m4, m5, m4b, so a
  breakage can be placed by milestone first.
- No replay pixel diffs: input replay is not deterministic. Compare by
  the counters and by eye.

## The flash watch

For a flash too short to mark with F8: run with `RG_FLASH_WATCH=1`. Every
drawn frame's picture is read back small (slower), and a frame that
differs from both its neighbours where they agree is reported:

- `[flash] frame N ...`: where on the picture, how many draws each of the
  three frames had, their cameras, and the draws only the flash frame has
  or lacks (by geometry, texture, state, target; then with the model
  matrix too).
- `build/run/flash/frameN.ppm`: the three pictures side by side (before,
  flash, after), 192x56.

Pan until it has flashed a few times; push the log and `build/run/flash/`.
At most 300 reports a run.
