# DSPad

Xbox (XInput) controller support for **Dungeon Siege: Legends of Aranna** (`DSLOA.exe` version 1.50), aimed at playing it like a twin-stick action RPG: you steer one hero directly with the left stick, the rest of the party follows and fights alongside.

It is a single DLL. No game files are modified on disk.

> Single-player only. The mod places the hero itself every frame, which a multiplayer session would not accept.

## Video

Raw gameplay with the mod:

[![DSPad gameplay video](https://img.youtube.com/vi/6PtO3ccN4Xw/hqdefault.jpg)](https://youtu.be/6PtO3ccN4Xw)

https://youtu.be/6PtO3ccN4Xw

## Features

- **Direct 360° movement** of the controlled hero with the left stick, checked against the game's own walk mesh (lifts work, furniture and props block the way as they do for mouse movement).
- **Party follow**: companions follow the controlled hero, join its attacks and respect their combat orders (attack freely / defend / hold).
- **Party auto potions**: companions drink their own health or mana potions below a threshold (default 30%). The controlled hero never does.
- **Targeting**: the nearest enemy in the stick direction is highlighted and attacked; enemies take priority over levers, chests and other interactables. Target cycling on R3.
- **Target readout**: the name / health bar at the bottom of the screen follows the pad target instead of the mouse.
- **Mini map** in place of the compass, with the party marker and the icons of the game's big map.
- **Ground item stats**: a stats box for the targeted item on the ground (equippable items only).
- **Compare with equipped**: item stats boxes in the inventory also list what the hero wears in the same slot.
- **Menu navigation**: d-pad / left stick snaps between buttons and inventory slots, right stick moves the cursor freely.
- **UI scaling** (default 1.5×) for high resolutions; the 3D scene is untouched.
- **Camera lock** on the controlled hero (cutscenes are left alone).
- **Mouse pointer hidden** during play while the pad is in use; it comes back in menus or when the mouse moves.

Every feature has a switch in `dspad.ini`.

## Controls

### In game

| Input | Action |
|---|---|
| Left stick | Move the controlled hero |
| Right stick | Rotate / tilt camera |
| D-pad up / down | Zoom in / out |
| D-pad left / right | Previous / next party member |
| A or RT (hold) | Attack the highlighted target |
| X (tap) | Pick up / use / talk to the nearest thing |
| X (hold) | Whole party collects loot |
| B | Stop, drop the target lock |
| Y | Cycle weapon |
| LB / RB | Drink health / mana potion |
| R3 | Cycle target |
| L3 | Big map |
| Back | Inventory |
| Start | Game menu |
| LT + A, B, X, Y, d-pad up, right, down, left | Weapon / spell configurations 1–8 |
| LT + LB | Select whole party |
| LT + RB | Spell book |
| LT + Back | Journal |
| LT + R3 | Pause |

### In menus and the inventory

| Input | Action |
|---|---|
| D-pad / left stick | Snap to the next button or slot |
| Right stick | Move the cursor freely |
| A / X | Left / right mouse button |
| LT / RT (hold) | Shift / Ctrl |
| B or Start | Close (Esc) |
| Y | Spell book |
| Back | Inventory |
| LB / RB | Drink health / mana potion |
| L3 / R3 | Big map / pause |

Keyboard-driven actions are sent as key presses, so the `[Keys]` section of `dspad.ini` has to match your in-game key bindings (it ships with the game defaults).

## Requirements

- Dungeon Siege: Legends of Aranna, `DSLOA.exe` **version 1.50**. The mod uses fixed addresses inside that executable; other builds, and the original `DungeonSiege.exe`, are not supported.
- An XInput controller.
- Developed and tested on Windows with the game running through dgVoodoo2. Other setups have not been tried.

## Install

The game loads its sound library `Mss32.dll` from the game folder. DSPad takes that name and passes every sound call on to the original, which has to be renamed.

1. Download the release zip and put the `DSPad` folder from it inside the game folder (next to `DSLOA.exe`).
2. Run `DSPad\install.bat`.

Or by hand, in the game folder:

1. Rename the game's `Mss32.dll` to `mss32o.dll`.
2. Copy the mod's `Mss32.dll` and `dspad.ini` there.

To uninstall, run `DSPad\uninstall.bat`, or delete the mod's `Mss32.dll` and rename `mss32o.dll` back to `Mss32.dll`.

## Configuration

`dspad.ini` in the game folder is read at start-up; each key is explained by a comment in the file. The ones most likely to be changed:

| Key | Default | Meaning |
|---|---|---|
| `UIScale` | 1.5 | Interface scale; 1.0 turns it off |
| `MiniMap` | 1 | Mini map instead of the compass |
| `MiniMapSize`, `MiniMapMeters` | 200, 60 | Size in pixels and metres shown across |
| `CameraLockToLeader` | 1 | Camera stays exactly on the controlled hero |
| `PartyFollow`, `FollowDistance` | 1, 3.5 | Companions follow, and from how far |
| `PartyAutoPotion`, `PartyAutoPotionPercent` | 1, 30 | Companions drink potions below this percentage |
| `GroundItemInfo` | 1 | Stats box for items on the ground |
| `CompareWithEquipped` | 1 | Show the equipped item in stats boxes |
| `TargetInfoBar` | 1 | Bottom readout follows the pad target |
| `HideCursorWithPad` | 1 | Hide the mouse pointer during pad play |
| `MoveMode` | 2 | 2 direct control, 1 steer through the movement planner, 0 plain move orders |
| `StickXSign` | 0 | Set to 1 if left and right are mirrored |
| `Log` | 1 | Write `dspad.log` in the game folder; 0 turns it off |

## Building

One C file, built with 32-bit MinGW-w64:

```
./build.sh
```

or directly:

```
i686-w64-mingw32-gcc -O2 -fno-omit-frame-pointer -Wall -Wextra -shared -static -static-libgcc \
    -o Mss32.dll src/dspad.c src/mss32.def -lwinmm
```

`src/mss32.def` forwards the 351 Miles Sound System exports to `mss32o.dll`.

## How it works

- `Mss32.dll` is a proxy for the game's sound library, which gets the mod loaded into the game process.
- A hook on `PeekMessageA` in the game's import table gives a per-frame callback on the main thread.
- The game is driven through its own exported script-binding (FuBi) functions, such as `GoMind::RSDoJob`, plus a small number of patched call sites and direct reads of engine structures at fixed addresses. That is why only the 1.50 executable works.
- Direct movement tests each step against the walk mesh of the terrain nodes (floor triangles, node joins, occupied leaves) and places the hero exactly, without the engine's own re-snapping.
- Menu actions are synthesized mouse and keyboard input.

`tools/` holds the small Python helpers used while reverse engineering (`pefile` and `capstone` needed). They expect `DSLOA.exe` in the current directory; run `exports.py` first to produce `exports.txt`.

## Troubleshooting

- `dspad.log` in the game folder records what the mod found and did. The first line names the build.
- Left and right swapped: set `StickXSign`.
- A button does nothing: check that `[Keys]` matches your in-game bindings.

## AI disclosure

The code in this repository, the reverse engineering behind it and this README were written by an AI assistant (Anthropic's Claude), directed by the repository owner, who set the requirements and tested every build by playing the game. The code has not been reviewed line by line by a human, so treat it accordingly.

## Disclaimer

Unofficial fan project, not affiliated with or endorsed by Gas Powered Games or Microsoft. No game code or assets are included; you need your own copy of the game.
