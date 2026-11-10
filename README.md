# PokePark Wiicompiled

A native Windows and Android port of *PokePark Wii: Pikachu's Adventure*
(USA, R8AE01).
The game's PowerPC code is statically recompiled into C and runs natively,
with the Wii's hardware replaced underneath it. There is no emulator: the
game's own code runs, drawing with Vulkan or OpenGL at high resolutions and
frame rates, played with a keyboard and mouse, a controller, or a touch
screen.

**This repository contains no game data.** You need your own copy of the
game. The program reads your disc image the first time it runs.

> **Open beta.** Please report problems in [Issues](../../issues). Say what
> you did and what happened, attach a screenshot if you can, and give your
> Windows version, graphics card and Renderer setting (Options > Display),
> or on Android your phone's model and Android version.

## Playing on Windows

1. Download the latest build from [Releases](../../releases) and unzip it.
2. Run `PokePark.exe`.
3. The first time, choose your disc image (ISO, RVZ, WBFS, WIA, CISO and
   the like). It's checked against the original disc, then the game is set
   up in an `R8AE01` folder beside the program, and your saves are kept in
   `R8AE01\nand`. WBFS and scrubbed copies work, after a warning; a
   patched or modded `main.dol` doesn't.

Keep the `prompts` and `tools` folders beside `PokePark.exe`.

**Requirements:** Windows 10 or 11 (64-bit) and a graphics card with
Vulkan 1.3 or OpenGL 3.3.

## Playing on Android

1. Download the `.apk` from [Releases](../../releases) on your phone and
   install it (allow installing apps from your browser or file manager
   when asked).
2. Open **PokePark**.
3. The first time, choose your disc image (RVZ, ISO, WBFS, WIA, CISO and
   the like) in the file picker. The game plays straight from it, so
   nothing is extracted; if you move the file, the app asks again.

Play with the touch controls drawn over the game (a stick on the left,
the buttons on the right, -, + and View at the top left, drag elsewhere
to turn the camera) or a controller. Settings are in Options, in the
PokéPark Pad.

**Dual-screen handhelds.** On a handheld with a second built-in screen,
such as the AYN Thor, the second screen becomes the PokéPark Pad: your
friends list (scroll it by touch) and the zone's map, with its name,
your berries and the friends who live there. Options > Dual screen,
shown only on these handhelds, turns it on or off, swaps the screens,
keeps the minimap on the game's screen too, and can show the map
without the friends list. The Retroid Dual Screen Add-On should work
the same way, but hasn't been tested.

**Requirements:** Android 11 or later on a 64-bit (arm64) phone or
handheld with Vulkan 1.3. Tested on a phone (Adreno 830 GPU) and the AYN
Thor so far.

## Improvements over the Wii version

### Graphics

- **Widescreen.** The game runs in its native 16:9 mode, with its
  widescreen pictures and layouts, at full horizontal resolution rather
  than squeezed for a TV to stretch.
- **High resolution.** Renders at 1x to 6x the Wii's resolution, or to
  fit the window. 3x is pixel for pixel on a 1440p display.
