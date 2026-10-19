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

	// Script mods only run in a modded game, like data mods: init() runs when the player starts
	// one from the main menu's mods window, and deinit() when they unload mods (and at shutdown).
	// Both are main menu only, since custom spell IDs depend on which mods are loaded.

	// Creates the Lua VM, loads every script mod that's turned on, then fires the onInit callbacks.
	void init();

	// Destroys the Lua VM and removes the spells script mods defined.
	void deinit();

	// Script mods can be turned off in the mods window. Turned-off mods are remembered by
	// folder name in the game's settings, and init() skips them.
	//
	// What the mods window shows comes from an optional mod.json next to main.lua, read without
	// running any script: { "name", "description", "author", "version", "icon" }, all strings.
	// "icon" names a spell whose picture to show; a preview.png in the folder takes precedence.
	struct ModFolder
	{
		std::string name;        // folder name, which identifies the mod
		std::string title;       // mod.json "name", or the folder name made readable
		std::string description;
		std::string author;
		std::string version;
		std::string iconSpell;   // mod.json "icon"
		bool enabled = true;
		bool loaded = false;     // its main.lua ran in the current Lua VM
		bool hadError = false;   // it has raised an error since loading (details in log.txt)
	};
	std::string scriptsDirectory();
	std::vector<ModFolder> listModFolders(); // folders in scriptsDirectory() with a main.lua, alphabetical
	size_t loadedModCount();
	std::vector<std::string> disabledMods();
	void setDisabledMods(const std::vector<std::string>& names); // these take effect at the next init()
	void setModEnabled(const std::string& name, bool enabled);

	// Re-adds script-defined spells after setupSpells() rebuilds the spell tables.
	void reapplyCustomSpells();

	// Save games store spells by number, but custom spell numbers depend on which script mods
	// are installed. Saves therefore carry a name table (in SaveGameInfo::additional_data):
	// savedSpellTable() produces it, savedSpellRemap() turns a save's table into
	// saved ID -> current ID (SPELL_NONE when that spell's mod is no longer installed).
	std::vector<std::pair<std::string, std::string>> savedSpellTable();
	std::map<int, int> savedSpellRemap(const std::vector<std::pair<std::string, std::string>>& additionalData);

	// Shared values (barony.setShared/getShared) belong to the current run: cleared when a run
	// starts or ends, written into the save's additional_data and restored from it on load.
	// Every change, including these, runs the mods' onSharedChange callbacks for that value.
	void resetSharedValues();
	std::vector<std::pair<std::string, std::string>> savedSharedTable();
	void restoreSharedValues(const std::vector<std::pair<std::string, std::string>>& additionalData);

	// Only the host changes shared values; clients keep a copy for reading. The host sends each
	// change as it happens. sendSharedValues() sends all of them to one remote client (host), in
	// reply to requestSharedValues() (client, after starting a run or loading a level).
	// receiveSharedValue() handles one "SHRD" packet in net_packet (client).
	void sendSharedValues(int player);
	void requestSharedValues();
	void receiveSharedValue();

	// Lines of text script mods show on the HUD (barony.setHudText), drawn for every local
	// player. clearHudText() removes them all when a run ends.
	static const int kMaxHudTextLines = 8;    // across all mods
	static const int kMaxHudTextLength = 128; // characters per line
	void clearHudText();
	size_t hudTextLineCount();
	const char* hudTextLine(size_t index);

	// Re-adds script-defined spells' tooltip text after readTooltipsFromFile() reloads templates.
	void reapplyCustomSpellTooltips();

	// Fires onBeforeSpellCast before a cast's mana is paid (or fizzle is rolled). Returns false
	// if a callback rejected the cast; its message has then been shown to the caster. It runs
	// twice per cast: in castSpellInit on the caster's machine, before the cast animation, and
	// again in castSpell on the host once the animation is done, since things may have changed
	// meanwhile (and a client's copy of shared values may be out of date). The host's check
	// decides.
	bool onBeforeSpellCast(spell_t* spell, int player);

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

	// Fires onFloorChange on the host once the party has arrived on a floor (currentlevel),
	// before the level-change save, so changes the callbacks make are saved. Host only.
	void onFloorChange(int floor, int previousFloor);
}
