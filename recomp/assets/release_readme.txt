PokePark Wii: Pikachu's Adventure - PC port (open beta)

A native Windows version of the game, recompiled from the Wii original.
No game data is included: you need your own copy of the game.

Getting started
- Windows 10 or 11, 64-bit, and a graphics card with Vulkan 1.3 or
  OpenGL 3.3.
- Run PokePark.exe. The first time, it asks for your disc image of
  PokePark Wii: Pikachu's Adventure (USA, R8AE01): an ISO, RVZ, WBFS
  or the like. It sets the game up in an R8AE01 folder beside the
  program; your saves are kept in R8AE01\nand.
- Keep the prompts and tools folders beside PokePark.exe.

Steam Deck
- Not tested yet, but it should run through Proton: in Desktop Mode, add
  PokePark.exe to Steam as a non-Steam game, and in its Properties >
  Compatibility, force the latest Proton. Then set Options > Display >
  Aspect ratio to 16:10 and turn on Fullscreen.

Settings
- Options are in the pause menu (and on the save files screen):
  display, keyboard, controller and gameplay. They are saved in
  settings.ini beside the program.
- Renderer: Vulkan by default; OpenGL in Options > Display (from the
  next start). If Vulkan can't start, OpenGL is used.
- Xbox-style controllers and keyboard and mouse are supported.
- Updates: at start it asks GitHub whether a newer version is out, and
  if so, Options > Display > Updates opens its download page. Turn the
  Updates setting off to stop it asking.

This is a beta
Some things aren't done yet: a few rare graphics effects, the Wii
Remote speaker, and the Wii Remote pointer and Nunchuk (the game
doesn't need them). It has been tested on one PC so far, so problems
on other hardware are likely.

Reporting problems: open an issue on the project's GitHub page. Say
what you did and what happened, attach a screenshot if you can, and
give your Windows version, graphics card, and the Renderer setting.

Credits
- Button icons by Icons8 (https://icons8.com).
- Disc images are read with decomp-toolkit (tools\dtk.exe); its license
  is in tools\dtk_license.txt.
- PokePark Wii is (c) Nintendo, Creatures Inc., GAME FREAK inc. and The
  Pokemon Company. This project is not affiliated with or endorsed by
  them.
