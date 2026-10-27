# Options

**F1** (or the Guide button, or Back+Start) opens the in-game menu, from
DQ8-Recompiled: window and fullscreen, vertical sync, internal resolution,
aspect and scaling filter, what fills the sides beside a 4:3 picture
(Display > Borders: black, the picture blurred, or your image from the
`borders` folder, see `borders\README.txt`), volume, controller and keyboard remapping with
dead zones, pause (F2), fast-forward (F4), a frame-rate counter (F3),
screenshots (F12) and renderer statistics, and on the Game page the game's
own switches and numbers (a slider between - and + buttons, with a Default
button), in four groups: Camera and view, Convenience, Battles and rewards,
Developers:
- "Widescreen": the game draws a wider view, with the HUD over the
  scene, the compass and the movies fitted to it (events keep their
  letterbox bars, which their text is placed by); with Display > Aspect
  ratio on Auto the picture fills the window, whatever its shape from 4:3
  to 32:9, once a game is loaded (the title, load menu and loading screen
  stay 4:3). Menus and text are drawn for 4:3 and come out stretched.
- "Skip the opening": at boot the Sony, Japan Studio and Level-5 logos give
  way to the title screen at once.
- "Show the loading recap at once": the whole story so far on the loading
  screen at once, instead of line by line, and Press X comes as soon as the
  game has loaded.
- "Field of view": how wide the camera sees while exploring and in
  battle, in degrees across a 4:3 picture (the game's 52, from 40 to 90; a
  wide picture adds to it); scenes keep their own camera
  (`src/camera_view.h`).
- "Camera distance": how far the camera stays from the party, 70% to 160%
  of the game's.
- "Faster camera rotation": the stick turns the camera twice as fast,
  exploring, in battle and in the first-person view (both sticks there).
  Otherwise it turns at the game's speed:
  the game adds a frame's turn each frame, which the 60 frames a second
  mode halves (`src/camera_view.h`).
- "Draw distance": how far away the scenery and the objects are still
  drawn, 100% to 400% of the game's, the fog thinned in step
  (`src/draw_distance.h`), from the next map loaded. Most of Salgin has no
  limit to raise; the open areas gain more.
- "Encounter rate", "Experience from battles", "Money from battles":
  percentages of the game's (`src/battle_rates.h`). Encounters from 0%
  (no random battles; the story's stay) to 300%; a won battle's
  experience, up to the game's 9999, and the defeated enemies' money, from
  0% to 500%. Selling, chests and the story's rewards stay as they are.
- "Item drops": how likely defeated enemies are to leave each of their
  items, 0% to 1000% of the game's (most have a 2 to 10% chance); a
  battle still gives three kinds of items at most, as the game does.
- "Shop prices": what shops ask, 10% to 200% of the game's
  (`src/shop_prices.h`). Below 100% nothing sells for more than it costs.
- "Action gauge speed": how fast the party's action gauge fills again in
  battle, 25% to 400% of the game's.
- "Fast transporters", "Fast Revelation Flow": scenes that only make
  you wait play at up to twice the speed (the pace the 60 fps mode once
  played them at) (`src/quick_scenes.h`): a transporter's warp, and items
  going into a Revelation Flow cell. The music and the sounds keep their
  pace.
- "Free camera (F6)": the developers' free camera (`src/free_camera.h`),
  exploring or in battle. The party stands still and the controller moves
  the camera: the left stick moves the point it looks at (Square held,
  faster; L2 held, up and down), the right stick turns it around that
  point and tilts it (R2 held, nearer and farther). Start, F6 or the
  switch turns it off, and a notice says when it turns on or off. It is
  off at every start.
- "Instant text": dialogue and messages a page at a time instead of
  letter by letter, the game's own instant mode, which some scenes ask for
  (`src/instant_text.h`); the pauses written into the text stay.
- "Chests on the map": the chests not yet opened show on the maps (the
  corner one, the full one, the transporter's) in the parts explored, as
  the game shows them once every save point of a planet is activated
  (`src/map_chests.h`). Nothing is written to the save.
- "Developer tools": what the developers left in the game, switched off
  for release (`src/dev_tools.h`). In the main menu R3 shows every item's
  icon and R1 + R3 a list of them, and Cross gives one of the one picked;
  R1 + Select unlocks every help page and info entry; Select fills the
  bestiary; R2 + Triangle on the Revelation Flow gives one of each item a
  cell asks for;
  paused in a scene, Triangle + R1 + R2 ends it, also one the game would
  not let you skip, which can leave the story out of order.

The game's own Settings screen (System > Settings in the main menu) has
these too: L1 and R1 turn to a page of the switches (On / Off), one of the
numbers (Low takes a step off, High adds one; Display > Borders is there too,
Low and High going through Black, Blurred and Image) and one of the quick
scenes, each change kept as the menu's own are (`src/settings_pages.h`). Without
the F1 menu (`RG_GS=sw`, `RG_GS_NO_WINDOW=1`) the screen stays the game's.

F11 or Alt+Enter toggles fullscreen. The window opens as it was left: its
size and place, maximized or not, fullscreen or not (a place on a display
unplugged since opens centred). The settings live in
`%APPDATA%\RogueGalaxy\Recompiled\settings.ini` (`RG_SETTINGS_FILE` names
another file).

The game runs at 60 frames per second unless the menu's Game > 60 frames
per second is off (the game's own 30; a change takes effect at the
next start; `RG_FPS=30` or `RG_FPS=60` decides for one run). On a 144 Hz
monitor a frame lasts 2 or 3 refreshes at 60 and 4 or 5 at 30, which reads
as judder, so 120 Hz or variable refresh looks smoother.
