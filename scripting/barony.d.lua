---@meta
-- Type definitions for the Barony Lua modding API.
-- Point your editor's Lua language server (LuaLS / sumneko) at this folder
-- for autocomplete and type checking in script mods.
--
-- Script mods live in <barony user folder>/scripts/<modname>/main.lua
-- (on macOS/Linux: ~/.barony/scripts/<modname>/main.lua).
-- Each mod runs in its own sandbox: io, os, require, load and debug are unavailable.

---@class barony
barony = {}

---The game version string, e.g. "v5.0.2".
---@type string
barony.version = ""

---How many player slots the game has (players are numbered 0 to maxPlayers - 1).
---@type integer
barony.maxPlayers = 4

---Writes a line to the game's log.txt, prefixed with the mod's name.
---Arguments are converted with tostring and joined by spaces. `print` is an alias.
---@param ... any
function barony.log(...) end

---Registers a function to run once at game startup, after every script mod has loaded.
---@param callback fun()
function barony.onInit(callback) end

---Registers a function to run whenever a spell is successfully cast (after fizzle checks).
---`spellName` is the spell's internal name, e.g. "spell_light" or "spell_healing".
---`player` is the caster's player index (0-based), or nil for monsters and traps.
---Multiplayer: only fires on the host (or in singleplayer), for every player's casts.
---Return false to skip the spell's default effect (mana is still spent); return nothing
---or true to let it happen. Every registered callback runs either way.
---@param callback fun(spellName: string, player: integer?): boolean?
function barony.onSpellCast(callback) end

---Registers a function to run when a spell is about to be cast, before any mana is spent.
---Return false to reject the cast, optionally followed by a message shown to the caster:
---    return false, "You can't do that right now."
---Return nothing to allow it. Rejection stops at the first callback that rejects.
---`player` is the caster's player index, or nil for monsters and traps.
---Multiplayer: runs on the host (like onSpellCast), so it can read shared values. A client's
---cast animation has already played by then; only the cost and the effect are prevented.
---@param callback fun(spellName: string, player: integer?): boolean?, string?
function barony.onBeforeSpellCast(callback) end

---Registers a function to run for each local player when a new run starts
---(after their class loadout is set up; not when loading a saved game).
---Runs on every machine, for that machine's own players.
---@param callback fun(player: integer)
function barony.onGameStart(callback) end

---Teaches a local player a spell by its internal name, e.g. "spell_light".
---Returns true if the spell was learned, false if the player isn't local or already knows it.
---@param player integer
---@param spellName string
---@return boolean
function barony.teachSpell(player, spellName) end

