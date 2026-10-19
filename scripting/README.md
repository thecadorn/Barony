# Barony Lua scripting

Barony can load **script mods**: small Lua programs that react to game events and change the game through the `barony` API. This document is for people writing those mods.

## Installing a script mod

Each script mod is a folder with a `main.lua` in it, placed in the game's `scripts` folder:

| Platform | Scripts folder |
|---|---|
| macOS / Linux | `~/.barony/scripts/` |
| macOS / Linux, Epic Games build | `~/.barony/epicgames/scripts/` |
| Windows | `scripts\` inside the game's install folder |

For example, `~/.barony/scripts/hello_world/main.lua`. Create the `scripts` folder if it doesn't exist.

Script mods only run in a modded game, like the game's other mods. In the main menu, open **Mods**, go to the **Script Mods** tab, and use **Load Mod** / **Unload Mod** to choose which ones you want. The game remembers the choice, and newly installed mods start out chosen. Then press **Start Modded Game**: the chosen script mods load, in alphabetical order by folder name, and stay loaded until you unload mods (the **Unload Mods** button on the main menu, or leaving the mods menu with **Back**). A game started without going through the mods menu runs no scripts.

Script mods count as mods like any other: they're included in the mods menu's **Mods Loaded** count and in a lobby's number of mods, and a run with them is marked as modded. Because a script can change anything about the game, any script mod turns achievements off, as data mods that change gameplay do.

Each time you press **Start Modded Game**, script mods are loaded afresh, so that's how to pick up changes you've made to a script without restarting the game. The tab shows each mod's status: whether it's running, or has hit an error (the details are in `log.txt`).

### How a mod appears in the list

By default a mod is listed under its folder name. To give it a proper name and description, add a `mod.json` next to `main.lua`:

```json
{
	"name": "Sunburst",
	"description": "A new spell, built on Light, that permanently raises the caster's max HP.",
	"author": "Your name",
	"version": "1.0",
	"icon": "spell_light"
}
```

Every field is optional. The description is shown on one line, so keep it short. For the mod's picture, put a 64×64 `preview.png` in its folder; otherwise `icon` can name a spell whose picture to use, and mods with neither get a spellbook.

Everything a script does is written to the game's `log.txt` (in the same folder as `scripts`), prefixed with the mod's name, e.g. `[Lua:hello_world] hello world`. Errors in a script, including the line number, are logged there too.

## A first mod

`scripts/hello_world/main.lua`:

```lua
barony.onInit(function()
	barony.log("hello world")
end)
```

`barony.onInit` registers a function for the game to call later, once every script mod has loaded. Most of a mod is made of these: you register functions for the events you care about, and the game calls them.

## Examples

- [`examples/hello_world`](examples/hello_world/main.lua): logs a line when it loads.
- [`examples/sunburst`](examples/sunburst/main.lua): defines a custom spell, teaches it to every new character, puts it on the hotbar, and gives it its own effect (raising the caster's max HP).
- [`examples/resurrection`](examples/resurrection/main.lua): a multiplayer spell that brings a fallen ally back to life next to the caster, limited by a party-wide pool of charges.

## API reference

The complete, typed API is in [`barony.d.lua`](barony.d.lua). If your editor uses the Lua language server (the "Lua" extension in VS Code), point it at this folder to get autocomplete and type checking. In summary:

**Events**

| Function | Called when |
|---|---|
| `barony.onInit(fn())` | Once all script mods have loaded, when a modded game is started from the mods menu. Define custom spells here. |
| `barony.onGameStart(fn(player))` | A new run starts, after each local player's class loadout is set up. Not called when loading a save. |
| `barony.onBeforeGameplayStart(fn(player))` | A new run is fully set up, including the default hotbar, right before play begins. Arrange the hotbar here. |
| `barony.onSharedChange(name, fn(value))` | This machine's copy of a shared value changed: `setShared` on the host, an update reaching a client, a save loading, or values being cleared when a run starts or ends (`value` is then `nil`). Runs on every machine, so it's the place to keep HUD text up to date. Don't do gameplay here; it can run while a save is loading. |
| `barony.onFloorChange(fn(floor, previousFloor))` | The party arrived on a floor (numbered as the game shows them), before the game saves. Also called for going back up, secret levels and reloading a floor, so compare the numbers. Host only; not called when loading a save. |
| `barony.onBeforeSpellCast(fn(spellName, player))` | A spell is about to be cast, before any mana is spent. Return `false, "reason"` to refuse it; the caster sees the reason. Called on the caster's machine before the cast animation, and again on the host when it finishes, so only check things here, don't change them. The host's check decides. |
| `barony.onSpellCast(fn(spellName, player))` | Any spell is successfully cast. `player` is `nil` for monsters and traps. Return `false` to skip the spell's normal effect. |

**Functions and objects**

| Name | What it does |
|---|---|
| `barony.log(...)` | Writes to `log.txt`. `print` does the same. |
| `barony.version` | The game version, e.g. `"v5.0.2"`. |
| `barony.registerSpell{...}` | Defines a new spell based on an existing one. See below. |
| `barony.teachSpell(player, spellName)` | Teaches a player a spell. |
| `barony.player(player)` | The player's stats (`hp`, `maxhp`, `mp`, `maxmp`), whether they're `alive` (or their `state`: `"alive"`, `"dying"` or `"dead"`), their position (`x`, `y`), and `revive(x, y)` to bring them back to life when `canRevive` is true. |
| `barony.maxPlayers` | How many player slots there are; players are numbered 0 to `maxPlayers - 1`. |
| `barony.message(player, text)` | Shows a line in a player's in-game message log. |
| `barony.setHudText(id, text)` | Shows a line of text (up to 128 characters) in the bottom right of every local player's HUD, or removes it when `text` is `nil`. Calling it again with the same `id` replaces the line. Only affects this machine. At most 8 lines across all mods; they're removed when the run ends. |
| `barony.isHost()` | `true` on the host and in single player, where gameplay is decided. |
| `barony.setShared(name, value)` / `barony.getShared(name)` | A named number for the current run (for example a party-wide counter). Saved with the game, cleared when a run starts or ends. Only the host can set it. Clients can read it, but get the host's value as of the last update (`nil` until the first one arrives). |
| `barony.hotbar(player)` | The player's hotbar: `getSlot`, `setSlot`, `clearSlot`, `firstEmptySlot`. Slots are numbered 0–9. |
| `barony.inventory(player)` | The player's inventory: `findSpell(spellName)`. |

Players are numbered from 0. Spells are referred to by their internal names, such as `"spell_light"` or `"spell_fireball"`. The game's own spell names are listed in `items/items.json` in the game data.

## Custom spells

A custom spell is a copy of an existing spell with its own name, cost and tooltip:

```lua
barony.onInit(function()
	barony.registerSpell{
		id = "spell_sunburst",       -- your spell's internal name
		base = "spell_light",        -- the spell it starts as a copy of
		name = "Sunburst",           -- shown in the game
		mana = 5,
		icon = "spell_light",        -- whose picture to show
		effect = { "Permanently increases", "maximum HP by 10" },
		description = { "Press [Cast] while equipped", "to cast." },
		tooltipWidth = 316,
	}
end)
```

- The new spell does whatever `base` does when cast. To give it a different effect, handle it in `barony.onSpellCast` and `return false` to skip the base spell's effect.
- Tooltip text is not copied from `base`: write your own `effect` and `description`. Words in `[brackets]` are highlighted, like in the game's own tooltips.
- Spells can only be defined in `onInit` (or while `main.lua` first runs).
- Custom spells can't appear on spellbooks or staffs yet. Use `barony.teachSpell` to give them to players.

## Multiplayer

**Every player must have the same script mods installed and turned on.** Spells defined by scripts are identified by number in multiplayer, and the numbers come from which mods are loaded, so mismatched mods mean casts that don't work or that act as the wrong spell. The game does not check this yet.

Where things run:

- `onFloorChange` runs only on the host.
- `onSharedChange` runs on every machine, whenever that machine's copy of the value changes. `setHudText` only affects the machine it runs on.
- `onBeforeSpellCast` runs on the caster's machine before the cast animation, then on the host.
- `onSpellCast` runs only on the host, for every player's casts. Changes it makes (such as `player.maxhp`) are sent to everyone automatically.
- `onGameStart` and `onBeforeGameplayStart` run on each player's own machine, for that machine's players. Teaching spells and arranging the hotbar happen there.
- Player stats can only be changed on the host.

## Saved games

Saves remember custom spells by name, so adding or removing other script mods doesn't mix them up. If a save contains a custom spell whose mod is no longer installed or is turned off, that spell is removed from the character when the save loads.

## The sandbox

Script mods are sandboxed so that a mod downloaded from someone else can't harm your computer:

- No access to files, the operating system, or other programs: Lua's `io`, `os`, `require`, `load`, `dofile` and `debug` libraries are not available.
- Available: the basic Lua functions plus the `string`, `table`, `math`, `utf8` and `coroutine` libraries.
- Each mod has its own global variables; mods can't see or change each other's.
- A script that runs too long (for example an infinite loop) is stopped, and all scripts together are limited to 64 MB of memory.
- A script error doesn't crash the game: it's written to `log.txt` and the game carries on.
