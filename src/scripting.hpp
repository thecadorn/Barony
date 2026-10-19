/*-------------------------------------------------------------------------------

	BARONY
	File: scripting.hpp
	Desc: Lua scripting engine for mods

-------------------------------------------------------------------------------*/

#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

struct spell_t;

// Lua mods live in <outputdir>/scripts/<modname>/main.lua (e.g. ~/.barony/scripts/hello_world/main.lua).
// Each mod's main.lua runs in its own sandboxed environment and registers callbacks
// through the typed `barony` API (barony.onInit, ...). See scripting/barony.d.lua.
//
// Multiplayer: every machine runs the same scripts. Gameplay callbacks only fire where the
// game simulates the event, i.e. on the host or in singleplayer.
namespace Scripting
{
	// Script-defined spells get IDs in [kCustomSpellIDStart, kCustomSpellIDEnd), assigned in
	// registration order. SPELL_ITEM appearances >= 1000 mean shapeshift spells, so the range
	// must stay below 1000.
	static const int kCustomSpellIDStart = 500;
	static const int kCustomSpellIDEnd = 1000;

	// Creates the Lua VM, loads every script mod, then fires the onInit callbacks.
	// Called once after initGame() succeeds.
	void init();

	// Destroys the Lua VM. Called before deinitGame().
	void deinit();

	// Re-adds script-defined spells after setupSpells() rebuilds the spell tables.
	void reapplyCustomSpells();

	// Save games store spells by number, but custom spell numbers depend on which script mods
	// are installed. Saves therefore carry a name table (in SaveGameInfo::additional_data):
	// savedSpellTable() produces it, savedSpellRemap() turns a save's table into
	// saved ID -> current ID (SPELL_NONE when that spell's mod is no longer installed).
	std::vector<std::pair<std::string, std::string>> savedSpellTable();
	std::map<int, int> savedSpellRemap(const std::vector<std::pair<std::string, std::string>>& additionalData);

	// Re-adds script-defined spells' tooltip text after readTooltipsFromFile() reloads templates.
	void reapplyCustomSpellTooltips();

	// Fires onSpellCast once a cast has passed the fizzle checks and mana is paid. `player` is
	// the casting player's index, or -1 for monsters and traps. Host only. Returns false if a
	// callback returned false, meaning the spell's default effect should be skipped.
	bool onSpellCast(spell_t* spell, int player);

	// Fires onGameStart for a local player whose character was just set up for a new run
	// (not when loading a save). Runs on every machine, for that machine's own players.
	void onGameStart(int player);

	// Fires onBeforeGameplayStart for a local player once a new run is fully set up
	// (class loadout and hotbar layout), right before gameplay begins.
	void onBeforeGameplayStart(int player);
}