---@class SpellDefinition
---@field id string           Unique internal name for the new spell, e.g. "spell_sunburst" (letters, digits, _)
---@field base string         Internal name of the existing spell to clone, e.g. "spell_light"
---@field name string         Display name shown in the game
---@field mana integer?       Mana cost (defaults to the base spell's)
---@field duration integer?   Effect duration in ticks, 50 per second (defaults to the base spell's)
---@field difficulty integer? Skill needed to learn it normally (defaults to the base spell's)
---@field icon string?       Internal name of the spell whose picture to show (defaults to `base`)
---@field effect string|string[]?      Tooltip summary shown next to the effect icon, one entry per line
---@field description string|string[]? Tooltip usage text below the summary, one entry per line
---@field tooltipWidth integer?      Minimum tooltip width in pixels (game default 280; Light uses 316)
---Tooltip text supports the game's [Keyword] highlighting. Nothing is inherited from the
---base spell: leave `effect`/`description` out and those tooltip sections stay empty.

---Defines a new spell by cloning an existing one; it behaves like `base` but has its own
---name, stats and identity (so onSpellCast sees `def.id`). Only callable while scripts load
---or in onInit. Returns the new spell's numeric ID.
---Multiplayer: every player must run the same script mods so spell IDs match.
---@param def SpellDefinition
---@return integer
function barony.registerSpell(def) end

---Registers a function to run for each local player once a new run is fully set up
---(class loadout and hotbar layout), right before gameplay begins. Use it to arrange
---the hotbar: changes made in onGameStart are overwritten by the default hotbar layout.
---@param callback fun(player: integer)
function barony.onBeforeGameplayStart(callback) end

---Returns a player's hotbar, or nil if that player isn't local to this machine.
---@param player integer
---@return Hotbar?
function barony.hotbar(player) end

---Returns a player's inventory, or nil if that player isn't local to this machine.
---@param player integer
---@return Inventory?
function barony.inventory(player) end

---A player's hotbar. Slots are numbered 0-9, left to right.
---@class Hotbar
---@field size integer Number of slots (10)
local Hotbar = {}

---Returns the item in a slot, or nil if the slot is empty.
---@param slot integer 0-9
---@return Item?
function Hotbar:getSlot(slot) end

---Puts one of the player's own items in a slot (replacing whatever was there).
---@param slot integer 0-9
---@param item Item
function Hotbar:setSlot(slot, item) end

---Empties a slot.
---@param slot integer 0-9
function Hotbar:clearSlot(slot) end

---Returns the lowest-numbered empty slot, or nil if the hotbar is full.
---@return integer?
function Hotbar:firstEmptySlot() end

---A player's inventory.
---@class Inventory
local Inventory = {}

---Returns the inventory item for a spell the player knows, or nil if they don't know it.
---@param spellName string Internal spell name, e.g. "spell_sunburst"
---@return Item?
function Inventory:findSpell(spellName) end

---An item in a player's inventory. Handles refer to the item by UID: once the item leaves
---the inventory, `valid` becomes false and reading other fields raises an error.
---@class Item
---@field valid boolean      Whether the item is still in the inventory
---@field uid integer        The item's unique ID
---@field name string        Internal item name, e.g. "spell_item" or "quarterstaff"
---@field isSpell boolean    Whether this item is a learned spell
---@field spellName string?  For spells, the internal spell name (e.g. "spell_light"); otherwise nil
local Item = {}

---Returns a player in the game, or nil if that player slot is empty.
---@param player integer 0-based player index
---@return Player?
function barony.player(player) end

---A player's core stats. Readable everywhere; writable only on the host (where gameplay
---callbacks like onSpellCast run). Writes are clamped (hp/mp to 0..max, maxhp >= 1,
---maxmp >= 0) and sent to every client immediately.
---@class Player
---@field index integer  The player's 0-based index (read-only)
---@field hp integer     Current health
---@field maxhp integer  Maximum health
---@field mp integer     Current mana
---@field maxmp integer  Maximum mana
---@field alive boolean  Whether the player is alive (false while dead or a ghost)
---@field state "alive"|"dying"|"dead" "dying" while the death animation plays; "dead" once the body is gone (death screen or ghost)
---@field canRevive boolean Whether revive() would succeed right now: fully dead and still connected
---@field x number?      World X position (16 units per tile), or nil while dead
---@field y number?      World Y position (16 units per tile), or nil while dead
local Player = {}

---Brings a dead player back to life on a free tile next to (x, y), with half their HP and
---mana. If nobody has opened the loot bag they dropped on death, the bag disappears and they
---keep all their items, still equipped; otherwise whoever opened it keeps them.
---Host only (e.g. from onSpellCast). Returns false unless `canRevive` is true.
---@param x number World X position, e.g. another player's `x`
---@param y number World Y position
---@return boolean
function Player:revive(x, y) end

---Returns true on the host (and in singleplayer), where gameplay is decided and where
---onSpellCast runs. Use it to run host-only code from events that fire on every machine.
---@return boolean
function barony.isHost() end

---Stores a named number for the current run, e.g. a party-wide counter. Pass nil to remove it.
---Values are cleared when a new run starts and are saved with the game.
---Host only: shared values are not sent to clients yet, so calling this on a client errors.
---@param name string 1-64 letters, digits or _
---@param value number?
function barony.setShared(name, value) end

---Returns a number stored with barony.setShared, or nil if it isn't set. Host only.
---@param name string
---@return number?
function barony.getShared(name) end

---Shows a line of text in a player's in-game message log (up to 255 characters).
---On the host this also reaches players on other machines.
---@param player integer
---@param text string
function barony.message(player, text) end
