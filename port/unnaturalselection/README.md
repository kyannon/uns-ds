# Unnatural Selection (1993) — dual-screen port for ROCKNIX

The top screen shows the whole game. The bottom touch screen shows the whole game, its top half, or its bottom half, zoomed in so the buttons are easy to tap. A thin green bar along the bottom edge briefly shows where you are when you pan. The split falls at the top edge of the DigiLife floppy drive (row 120 of 200).

Outside the lab and battle, the layout follows what the game is showing:

- **Intro** (Maxis logo, title card): the same picture on both screens. Tap anywhere to skip ahead.
- **Desk** (main menu): the desk on the touch screen and the title card on the top screen.
- **Briefings and credits**: the picture monitor on the top screen and the text monitor on the touch screen, each with its bezel.

Shortly after each tap the pointer moves to the bottom-right corner and the game's cursor is painted out, so the picture stays clear.

The desk's title card is grabbed from the intro. On the very first run, taps during the intro wait until the title has appeared once.

The game runs in DOSBox Pure. This port uses the copy built into ROCKNIX.

## Install

1. Copy `UnnaturalSelection.sh` and the `unnaturalselection` folder into your ports folder, for example `/storage/roms/ports/`.
2. If `unnaturalselection/gamedata/` is empty, copy the DOS game files (`UNS.EXE`, `UNS_LAB.EXE`, `DATA`, `ISLANDS`, `SAVE`, …) into it.
3. Refresh the game list in EmulationStation and launch **UnnaturalSelection** from Ports.

## Controls

Each action is on both sides of the device (D-pad side and face-button side), so the buttons work with whichever hand isn't holding the stylus.

| Button | Action |
|---|---|
| Tap / drag (bottom screen) | Left mouse button |
| Hold **A** or **D-pad ▶** + tap, or tap with two fingers | Right mouse button (scroll at Zoom 2, fast-load cargo, fast-transfer in the Database) |
| **D-pad ▲** or **X** | Top half · press again for full screen |
| **D-pad ▼** or **B** | Bottom half · press again for full screen |
| **L1** / **R1** | Pan left (hold to scroll) |
| **L2** / **R2** | Pan right (hold to scroll) |
| Hold **Y** or **D-pad ◀** | Shift (vertical walls) |
| **Start** or **L3** | Click the floppy drive (File / Options / Variables / Battle menus) |
| **Select** or **BACK** | On-screen keyboard (for naming saves; also has Enter and Esc) |
| Hold **Home** | Quit |
| **Select + Start** | Quit |
| **Home + Select** | Swap the screens |

Saves go into `gamedata/save/`, like on the original PC. File > Exit in the game returns to the desk.

To leave, click the keyboard on the desk and choose EXIT. Tap once during the credits to skip them, and the port closes. Holding **Home** or pressing **Select + Start** quits at any time.

## Settings

All settings are in `usds.cfg`: the slice row, pan speed, scaling, button bindings, pointer parking and touch orientation. If touch lands in the wrong place, try `touch_swap_xy`, `touch_invert_x` and `touch_invert_y`. If the two screens come up the wrong way round, set `swap_screens = 1`, or set `USDS_SWAP_OUTPUTS=1` in the launcher.

`log.txt` is written on each run. It lists the outputs, the touch device that was picked, the input devices and the controller name (set `log_raw_buttons = 1` to also log presses of unbound buttons), which is the first thing to check when something misbehaves.

## Building

`usds` is a small libretro frontend (`src/usds.c`) that needs only SDL2 at run time. `cross/build_aarch64.sh` cross-builds it with clang and lld without a sysroot. It needs only the SDL2 headers (`SDL_INC=…`). `build_x86.sh` builds a desktop version for testing. That version shows both screens stacked in one window, and the mouse acts as the stylus.