- **High frame rates.** 30 fps (the game's own), 60 fps, or 120, 144,
  165 or 240 fps. Above 60, extra frames are interpolated between the
  game's, with every object, camera and light moving smoothly. The game
  still updates 60 times a second, so nothing speeds up.
- **Anti-aliasing.** 2x, 4x or 8x MSAA.
- **Anisotropic filtering.** 2x to 16x, for sharp ground and walls at
  an angle.
- **Aspect ratio.** 16:9 (the game's), its own 4:3, or 16:10 and 21:9
  ultrawide, which show more of the park above and below or at the
  sides. The HUD and menus stay at 16:9 in the middle.
- **Field of view.** 80% to 150% of the game's, in the park.
- **Texture packs.** Packs made for Dolphin work as they are: put the
  pack's folder in a `textures` folder beside the game (on Android, the
  `textures` folder beside `R8AE01`). Options > Display > Texture pack
  turns it off and on.
- **Vulkan and OpenGL renderers.** Vulkan is the default and the
  fastest. It falls back to OpenGL if Vulkan can't start.
- **Fullscreen** (borderless) or windowed, toggled with Alt+Enter.

### Controls

- **Keyboard and mouse**, with a **mouse-controlled camera** that turns
  and tilts the player camera, even while running.
- **Xbox-style controllers**, with rumble (off, light, normal or strong).
- **Fully rebindable.** Each Wii Remote button has two keyboard or mouse
  slots and two controller slots.
- **Button prompts match your device.** The game's on-screen Wii Remote
  icons are redrawn as the key or button they're bound to, switching
  between keyboard and controller as you use them. The title screen,
  the minigames' instructions and Primeape's Iron Tail tip name your
  keys or buttons too, instead of telling you to shake or point the
  remote.
- **First person without a pointer.** The game's first-person view,
  which on the Wii needed the remote pointed at the screen, is a button
  (the right stick's click, or View on Android's touch controls).
- **Motion controls mapped.** The minigames' shaking, tilting, swinging
  and pointing are mapped to keys, the mouse and the sticks.
- Camera speed and inverted camera settings.

### Gameplay options

Optional changes to the game's balance, all off (100%) by default:

| Setting | Choices |
|---|---|
| Text speed | 1x (the game's), 2x, 3x, or a page at once |
| Berries collected | 50% to 300% |
| Prices | free, 50%, 100% or 200% |
| Damage dealt | 50% to 300% |
| Damage taken | none, 50% to 300% |
| Chase speed | 50% to 150% of the speed of the Pokémon you chase |

### Quality of life

- **Faster start-up.** The title screen comes up about 7 s after launch
  instead of about 28 s.
- **An in-game Options menu.** It's in the PokéPark Pad (pause menu) and
  on the save files screen, in the game's own style. Changes apply
  immediately and are saved in `settings.ini`.
- **Save photos anywhere.** Copying photos to the SD card (from
  Misdreavus's album) asks for a folder, and the photos are saved
  there as JPEGs.
- **Autosave.** With Options > Gameplay > Autosave on, the game saves
  itself, as the PokéPark Pad's Save does, whenever you make a friend
  or enter another area. It's off by default.
- **Unlock all passwords.** One button on the Password screen gives that
  save file what all seven passwords give, and the game saves it.
- **Update check.** At start it asks GitHub whether a newer release is
  out. If one is, the save files screen says so, and Options > Display >
  Updates opens its download page. Nothing is downloaded by itself, and
  the Updates setting turns the check off.
- Runs at 60 Hz to match PC displays (the Wii runs at 59.94 Hz).

## Default controls

| Wii Remote | In the park | Keyboard and mouse | Controller |
|---|---|---|---|
| D-pad | Move | WASD / arrow keys | D-pad / left stick |
| 2 | Jump, talk | Space / X | A |
| 1 | Dash | Left click / Z | X |
| A | Thunderbolt | Right click / Enter | B |
| B | Recenter camera | V / Left Ctrl | Y / right trigger |
| | Turn camera | Mouse | |
| + | PokéPark Pad | P | Start |
| - | Map | M | Back |
| | First person | Home / H | Right stick click |
| Shake | Iron Tail (in battles) | Q, Left Shift or E | LB or RB |
| Shake (minigames) | Running, swinging | Q and E by turns | LB and RB by turns |

In first person, the mouse or right stick looks around and - takes a
Photo. The same button leaves it, as does any other button but + and -.

While the window has focus, the mouse is captured for the camera. Esc
releases it, and a click captures it again.

<details>
<summary>Minigame controls</summary>

| Minigame | Keyboard and mouse | Controller |
|---|---|---|
| Absol's Hurdle Bounce, Bulbasaur's Daring Dash, Dusknoir's Speed Slam | Run: Q and E by turns | LB and RB by turns |
| Venusaur's Vine Swing, Tangrowth's Swing-Along | Swing: Q and E by turns, in time. Jump: Space | LB and RB. A |
| Blaziken's Boulder Bash | Swing: left click or Space. Recover: Q and E by turns | X or A. LB and RB |
| Gyarados' Aqua Dash, Empoleon's Snow Slide | Steer: A / D | Left stick |
| Pelipper's Circle Circuit, Salamence's Air Race | Steer: A / D. Climb and dive: W / S | Left stick |
| Rhyperior's Bumper Burn | Roll: WASD | Left stick |
| Rayquaza's Balloon Panic | As flying, then aim with the mouse | Left stick, then right stick |
| Rotom's Spooky Shoot-'em-Up | Aim: mouse | Right stick |
| Bastiodon's Panel Crush | Buttons, as on the Wii | Buttons |

</details>

## Known gaps

- **Graphics:** logic ops, z-textures, and lines and points aren't
  drawn yet (no area tested so far uses them).
- **Sound:** the Wii Remote speaker is silent. The game's own effects
  (such as reverb) are mixed in, but no area tested so far uses them, so
  that is untested.
- **Input:** no Wii Remote pointer or Nunchuk. The game is played
  sideways, so neither is needed for the main game.
- **Android:** no rumble yet.
- It has been tested on one PC, one phone and the AYN Thor, so problems
  on other hardware are likely: please report them.

## Building

Needs Windows 10 or 11, Visual Studio 2022 (C++ desktop workload),
Python 3 and, for the Vulkan renderer, the
[Vulkan SDK](https://vulkan.lunarg.com/). Without the SDK, only OpenGL is
built.

1. Get [decomp-toolkit](https://github.com/encounter/decomp-toolkit/releases)
   v1.6.2 and put it at `build/tools/dtk.exe`.
2. Extract your disc image:
   ```
   build\tools\dtk.exe disc extract "<your disc image>" orig\R8AE01\disc
   ```
3. Recompile and build:
   ```
   python tools/recomp/recomp.py
   python tools/recomp/build.py --run test
   python tools/recomp/build.py --public
   ```
   `--public` puts a release build in `Releases/`. Without it you get a
   development build at `build/recomp/boot.exe`, which also has a debug
   settings menu (Backspace, or Back+Start on a controller) that can start
   any minigame.

The Android app is built with `python tools/recomp/build.py --android`,
which needs the Android SDK and NDK and Rust; see
[recomp/README.md](recomp/README.md#android).

How it works, and the environment variables for testing, are described
in [recomp/README.md](recomp/README.md).

## Credits

- The function list (`config/R8AE01/symbols.txt`) and the decompiled
  sources in `src/` come from the
  [PokePark Wii decompilation](https://github.com/SephDB/pokepark-wii-decomp)
  by Seph De Busser and contributors.
- Disc images are read with
  [decomp-toolkit](https://github.com/encounter/decomp-toolkit) on
  Windows and [nod](https://github.com/encounter/nod) on Android, both by
  Luke Street and contributors (MIT or Apache-2.0; licenses in
  `recomp/assets`).
- The disc check follows [SparkPark](https://github.com/Dvrkie13/SparkPark)
  by Dvrkie13 (CC0).
- Button icons by [Icons8](https://icons8.com).

PokePark Wii is © Nintendo, Creatures Inc., GAME FREAK inc. and The
Pokémon Company. This project is not affiliated with or endorsed by them.

Released under CC0 1.0 (see `LICENSE`).
