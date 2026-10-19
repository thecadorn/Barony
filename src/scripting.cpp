/*-------------------------------------------------------------------------------

	BARONY
	File: scripting.cpp
	Desc: Lua scripting engine for mods

-------------------------------------------------------------------------------*/

#include "main.hpp"
#include "game.hpp"
#include "files.hpp"
#include "player.hpp"
#include "stat.hpp"
#include "items.hpp"
#include "entity.hpp"
#include "net.hpp"
#include "mod_tools.hpp"
#include "magic/magic.hpp"
#include "Directory.hpp"
#include "scripting.hpp"
#include "lua/lua.hpp"

#include <fstream>
#include <optional>
#include <sstream>

// NOTE: Lua is compiled as C and reports errors with longjmp. Inside any lua_CFunction
// below, never hold a C++ object with a destructor across a call that can raise a Lua
// error (luaL_check*, luaL_error, lua_call, ...), or that destructor will be skipped.

namespace Scripting
{
	namespace
	{
		// Sandbox limits.
		constexpr size_t kMemoryLimit = 64 * 1024 * 1024; // bytes, shared by all script mods
		constexpr int kHookInterval = 10000;               // instructions between budget checks
		constexpr long kInstructionBudget = 100000000;     // per call from the engine into Lua

		struct ScriptMod
		{
			std::string name;
			int env = LUA_NOREF; // registry ref to this mod's sandboxed _ENV table
		};

		struct Callback
		{
			size_t mod; // index into mods
			int fn;     // registry ref to the Lua function
		};

		lua_State* L = nullptr;
		size_t memoryUsed = 0;
		long instructionsUsed = 0;
		int callDepth = 0; // > 1 while Lua calls into the engine, which calls back into Lua
		std::vector<ScriptMod> mods;
		std::vector<Callback> onInitCallbacks;
		std::vector<Callback> onSpellCastCallbacks;
		std::vector<Callback> onBeforeSpellCastCallbacks;
		std::vector<Callback> onGameStartCallbacks;
		std::vector<Callback> onBeforeGameplayStartCallbacks;
		std::vector<Callback> onFloorChangeCallbacks;
		struct SharedChangeCallback
		{
			std::string name; // the shared value it watches
			Callback cb;
		};
		std::vector<SharedChangeCallback> onSharedChangeCallbacks;

		// A spell defined by a script: a clone of an existing spell with a new ID and tweaks.
		struct CustomSpell
		{
			int id = SPELL_NONE;
			int baseID = SPELL_NONE;
			size_t mod = 0;
			std::string internalName;
			std::string name;
			std::optional<int> mana;
			std::optional<int> duration;
			std::optional<int> difficulty;
			int iconSpellID = SPELL_NONE;              // whose picture the spell shows
			std::vector<std::string> effectLines;      // summary next to the effect icon
			std::vector<std::string> descriptionLines; // usage text below it
			std::optional<int> tooltipWidth;           // minimum tooltip width, in pixels
		};
		std::vector<CustomSpell> customSpells; // customSpells[i] has ID kCustomSpellIDStart + i
		bool registrationOpen = false;         // spells can only be defined while scripts initialize

		// Shared values: named numbers for game-wide state such as a party's resurrection
		// charges. They belong to the current run: cleared when a new run starts, stored in
		// the save game and restored on load.
		//
		// Only the host changes them. It sends each change to the clients ("SHRD": name + value),
		// and sends them all to a client that asks ("SHRQ") after starting a run or loading a
		// level. Clients keep the received values in this same map, so on a client getShared()
		// returns the host's value as of the last update, which can briefly be out of date.
		std::map<std::string, lua_Number> sharedValues;
		constexpr size_t kMaxSharedNameLength = 64;

		// Lines of text script mods show on every local player's HUD (barony.setHudText), in the
		// order they were added. Local to this machine and never saved.
		struct HudLine
		{
			size_t mod;
			std::string id;
			std::string text;
		};
		std::vector<HudLine> hudLines;
		constexpr size_t kMaxHudIdLength = 64;

		// host: sends one shared value to a remote client; a null value means it was removed
		void sendSharedValue(int player, const char* name, const lua_Number* value)
		{
			if ( multiplayer != SERVER || player <= 0 || player >= MAXPLAYERS
				|| client_disconnected[player] || players[player]->isLocalPlayer() )
			{
				return;
			}
			char text[32] = "";
			if ( value )
			{
				snprintf(text, sizeof(text), "%.17g", (double)*value);
			}
			const size_t textLength = strlen(text) + 1;
			const size_t nameLength = strlen(name) + 1; // names are at most 64 characters
			memcpy(net_packet->data, "SHRD", 4);
			net_packet->data[4] = value ? 0 : 1; // 1: the value was removed
			memcpy(&net_packet->data[5], text, textLength);
			memcpy(&net_packet->data[5 + textLength], name, nameLength);
			net_packet->len = 5 + textLength + nameLength;
			net_packet->address.host = net_clients[player - 1].host;
			net_packet->address.port = net_clients[player - 1].port;
			sendPacketSafe(net_sock, -1, net_packet, player - 1);
		}

		void* sandboxAlloc(void*, void* ptr, size_t osize, size_t nsize)
		{
			const size_t oldSize = ptr ? osize : 0; // when ptr is null, osize is a type tag
			if ( nsize == 0 )
			{
				memoryUsed -= oldSize;
				free(ptr);
				return nullptr;
			}
			if ( nsize > oldSize && memoryUsed - oldSize + nsize > kMemoryLimit )
			{
				return nullptr; // Lua turns this into a "not enough memory" error
			}
			void* result = realloc(ptr, nsize);
			if ( result )
			{
				memoryUsed = memoryUsed - oldSize + nsize;
			}
			return result;
		}

		void instructionHook(lua_State* L, lua_Debug*)
		{
			instructionsUsed += kHookInterval;
			if ( instructionsUsed > kInstructionBudget )
			{
				luaL_error(L, "script exceeded its instruction budget (infinite loop?)");
			}
		}

		int panicHandler(lua_State* L)
		{
			printlog("[Lua] unprotected error: %s", lua_tostring(L, -1));
			return 0; // Lua aborts after this returns
		}

		int messageHandler(lua_State* L)
		{
			const char* msg = lua_tostring(L, 1);
			if ( !msg )
			{
				msg = luaL_tolstring(L, 1, nullptr);
			}
			luaL_traceback(L, L, msg, 1);
			return 1;
		}

		// Calls the function below `nargs` arguments on the stack with error handling and
		// the instruction budget. Errors are logged against `mod`; returns true on success.
		bool protectedCall(size_t mod, int nargs, int nresults, const char* what)
		{
			const int handler = lua_gettop(L) - nargs;
			lua_pushcfunction(L, messageHandler);
			lua_insert(L, handler);
			if ( callDepth++ == 0 ) // nested calls share the outermost call's budget
			{
				instructionsUsed = 0;
				lua_sethook(L, instructionHook, LUA_MASKCOUNT, kHookInterval);
			}
			const int status = lua_pcall(L, nargs, nresults, handler);
			if ( --callDepth == 0 )
			{
				lua_sethook(L, nullptr, 0, 0);
			}
			lua_remove(L, handler);
			if ( status != LUA_OK )
			{
				printlog("[Lua:%s] error in %s: %s", mods[mod].name.c_str(), what, lua_tostring(L, -1));
				lua_pop(L, 1);
				return false;
			}
			return true;
		}

		size_t modIndex(lua_State* L)
		{
			return (size_t)lua_tointeger(L, lua_upvalueindex(1));
		}

		void addCallback(std::vector<Callback>& list, lua_State* L)
		{
			luaL_checktype(L, 1, LUA_TFUNCTION);
			lua_settop(L, 1);
			list.push_back(Callback{ modIndex(L), luaL_ref(L, LUA_REGISTRYINDEX) });
		}

		// barony.log(...): writes all arguments, tostring'd and space separated, to log.txt.
		int api_log(lua_State* L)
		{
			const int n = lua_gettop(L);
			luaL_Buffer buf;
			luaL_buffinit(L, &buf);
			for ( int i = 1; i <= n; ++i )
			{
				if ( i > 1 )
				{
					luaL_addchar(&buf, ' ');
				}
				luaL_tolstring(L, i, nullptr);
				luaL_addvalue(&buf);
			}
			luaL_pushresult(&buf);
			printlog("[Lua:%s] %s", mods[modIndex(L)].name.c_str(), lua_tostring(L, -1));
			return 0;
		}

		// barony.onInit(fn): fn() runs once, after every script mod has loaded.
		int api_onInit(lua_State* L)
		{
			addCallback(onInitCallbacks, L);
			return 0;
		}

		// barony.onSpellCast(fn): fn(spellName, player) runs whenever a spell is cast.
		// Returning false skips the spell's default effect.
		int api_onSpellCast(lua_State* L)
		{
			addCallback(onSpellCastCallbacks, L);
			return 0;
		}

		// barony.onBeforeSpellCast(fn): fn(spellName, player) runs before a spell's cost is paid.
		// Returning false (optionally followed by a message for the caster) rejects the cast.
		// It runs on the caster's machine before the cast animation, then on the host once the
		// animation is done, so fn should only check things. On a client, shared values can be
		// slightly out of date and only the host can change them; the host's check decides.
		int api_onBeforeSpellCast(lua_State* L)
		{
			addCallback(onBeforeSpellCastCallbacks, L);
			return 0;
		}

		// barony.onGameStart(fn): fn(player) runs for each local player when a new run starts.
		int api_onGameStart(lua_State* L)
		{
			addCallback(onGameStartCallbacks, L);
			return 0;
		}

		int findSpellID(const char* name)
		{
			auto& ids = ItemTooltips.spellNameStringToSpellID;
			auto find = ids.find(name);
			if ( find == ids.end() || allGameSpells.find(find->second) == allGameSpells.end() )
			{
				return SPELL_NONE;
			}
			return find->second;
		}

		// barony.teachSpell(player, spellName): gives a local player a spell. Returns true if learned.
		int api_teachSpell(lua_State* L)
		{
			const lua_Integer player = luaL_checkinteger(L, 1);
			luaL_argcheck(L, player >= 0 && player < MAXPLAYERS, 1, "player index out of range");
			const char* name = luaL_checkstring(L, 2);
			const int spellID = findSpellID(name);
			if ( spellID == SPELL_NONE )
			{
				return luaL_error(L, "unknown spell '%s'", name);
			}
			const bool learned = players[player] && players[player]->isLocalPlayer()
				&& addSpell(spellID, (int)player, true);
			lua_pushboolean(L, learned);
			return 1;
		}

		// The element a spell's damage/duration live on, matching setupSpells(): the first
		// child of the root element, or the root itself for spells without children (e.g. Light).
		spellElement_t* primaryElement(spell_t* spell)
		{
			if ( !spell->elements.first || !spell->elements.first->element )
			{
				return nullptr;
			}
			auto element = (spellElement_t*)spell->elements.first->element;
			if ( element->elements.first && element->elements.first->element )
			{
				element = (spellElement_t*)element->elements.first->element;
			}
			return element;
		}

		// Writes the mod's tooltip text into the same templates item_tooltips.json fills in.
		// readTooltipsFromFile() clears them, so this is re-run after every tooltip reload.
		void applyCustomSpellTooltips(const CustomSpell& cs)
		{
			auto info = ItemTooltips.spellItems.find(cs.id);
			if ( info == ItemTooltips.spellItems.end() )
			{
				return;
			}
			// Effect text is used as a printf format for spells tagged as dealing damage or
			// healing, so a literal % in mod text must be escaped there.
			auto& tags = info->second.spellTags;
			const bool effectIsFormatted = tags.count(ItemTooltips_t::SpellTagTypes::SPELL_TAG_DAMAGE)
				|| tags.count(ItemTooltips_t::SpellTagTypes::SPELL_TAG_HEALING)
				|| !info->second.spellFormatTags.empty();

			auto& effect = ItemTooltips.templates["template_icon_" + cs.internalName];
			effect.clear();
			for ( const std::string& line : cs.effectLines )
			{
				std::string escaped;
				for ( char c : line )
				{
					escaped += c;
					if ( c == '%' && effectIsFormatted )
					{
						escaped += '%';
					}
				}
				effect.push_back(escaped);
			}
			ItemTooltips.templates["template_desc_" + cs.internalName] = cs.descriptionLines;

			// per-spell width override, like the "item_overrides" entries in item_tooltips.json
			auto& minWidths = ItemTooltips.tooltips["tooltip_spell_item"].minWidths;
			if ( cs.tooltipWidth )
			{
				minWidths[cs.internalName] = *cs.tooltipWidth;
			}
			else
			{
				minWidths.erase(cs.internalName);
			}
		}

		// Clones the base spell into allGameSpells and its UI entry into ItemTooltips.
		bool applyCustomSpell(const CustomSpell& cs)
		{
			auto baseSpell = allGameSpells.find(cs.baseID);
			auto baseInfo = ItemTooltips.spellItems.find(cs.baseID);
			if ( baseSpell == allGameSpells.end() || baseInfo == ItemTooltips.spellItems.end()
				|| allGameSpells.count(cs.id) )
			{
				return false;
			}

			spell_t* spell = copySpell(baseSpell->second);
			spell->ID = cs.id;
			snprintf(spell->spell_internal_name, sizeof(spell->spell_internal_name), "%s", cs.internalName.c_str());
			spell->needsDataFreed = 1; // setupSpells() frees it when it rebuilds the tables
			if ( cs.mana ) { spell->mana = *cs.mana; }
			if ( cs.difficulty ) { spell->difficulty = *cs.difficulty; }
			if ( cs.duration )
			{
				if ( spellElement_t* element = primaryElement(spell) )
				{
					element->duration = *cs.duration;
				}
			}
			allGameSpells[cs.id] = spell;

			auto info = baseInfo->second;
			info.id = cs.id;
			info.internalName = cs.internalName;
			info.name = cs.name;
			info.name_lowercase = cs.name;
			for ( char& c : info.name_lowercase )
			{
				c = (char)tolower((unsigned char)c);
			}
			info.iconSpellID = cs.iconSpellID;
			info.spellbookId = info.magicstaffId = info.fociId = -1; // no item teaches or casts it
			info.spellbookInternalName.clear();
			info.magicstaffInternalName.clear();
			info.fociInternalName.clear();
			if ( cs.mana ) { info.mana = *cs.mana; }
			if ( cs.difficulty ) { info.difficulty = *cs.difficulty; }
			if ( cs.duration ) { info.duration = *cs.duration; }
			ItemTooltips.spellItems[cs.id] = info;
			ItemTooltips.spellNameStringToSpellID[cs.internalName] = cs.id;
			applyCustomSpellTooltips(cs);
			return true;
		}

		// Validates and creates a custom spell. Returns its ID, or SPELL_NONE with `error` set.
		// `icon` is the spell whose picture to show, or null to use the base spell's.
		int registerSpell(CustomSpell cs, const char* base, const char* icon, char* error, size_t errorSize)
		{
			const size_t length = cs.internalName.size();
			if ( length == 0 || length >= sizeof(spell_t::spell_internal_name) )
			{
				snprintf(error, errorSize, "spell id must be 1-%d characters", (int)sizeof(spell_t::spell_internal_name) - 1);
				return SPELL_NONE;
			}
			for ( char c : cs.internalName )
			{
				if ( !isalnum((unsigned char)c) && c != '_' )
				{
					snprintf(error, errorSize, "spell id '%s' may only contain letters, digits and _", cs.internalName.c_str());
					return SPELL_NONE;
				}
			}
			if ( ItemTooltips.spellNameStringToSpellID.count(cs.internalName) )
			{
				snprintf(error, errorSize, "a spell named '%s' already exists", cs.internalName.c_str());
				return SPELL_NONE;
			}
			cs.baseID = findSpellID(base);
			if ( cs.baseID == SPELL_NONE )
			{
				snprintf(error, errorSize, "unknown base spell '%s'", base);
				return SPELL_NONE;
			}
			const int iconSource = icon ? findSpellID(icon) : cs.baseID;
			if ( iconSource == SPELL_NONE )
			{
				snprintf(error, errorSize, "unknown icon spell '%s'", icon);
				return SPELL_NONE;
			}
			// a custom spell used as the icon source passes its own picture on
			auto iconInfo = ItemTooltips.spellItems.find(iconSource);
			cs.iconSpellID = (iconInfo != ItemTooltips.spellItems.end() && iconInfo->second.iconSpellID >= 0)
				? iconInfo->second.iconSpellID : iconSource;
			if ( kCustomSpellIDStart + (int)customSpells.size() >= kCustomSpellIDEnd )
			{
				snprintf(error, errorSize, "too many custom spells (max %d)", kCustomSpellIDEnd - kCustomSpellIDStart);
				return SPELL_NONE;
			}
			cs.id = kCustomSpellIDStart + (int)customSpells.size();
			if ( !applyCustomSpell(cs) )
			{
				snprintf(error, errorSize, "could not clone base spell '%s'", base);
				return SPELL_NONE;
			}
			customSpells.push_back(cs);
			printlog("[Lua:%s] defined spell '%s' (id %d, based on %s)",
				mods[cs.mod].name.c_str(), cs.internalName.c_str(), cs.id, base);
			return cs.id;
		}

		const char* requiredStringField(lua_State* L, const char* field)
		{
			// left on the stack so the string stays alive for the rest of the call
			if ( lua_getfield(L, 1, field) != LUA_TSTRING )
			{
				luaL_error(L, "spell definition needs a string field '%s'", field);
			}
			return lua_tostring(L, -1);
		}

		bool optionalIntField(lua_State* L, const char* field, int& out)
		{
			const int type = lua_getfield(L, 1, field);
			if ( type != LUA_TNIL && type != LUA_TNUMBER )
			{
				luaL_error(L, "spell field '%s' must be a number", field);
			}
			const bool present = type == LUA_TNUMBER;
			if ( present )
			{
				out = (int)lua_tonumber(L, -1);
			}
			lua_pop(L, 1);
			return present;
		}

		const char* optionalStringField(lua_State* L, const char* field)
		{
			// left on the stack so the string stays alive for the rest of the call
			const int type = lua_getfield(L, 1, field);
			if ( type != LUA_TNIL && type != LUA_TSTRING )
			{
				luaL_error(L, "spell field '%s' must be a string", field);
			}
			return lua_tostring(L, -1);
		}

		// Checks that a field is nil, a string, or a list of strings. Leaves it on the stack
		// and returns its index, so readLines() can read it later without raising errors.
		int checkLinesField(lua_State* L, const char* field)
		{
			const int type = lua_getfield(L, 1, field);
			if ( type == LUA_TTABLE )
			{
				const lua_Integer n = (lua_Integer)lua_rawlen(L, -1);
				for ( lua_Integer i = 1; i <= n; ++i )
				{
					if ( lua_rawgeti(L, -1, i) != LUA_TSTRING )
					{
						luaL_error(L, "spell field '%s' must be a string or a list of strings", field);
					}
					lua_pop(L, 1);
				}
			}
			else if ( type != LUA_TNIL && type != LUA_TSTRING )
			{
				luaL_error(L, "spell field '%s' must be a string or a list of strings", field);
			}
			return lua_gettop(L);
		}

		std::vector<std::string> readLines(lua_State* L, int index)
		{
			std::vector<std::string> lines;
			if ( lua_type(L, index) == LUA_TSTRING )
			{
				lines.push_back(lua_tostring(L, index));
			}
			else if ( lua_type(L, index) == LUA_TTABLE )
			{
				const lua_Integer n = (lua_Integer)lua_rawlen(L, index);
				for ( lua_Integer i = 1; i <= n; ++i )
				{
					lua_rawgeti(L, index, i);
					lines.push_back(lua_tostring(L, -1));
					lua_pop(L, 1);
				}
			}
			return lines;
		}

		// barony.registerSpell(def): defines a new spell cloned from def.base. Returns its ID.
		int api_registerSpell(lua_State* L)
		{
			luaL_checktype(L, 1, LUA_TTABLE);
			if ( !registrationOpen )
			{
				return luaL_error(L, "spells can only be registered while scripts load or in onInit");
			}
			const char* id = requiredStringField(L, "id");
			const char* base = requiredStringField(L, "base");
			const char* name = requiredStringField(L, "name");
			const char* icon = optionalStringField(L, "icon");
			const int effectIndex = checkLinesField(L, "effect");
			const int descriptionIndex = checkLinesField(L, "description");
			int mana = 0, duration = 0, difficulty = 0, tooltipWidth = 0;
			const bool hasMana = optionalIntField(L, "mana", mana);
			const bool hasDuration = optionalIntField(L, "duration", duration);
			const bool hasDifficulty = optionalIntField(L, "difficulty", difficulty);
			const bool hasTooltipWidth = optionalIntField(L, "tooltipWidth", tooltipWidth);

			char error[256];
			int spellID = SPELL_NONE;
			{
				// C++ objects are confined to this block so luaL_error below can't skip their destructors
				CustomSpell cs;
				cs.mod = modIndex(L);
				cs.internalName = id;
				cs.name = name;
				if ( hasMana ) { cs.mana = mana; }
				if ( hasDuration ) { cs.duration = duration; }
				if ( hasDifficulty ) { cs.difficulty = difficulty; }
				if ( hasTooltipWidth ) { cs.tooltipWidth = tooltipWidth; }
				cs.effectLines = readLines(L, effectIndex);
				cs.descriptionLines = readLines(L, descriptionIndex);
				spellID = registerSpell(cs, base, icon, error, sizeof(error));
			}
			if ( spellID == SPELL_NONE )
			{
				return luaL_error(L, "%s", error);
			}
			lua_pushinteger(L, spellID);
			return 1;
		}

		/*-------------------------------------------------------------------------------
			Hotbar, Inventory and Item handles

			Handles store a player index (and an item UID), never pointers. Every access
			looks the data up again, so a handle kept after its item is gone reads as
			invalid instead of touching freed memory. Only local players have a hotbar
			and inventory on this machine.
		-------------------------------------------------------------------------------*/

		constexpr const char* kHotbarMetatable = "Barony.Hotbar";
		constexpr const char* kInventoryMetatable = "Barony.Inventory";
		constexpr const char* kItemMetatable = "Barony.Item";
		constexpr const char* kPlayerMetatable = "Barony.Player";

		struct PlayerHandle { int player; };
		struct ItemHandle { int player; Uint32 uid; };

		bool isLocalPlayer(int player)
		{
			return player >= 0 && player < MAXPLAYERS && players[player] && players[player]->isLocalPlayer() && stats[player];
		}

		int checkPlayerArg(lua_State* L, int arg)
		{
			const lua_Integer player = luaL_checkinteger(L, arg);
			luaL_argcheck(L, player >= 0 && player < MAXPLAYERS, arg, "player index out of range");
			return (int)player;
		}

		// Finds an item by UID in one player's inventory.
		Item* findItem(int player, Uint32 uid)
		{
			if ( !isLocalPlayer(player) || uid == 0 )
			{
				return nullptr;
			}
			for ( node_t* node = stats[player]->inventory.first; node; node = node->next )
			{
				Item* item = (Item*)node->element;
				if ( item && item->uid == uid )
				{
					return item;
				}
			}
			return nullptr;
		}

		// The spell a SPELL_ITEM stands for (appearances >= 1000 are shapeshift variants).
		int spellIDOfItem(const Item* item)
		{
			if ( !item || item->type != SPELL_ITEM )
			{
				return SPELL_NONE;
			}
			return item->appearance >= 1000 ? (int)item->appearance - 1000 : (int)item->appearance;
		}

		void pushItem(lua_State* L, int player, Item* item)
		{
			if ( !item )
			{
				lua_pushnil(L);
				return;
			}
			auto handle = (ItemHandle*)lua_newuserdatauv(L, sizeof(ItemHandle), 0);
			handle->player = player;
			handle->uid = item->uid;
			luaL_setmetatable(L, kItemMetatable);
		}

		int checkSlotArg(lua_State* L, int arg)
		{
			const lua_Integer slot = luaL_checkinteger(L, arg);
			luaL_argcheck(L, slot >= 0 && slot < (lua_Integer)NUM_HOTBAR_SLOTS, arg, "hotbar slot must be 0-9");
			return (int)slot;
		}

		// Item: read-only fields describing an inventory item.
		int item_index(lua_State* L)
		{
			auto handle = (ItemHandle*)luaL_checkudata(L, 1, kItemMetatable);
			const char* key = luaL_checkstring(L, 2);
			Item* item = findItem(handle->player, handle->uid);
			if ( !strcmp(key, "valid") ) { lua_pushboolean(L, item != nullptr); return 1; }
			if ( !strcmp(key, "uid") ) { lua_pushinteger(L, handle->uid); return 1; }
			if ( !item )
			{
				return luaL_error(L, "item %d is no longer in the inventory (check item.valid first)", (int)handle->uid);
			}
			if ( !strcmp(key, "name") )
			{
				const bool known = item->type >= 0 && item->type < (int)ItemTooltips.tmpItems.size();
				lua_pushstring(L, known ? ItemTooltips.tmpItems[item->type].internalName.c_str() : "unknown");
				return 1;
			}
			if ( !strcmp(key, "isSpell") ) { lua_pushboolean(L, item->type == SPELL_ITEM); return 1; }
			if ( !strcmp(key, "spellName") )
			{
				auto spell = allGameSpells.find(spellIDOfItem(item));
				if ( spell != allGameSpells.end() && spell->second )
				{
					lua_pushstring(L, spell->second->spell_internal_name);
				}
				else
				{
					lua_pushnil(L);
				}
				return 1;
			}
			return luaL_error(L, "Item has no field '%s'", key);
		}

		int item_eq(lua_State* L)
		{
			auto a = (ItemHandle*)luaL_checkudata(L, 1, kItemMetatable);
			auto b = (ItemHandle*)luaL_checkudata(L, 2, kItemMetatable);
			lua_pushboolean(L, a->player == b->player && a->uid == b->uid);
			return 1;
		}

		int item_tostring(lua_State* L)
		{
			auto handle = (ItemHandle*)luaL_checkudata(L, 1, kItemMetatable);
			lua_pushfstring(L, "Item(%d)", (int)handle->uid);
			return 1;
		}

		// inventory:findSpell(spellName): the player's item for a spell they know, or nil.
		int inventory_findSpell(lua_State* L)
		{
			auto handle = (PlayerHandle*)luaL_checkudata(L, 1, kInventoryMetatable);
			const char* name = luaL_checkstring(L, 2);
			const int spellID = findSpellID(name);
			if ( spellID == SPELL_NONE )
			{
				return luaL_error(L, "unknown spell '%s'", name);
			}
			Item* found = nullptr;
			if ( isLocalPlayer(handle->player) )
			{
				for ( node_t* node = stats[handle->player]->inventory.first; node && !found; node = node->next )
				{
					Item* item = (Item*)node->element;
					if ( item && item->type == SPELL_ITEM && (int)item->appearance == spellID )
					{
						found = item;
					}
				}
			}
			pushItem(L, handle->player, found);
			return 1;
		}

		Player::Hotbar_t* checkHotbar(lua_State* L, int& player)
		{
			auto handle = (PlayerHandle*)luaL_checkudata(L, 1, kHotbarMetatable);
			player = handle->player;
			if ( !isLocalPlayer(player) )
			{
				luaL_error(L, "player %d is not a local player", player);
			}
			return &players[player]->hotbar;
		}

		// hotbar:getSlot(slot): the Item in a slot, or nil if it's empty.
		int hotbar_getSlot(lua_State* L)
		{
			int player;
			Player::Hotbar_t* hotbar = checkHotbar(L, player);
			const int slot = checkSlotArg(L, 2);
			pushItem(L, player, findItem(player, hotbar->slots()[slot].item));
			return 1;
		}

		// hotbar:setSlot(slot, item): puts one of the player's items in a slot.
		int hotbar_setSlot(lua_State* L)
		{
			int player;
			Player::Hotbar_t* hotbar = checkHotbar(L, player);
			const int slot = checkSlotArg(L, 2);
			auto handle = (ItemHandle*)luaL_checkudata(L, 3, kItemMetatable);
			if ( handle->player != player || !findItem(player, handle->uid) )
			{
				return luaL_error(L, "item %d is not in player %d's inventory", (int)handle->uid, player);
			}
			hotbar->slots()[slot].item = handle->uid;
			return 0;
		}

		// hotbar:clearSlot(slot): empties a slot.
		int hotbar_clearSlot(lua_State* L)
		{
			int player;
			Player::Hotbar_t* hotbar = checkHotbar(L, player);
			const int slot = checkSlotArg(L, 2);
			hotbar->slots()[slot].item = 0;
			hotbar->slots()[slot].resetLastItem();
			return 0;
		}

		// hotbar:firstEmptySlot(): the lowest empty slot, or nil if the bar is full.
		int hotbar_firstEmptySlot(lua_State* L)
		{
			int player;
			Player::Hotbar_t* hotbar = checkHotbar(L, player);
			for ( int slot = 0; slot < (int)NUM_HOTBAR_SLOTS; ++slot )
			{
				if ( !findItem(player, hotbar->slots()[slot].item) )
				{
					lua_pushinteger(L, slot);
					return 1;
				}
			}
			lua_pushnil(L);
			return 1;
		}

		// Player: a player's core stats. The host owns stats, so writes are host-only and
		// are sent to clients right away.
		Stat* checkPlayerStats(lua_State* L, int& player)
		{
			auto handle = (PlayerHandle*)luaL_checkudata(L, 1, kPlayerMetatable);
			player = handle->player;
			if ( client_disconnected[player] || !stats[player] )
			{
				luaL_error(L, "player %d is not in the game", player);
			}
			return stats[player];
		}

		// player:revive(x, y): brings a dead player back next to (x, y). Host only.
		int player_revive(lua_State* L)
		{
			int player;
			checkPlayerStats(L, player);
			const real_t x = luaL_checknumber(L, 2);
			const real_t y = luaL_checknumber(L, 3);
			if ( multiplayer == CLIENT )
			{
				return luaL_error(L, "players can only be revived on the host");
			}
			lua_pushboolean(L, revivePlayer(player, x, y));
			return 1;
		}

		int player_index(lua_State* L)
		{
			int player;
			Stat* stat = checkPlayerStats(L, player);
			const char* key = luaL_checkstring(L, 2);
			Entity* entity = players[player]->entity;
			if ( !strcmp(key, "index") ) { lua_pushinteger(L, player); return 1; }
			if ( !strcmp(key, "alive") ) { lua_pushboolean(L, entity && stat->HP > 0); return 1; }
			if ( !strcmp(key, "state") )
			{
				// "dying": the body is still there but HP ran out (death animation).
				// "dead": the body is gone. A player with no body and HP left (briefly, while a
				// level loads or a revive reaches a client) also reads as "dead", but can't be
				// revived; check canRevive for that.
				lua_pushstring(L, entity ? (stat->HP > 0 ? "alive" : "dying") : "dead");
				return 1;
			}
			if ( !strcmp(key, "canRevive") ) { lua_pushboolean(L, canRevivePlayer(player)); return 1; }
			if ( !strcmp(key, "x") ) { entity ? lua_pushnumber(L, entity->x) : lua_pushnil(L); return 1; }
			if ( !strcmp(key, "y") ) { entity ? lua_pushnumber(L, entity->y) : lua_pushnil(L); return 1; }
			if ( !strcmp(key, "revive") ) { lua_pushcfunction(L, player_revive); return 1; }
			if ( !strcmp(key, "hp") ) { lua_pushinteger(L, stat->HP); return 1; }
			if ( !strcmp(key, "maxhp") ) { lua_pushinteger(L, stat->MAXHP); return 1; }
			if ( !strcmp(key, "mp") ) { lua_pushinteger(L, stat->MP); return 1; }
			if ( !strcmp(key, "maxmp") ) { lua_pushinteger(L, stat->MAXMP); return 1; }
			return luaL_error(L, "Player has no field '%s'", key);
		}

		int player_newindex(lua_State* L)
		{
			int player;
			Stat* stat = checkPlayerStats(L, player);
			const char* key = luaL_checkstring(L, 2);
			const int value = (int)luaL_checknumber(L, 3);
			if ( multiplayer == CLIENT )
			{
				return luaL_error(L, "player.%s can only be changed on the host", key);
			}
			Entity* entity = players[player]->entity;
			if ( !strcmp(key, "maxhp") )
			{
				stat->MAXHP = std::max(1, value);
				if ( stat->HP > stat->MAXHP )
				{
					entity ? entity->setHP(stat->MAXHP) : (void)(stat->HP = stat->MAXHP);
				}
			}
			else if ( !strcmp(key, "hp") )
			{
				const int hp = std::clamp(value, 0, stat->MAXHP);
				entity ? entity->setHP(hp) : (void)(stat->HP = hp); // setHP handles death and sync
			}
			else if ( !strcmp(key, "maxmp") )
			{
				stat->MAXMP = std::max(0, value);
				if ( stat->MP > stat->MAXMP )
				{
					entity ? entity->setMP(stat->MAXMP) : (void)(stat->MP = stat->MAXMP);
				}
			}
			else if ( !strcmp(key, "mp") )
			{
				const int mp = std::clamp(value, 0, stat->MAXMP);
				entity ? entity->setMP(mp) : (void)(stat->MP = mp);
			}
			else
			{
				return luaL_error(L, "Player field '%s' is read-only or does not exist", key);
			}
			serverUpdatePlayerStats(); // sends HP/MAXHP/MP/MAXMP of every player to clients
			return 0;
		}

		int player_tostring(lua_State* L)
		{
			auto handle = (PlayerHandle*)luaL_checkudata(L, 1, kPlayerMetatable);
			lua_pushfstring(L, "Player(%d)", handle->player);
			return 1;
		}

		// barony.player(index): a player in the game, or nil if that slot is empty.
		int api_player(lua_State* L)
		{
			const int player = checkPlayerArg(L, 1);
			if ( client_disconnected[player] || !stats[player] )
			{
				lua_pushnil(L);
				return 1;
			}
			auto handle = (PlayerHandle*)lua_newuserdatauv(L, sizeof(PlayerHandle), 0);
			handle->player = player;
			luaL_setmetatable(L, kPlayerMetatable);
			return 1;
		}

		// Registers a handle type whose methods/constants live in its __index table.
		void createMethodMetatable(const char* name, const luaL_Reg* methods)
		{
			luaL_newmetatable(L, name);
			lua_newtable(L);
			luaL_setfuncs(L, methods, 0);
			lua_setfield(L, -2, "__index");
			lua_pushliteral(L, "locked");
			lua_setfield(L, -2, "__metatable");
		}

		void createHandleMetatables()
		{
			const luaL_Reg hotbarMethods[] = {
				{ "getSlot", hotbar_getSlot },
				{ "setSlot", hotbar_setSlot },
				{ "clearSlot", hotbar_clearSlot },
				{ "firstEmptySlot", hotbar_firstEmptySlot },
				{ nullptr, nullptr },
			};
			createMethodMetatable(kHotbarMetatable, hotbarMethods);
			lua_getfield(L, -1, "__index");
			lua_pushinteger(L, NUM_HOTBAR_SLOTS);
			lua_setfield(L, -2, "size");
			lua_pop(L, 2);

			const luaL_Reg inventoryMethods[] = {
				{ "findSpell", inventory_findSpell },
				{ nullptr, nullptr },
			};
			createMethodMetatable(kInventoryMetatable, inventoryMethods);
			lua_pop(L, 1);

			const luaL_Reg itemMeta[] = {
				{ "__index", item_index },
				{ "__eq", item_eq },
				{ "__tostring", item_tostring },
				{ nullptr, nullptr },
			};
			luaL_newmetatable(L, kItemMetatable);
			luaL_setfuncs(L, itemMeta, 0);
			lua_pushliteral(L, "locked");
			lua_setfield(L, -2, "__metatable");
			lua_pop(L, 1);

			const luaL_Reg playerMeta[] = {
				{ "__index", player_index },
				{ "__newindex", player_newindex },
				{ "__tostring", player_tostring },
				{ nullptr, nullptr },
			};
			luaL_newmetatable(L, kPlayerMetatable);
			luaL_setfuncs(L, playerMeta, 0);
			lua_pushliteral(L, "locked");
			lua_setfield(L, -2, "__metatable");
			lua_pop(L, 1);
		}

		// Pushes a player-scoped handle, or nil for players who aren't local.
		int pushPlayerHandle(lua_State* L, const char* metatable)
		{
			const int player = checkPlayerArg(L, 1);
			if ( !isLocalPlayer(player) )
			{
				lua_pushnil(L);
				return 1;
			}
			auto handle = (PlayerHandle*)lua_newuserdatauv(L, sizeof(PlayerHandle), 0);
			handle->player = player;
			luaL_setmetatable(L, metatable);
			return 1;
		}

		// barony.hotbar(player): the player's Hotbar, or nil if they aren't local.
		int api_hotbar(lua_State* L)
		{
			return pushPlayerHandle(L, kHotbarMetatable);
		}

		// barony.inventory(player): the player's Inventory, or nil if they aren't local.
		int api_inventory(lua_State* L)
		{
			return pushPlayerHandle(L, kInventoryMetatable);
		}

		// barony.onBeforeGameplayStart(fn): fn(player) runs for each local player once a new
		// run is fully set up (including the hotbar), right before gameplay begins.
		int api_onBeforeGameplayStart(lua_State* L)
		{
			addCallback(onBeforeGameplayStartCallbacks, L);
			return 0;
		}

		// barony.onFloorChange(fn): fn(floor, previousFloor) runs on the host when the party
		// arrives on a floor, before the game saves. Floors are numbered as the game shows them.
		int api_onFloorChange(lua_State* L)
		{
			addCallback(onFloorChangeCallbacks, L);
			return 0;
		}

		bool isValidSharedName(const char* name)
		{
			const size_t length = strlen(name);
			if ( length == 0 || length > kMaxSharedNameLength )
			{
				return false;
			}
			for ( size_t i = 0; i < length; ++i )
			{
				if ( !isalnum((unsigned char)name[i]) && name[i] != '_' )
				{
					return false;
				}
			}
			return true;
		}

		// Shared values are stored as floats; whole numbers go to Lua as integers so that they
		// print as "3", not "3.0".
		void pushSharedValue(lua_State* L, lua_Number value)
		{
			lua_Integer integer;
			if ( lua_numbertointeger(value, &integer) && (lua_Number)integer == value )
			{
				lua_pushinteger(L, integer);
			}
			else
			{
				lua_pushnumber(L, value);
			}
		}

		// Every change to this machine's shared values goes through here: stores the value (or
		// removes it, for a null value) and, if that changed anything, sends the change to the
		// clients (host only) and runs the onSharedChange callbacks for it.
		void changeSharedValue(const char* nameIn, const lua_Number* valueIn)
		{
			// copies, since the name can point into net_packet and callbacks can send packets
			char name[kMaxSharedNameLength + 1];
			snprintf(name, sizeof(name), "%s", nameIn);
			const bool removed = valueIn == nullptr;
			const lua_Number value = removed ? 0 : *valueIn;

			auto find = sharedValues.find(name);
			if ( removed )
			{
				if ( find == sharedValues.end() )
				{
					return;
				}
				sharedValues.erase(find);
			}
			else
			{
				if ( find != sharedValues.end() && find->second == value )
				{
					return;
				}
				sharedValues[name] = value;
			}

			for ( int c = 1; c < MAXPLAYERS; ++c )
			{
				sendSharedValue(c, name, removed ? nullptr : &value);
			}

			if ( !L )
			{
				return;
			}
			// by index, since a callback may register more callbacks
			for ( size_t i = 0; i < onSharedChangeCallbacks.size(); ++i )
			{
				if ( onSharedChangeCallbacks[i].name != name )
				{
					continue;
				}
				const Callback cb = onSharedChangeCallbacks[i].cb;
				lua_rawgeti(L, LUA_REGISTRYINDEX, cb.fn);
				if ( removed )
				{
					lua_pushnil(L);
				}
				else
				{
					pushSharedValue(L, value);
				}
				protectedCall(cb.mod, 1, 0, "onSharedChange");
			}
		}

		void checkSharedName(lua_State* L)
		{
			const char* name = luaL_checkstring(L, 1);
			luaL_argcheck(L, isValidSharedName(name), 1, "name must be 1-64 letters, digits or _");
		}

		// barony.setShared(name, value): stores a number (or nil to remove it) and sends it to
		// the clients. Host only.
		int api_setShared(lua_State* L)
		{
			if ( multiplayer == CLIENT )
			{
				return luaL_error(L, "barony.setShared is host-only");
			}
			checkSharedName(L);
			const char* name = lua_tostring(L, 1);
			if ( lua_isnoneornil(L, 2) )
			{
				changeSharedValue(name, nullptr);
			}
			else
			{
				const lua_Number value = luaL_checknumber(L, 2);
				changeSharedValue(name, &value);
			}
			return 0;
		}

		// barony.onSharedChange(name, fn): fn(value) runs on this machine whenever its copy of the
		// shared value changes: setShared on the host, an update reaching a client, a save loading,
		// or values being cleared when a run starts or ends (value is then nil).
		int api_onSharedChange(lua_State* L)
		{
			checkSharedName(L);
			luaL_checktype(L, 2, LUA_TFUNCTION);
			lua_settop(L, 2);
			const int fn = luaL_ref(L, LUA_REGISTRYINDEX); // pops fn
			onSharedChangeCallbacks.push_back(SharedChangeCallback{ lua_tostring(L, 1), Callback{ modIndex(L), fn } });
			return 0;
		}

		// barony.getShared(name): the stored number, or nil. On a client this is the host's
		// value as of the last update received, or nil if none has arrived yet.
		int api_getShared(lua_State* L)
		{
			checkSharedName(L);
			auto find = sharedValues.find(lua_tostring(L, 1));
			if ( find == sharedValues.end() )
			{
				lua_pushnil(L);
			}
			else
			{
				pushSharedValue(L, find->second);
			}
			return 1;
		}

		// barony.isHost(): true on the host and in singleplayer, where gameplay is decided.
		int api_isHost(lua_State* L)
		{
			lua_pushboolean(L, multiplayer != CLIENT);
			return 1;
		}

		// barony.message(player, text): shows text in a player's message log. On the host this
		// also reaches remote players.
		int api_message(lua_State* L)
		{
			const int player = checkPlayerArg(L, 1);
			const char* text = luaL_checkstring(L, 2);
			char line[256]; // messages are copied into fixed-size buffers and packets
			snprintf(line, sizeof(line), "%s", text);
			messagePlayer(player, MESSAGE_MISC, "%s", line);
			return 0;
		}

		// barony.setHudText(id, text): shows a line of text on every local player's HUD, or
		// removes it when text is nil. Each mod's ids are its own. Not sent to other machines.
		int api_setHudText(lua_State* L)
		{
			const char* id = luaL_checkstring(L, 1);
			const size_t idLength = strlen(id);
			luaL_argcheck(L, idLength >= 1 && idLength <= kMaxHudIdLength, 1, "id must be 1-64 characters");
			const char* text = luaL_optstring(L, 2, nullptr);
			luaL_argcheck(L, !text || strlen(text) <= (size_t)kMaxHudTextLength, 2, "text must be at most 128 characters");

			const size_t mod = modIndex(L);
			size_t i = 0;
			while ( i < hudLines.size() && !(hudLines[i].mod == mod && hudLines[i].id == id) )
			{
				++i;
			}
			if ( !text )
			{
				if ( i < hudLines.size() )
				{
					hudLines.erase(hudLines.begin() + i);
				}
			}
			else if ( i < hudLines.size() )
			{
				hudLines[i].text = text;
			}
			else if ( hudLines.size() >= (size_t)kMaxHudTextLines )
			{
				return luaL_error(L, "the HUD can show at most %d lines from script mods", kMaxHudTextLines);
			}
			else
			{
				hudLines.push_back(HudLine{ mod, id, text });
			}
			return 0;
		}

		// Builds the sandboxed _ENV for a mod and leaves it on the stack. Only safe
		// globals are exposed, and library tables are copied so mods can't tamper
		// with each other's.
		void pushModEnvironment(size_t mod)
		{
			static const char* safeGlobals[] = {
				"assert", "error", "getmetatable", "ipairs", "next", "pairs", "pcall",
				"rawequal", "rawget", "rawlen", "rawset", "select", "setmetatable",
				"tonumber", "tostring", "type", "xpcall", "_VERSION",
			};
			static const char* safeLibraries[] = { "coroutine", "math", "string", "table", "utf8" };

			lua_newtable(L);
			lua_pushglobaltable(L);
			for ( const char* name : safeGlobals )
			{
				lua_getfield(L, -1, name);
				lua_setfield(L, -3, name);
			}
			for ( const char* name : safeLibraries )
			{
				lua_getfield(L, -1, name);
				lua_newtable(L);
				lua_pushnil(L);
				while ( lua_next(L, -3) )
				{
					lua_pushvalue(L, -2);
					lua_insert(L, -2);
					lua_settable(L, -4);
				}
				lua_setfield(L, -4, name);
				lua_pop(L, 1);
			}
			lua_pop(L, 1); // global table

			lua_newtable(L); // barony
			const luaL_Reg api[] = {
				{ "log", api_log },
				{ "onInit", api_onInit },
				{ "onSpellCast", api_onSpellCast },
				{ "onBeforeSpellCast", api_onBeforeSpellCast },
				{ "onGameStart", api_onGameStart },
				{ "teachSpell", api_teachSpell },
				{ "registerSpell", api_registerSpell },
				{ "hotbar", api_hotbar },
				{ "player", api_player },
				{ "setShared", api_setShared },
				{ "getShared", api_getShared },
				{ "isHost", api_isHost },
				{ "message", api_message },
				{ "inventory", api_inventory },
				{ "onBeforeGameplayStart", api_onBeforeGameplayStart },
				{ "onFloorChange", api_onFloorChange },
				{ "onSharedChange", api_onSharedChange },
				{ "setHudText", api_setHudText },
				{ nullptr, nullptr },
			};
			lua_pushinteger(L, (lua_Integer)mod);
			luaL_setfuncs(L, api, 1);
			lua_pushstring(L, VERSION);
			lua_setfield(L, -2, "version");
			lua_pushinteger(L, MAXPLAYERS);
			lua_setfield(L, -2, "maxPlayers");
			lua_getfield(L, -1, "log");
			lua_setfield(L, -3, "print"); // print() is an alias for barony.log()
			lua_setfield(L, -2, "barony");

			lua_pushvalue(L, -1);
			lua_setfield(L, -2, "_G");
		}

		bool readFile(const std::string& path, std::string& out)
		{
			std::ifstream file(path, std::ios::binary);
			if ( !file )
			{
				return false;
			}
			std::stringstream ss;
			ss << file.rdbuf();
			out = ss.str();
			return true;
		}

		void loadMod(const std::string& name, const std::string& path)
		{
			std::string source;
			if ( !readFile(path, source) )
			{
				return; // not a script mod
			}

			const size_t mod = mods.size();
			mods.push_back(ScriptMod{ name });
			pushModEnvironment(mod);
			lua_pushvalue(L, -1);
			mods[mod].env = luaL_ref(L, LUA_REGISTRYINDEX);

			// Text mode only: precompiled bytecode can't be verified and is refused.
			const std::string chunkname = "@scripts/" + name + "/main.lua";
			if ( luaL_loadbufferx(L, source.data(), source.size(), chunkname.c_str(), "t") != LUA_OK )
			{
				printlog("[Lua:%s] failed to load: %s", name.c_str(), lua_tostring(L, -1));
				lua_pop(L, 2); // error, env
				return;
			}
			lua_insert(L, -2);
			lua_setupvalue(L, -2, 1); // the chunk's _ENV becomes the sandbox
			if ( protectedCall(mod, 0, 0, "main.lua") )
			{
				printlog("[Lua] loaded script mod '%s'", name.c_str());
			}
		}
	}

	void init()
	{
		if ( L )
		{
			return;
		}
		L = lua_newstate(sandboxAlloc, nullptr);
		if ( !L )
		{
			printlog("[Lua] failed to create Lua state, scripting disabled");
			return;
		}
		lua_atpanic(L, panicHandler);

		// io, os, package and debug are not compiled in at all (see lua/CMakeLists.txt).
		luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
		luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
		luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
		luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
		luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
		luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
		lua_settop(L, 0);

		// Lock the shared string metatable so mods can't modify the string library
		// for every other mod through ("").upper and friends.
		lua_pushliteral(L, "");
		lua_getmetatable(L, -1);
		lua_pushliteral(L, "locked");
		lua_setfield(L, -2, "__metatable");
		lua_settop(L, 0);

		createHandleMetatables();

		std::string scriptsDir = outputdir;
		scriptsDir += "/scripts";
		DIR* dir = opendir(scriptsDir.c_str());
		if ( !dir )
		{
			printlog("[Lua] no script mods found (looked in %s)", scriptsDir.c_str());
			return;
		}
		closedir(dir);

		// Scripts load in alphabetical order, so every machine with the same mods
		// gives custom spells the same IDs.
		registrationOpen = true;
		Directory directory(scriptsDir.c_str());
		for ( const std::string& name : directory.list )
		{
			loadMod(name, scriptsDir + "/" + name + "/main.lua");
		}
		printlog("[Lua] %d script mod(s) loaded", (int)mods.size());

		for ( size_t i = 0; i < onInitCallbacks.size(); ++i )
		{
			const Callback cb = onInitCallbacks[i];
			lua_rawgeti(L, LUA_REGISTRYINDEX, cb.fn);
			protectedCall(cb.mod, 0, 0, "onInit");
		}
		registrationOpen = false;
	}

	void deinit()
	{
		if ( !L )
		{
			return;
		}
		lua_close(L);
		L = nullptr;
		mods.clear();
		onInitCallbacks.clear();
		onSpellCastCallbacks.clear();
		onBeforeSpellCastCallbacks.clear();
		onGameStartCallbacks.clear();
		onBeforeGameplayStartCallbacks.clear();
		onFloorChangeCallbacks.clear();
		onSharedChangeCallbacks.clear();
		hudLines.clear();
		customSpells.clear();
	}

	bool onBeforeSpellCast(spell_t* spell, int player)
	{
		if ( !L || !spell )
		{
			return true;
		}
		for ( size_t i = 0; i < onBeforeSpellCastCallbacks.size(); ++i )
		{
			const Callback cb = onBeforeSpellCastCallbacks[i];
			lua_rawgeti(L, LUA_REGISTRYINDEX, cb.fn);
			lua_pushstring(L, spell->spell_internal_name);
			if ( player >= 0 )
			{
				lua_pushinteger(L, player);
			}
			else
			{
				lua_pushnil(L);
			}
			if ( !protectedCall(cb.mod, 2, 2, "onBeforeSpellCast") )
			{
				continue; // a broken callback doesn't block casting
			}
			// only an explicit `return false` rejects; returning nothing allows the cast
			const bool rejected = lua_isboolean(L, -2) && !lua_toboolean(L, -2);
			if ( rejected && player >= 0 && lua_type(L, -1) == LUA_TSTRING )
			{
				char reason[256]; // messages are copied into fixed-size buffers and packets
				snprintf(reason, sizeof(reason), "%s", lua_tostring(L, -1));
				messagePlayer(player, MESSAGE_MISC, "%s", reason);
			}
			lua_pop(L, 2);
			if ( rejected )
			{
				return false;
			}
		}
		return true;
	}

	bool onSpellCast(spell_t* spell, int player)
	{
		if ( !L || !spell || multiplayer == CLIENT )
		{
			return true;
		}
		bool runDefaultEffect = true;
		for ( size_t i = 0; i < onSpellCastCallbacks.size(); ++i )
		{
			const Callback cb = onSpellCastCallbacks[i];
			lua_rawgeti(L, LUA_REGISTRYINDEX, cb.fn);
			lua_pushstring(L, spell->spell_internal_name);
			if ( player >= 0 )
			{
				lua_pushinteger(L, player);
			}
			else
			{
				lua_pushnil(L);
			}
			if ( protectedCall(cb.mod, 2, 1, "onSpellCast") )
			{
				// only an explicit `return false` cancels; returning nothing keeps the effect
				if ( lua_isboolean(L, -1) && !lua_toboolean(L, -1) )
				{
					runDefaultEffect = false;
				}
				lua_pop(L, 1);
			}
		}
		return runDefaultEffect;
	}

	void reapplyCustomSpells()
	{
		for ( const CustomSpell& cs : customSpells )
		{
			if ( !applyCustomSpell(cs) )
			{
				printlog("[Lua:%s] could not re-create spell '%s'", mods[cs.mod].name.c_str(), cs.internalName.c_str());
			}
		}
	}

	namespace
	{
		constexpr const char* kSavedSpellKeyPrefix = "script_spell_";
	}

	std::vector<std::pair<std::string, std::string>> savedSpellTable()
	{
		std::vector<std::pair<std::string, std::string>> table;
		for ( const CustomSpell& cs : customSpells )
		{
			table.emplace_back(kSavedSpellKeyPrefix + std::to_string(cs.id), cs.internalName);
		}
		return table;
	}

	std::map<int, int> savedSpellRemap(const std::vector<std::pair<std::string, std::string>>& additionalData)
	{
		std::map<int, int> remap;
		const size_t prefixLength = strlen(kSavedSpellKeyPrefix);
		for ( auto& entry : additionalData )
		{
			if ( entry.first.compare(0, prefixLength, kSavedSpellKeyPrefix) != 0 )
			{
				continue;
			}
			const int savedID = atoi(entry.first.c_str() + prefixLength);
			const int currentID = findSpellID(entry.second.c_str());
			remap[savedID] = currentID;
			if ( currentID == SPELL_NONE )
			{
				printlog("[Lua] saved game uses spell '%s', but no installed script mod defines it", entry.second.c_str());
			}
		}
		return remap;
	}

	namespace
	{
		constexpr const char* kSavedSharedKeyPrefix = "script_shared_";
	}

	void resetSharedValues()
	{
		std::vector<std::string> names; // copied, since removing them changes the map
		for ( auto& value : sharedValues )
		{
			names.push_back(value.first);
		}
		for ( auto& name : names )
		{
			changeSharedValue(name.c_str(), nullptr);
		}
	}

	void clearHudText()
	{
		hudLines.clear();
	}

	size_t hudTextLineCount()
	{
		return hudLines.size();
	}

	const char* hudTextLine(size_t index)
	{
		return index < hudLines.size() ? hudLines[index].text.c_str() : "";
	}

	void sendSharedValues(int player)
	{
		for ( auto& value : sharedValues )
		{
			sendSharedValue(player, value.first.c_str(), &value.second);
		}
	}

	void receiveSharedValue()
	{
		if ( multiplayer != CLIENT || net_packet->len < 7 )
		{
			return;
		}
		net_packet->data[net_packet->len - 1] = 0; // the name ends the packet
		const char* text = (const char*)&net_packet->data[5];
		const size_t textLength = strnlen(text, net_packet->len - 5) + 1;
		if ( 5 + textLength >= (size_t)net_packet->len )
		{
			return;
		}
		const char* name = text + textLength;
		if ( !isValidSharedName(name) )
		{
			return;
		}
		if ( net_packet->data[4] == 1 )
		{
			changeSharedValue(name, nullptr);
		}
		else
		{
			const lua_Number value = (lua_Number)atof(text);
			changeSharedValue(name, &value);
		}
	}

	void requestSharedValues()
	{
		if ( multiplayer != CLIENT )
		{
			return;
		}
		memcpy(net_packet->data, "SHRQ", 4);
		net_packet->data[4] = clientnum;
		net_packet->len = 5;
		net_packet->address.host = net_server.host;
		net_packet->address.port = net_server.port;
		sendPacketSafe(net_sock, -1, net_packet, 0);
	}

	std::vector<std::pair<std::string, std::string>> savedSharedTable()
	{
		std::vector<std::pair<std::string, std::string>> table;
		for ( auto& value : sharedValues )
		{
			char text[64];
			snprintf(text, sizeof(text), "%.17g", (double)value.second);
			table.emplace_back(kSavedSharedKeyPrefix + value.first, text);
		}
		return table;
	}

	void restoreSharedValues(const std::vector<std::pair<std::string, std::string>>& additionalData)
	{
		std::map<std::string, lua_Number> saved;
		const size_t prefixLength = strlen(kSavedSharedKeyPrefix);
		for ( auto& entry : additionalData )
		{
			if ( entry.first.compare(0, prefixLength, kSavedSharedKeyPrefix) == 0
				&& isValidSharedName(entry.first.c_str() + prefixLength) )
			{
				saved[entry.first.substr(prefixLength)] = (lua_Number)atof(entry.second.c_str());
			}
		}

		// go through changeSharedValue so mods hear about the values the save changes
		std::vector<std::string> removed;
		for ( auto& value : sharedValues )
		{
			if ( saved.find(value.first) == saved.end() )
			{
				removed.push_back(value.first);
			}
		}
		for ( auto& name : removed )
		{
			changeSharedValue(name.c_str(), nullptr);
		}
		for ( auto& value : saved )
		{
			changeSharedValue(value.first.c_str(), &value.second);
		}
	}

	void reapplyCustomSpellTooltips()
	{
		for ( const CustomSpell& cs : customSpells )
		{
			applyCustomSpellTooltips(cs);
		}
	}

	void onGameStart(int player)
	{
		if ( !L || player < 0 || player >= MAXPLAYERS || !players[player] || !players[player]->isLocalPlayer() )
		{
			return;
		}
		for ( size_t i = 0; i < onGameStartCallbacks.size(); ++i )
		{
			const Callback cb = onGameStartCallbacks[i];
			lua_rawgeti(L, LUA_REGISTRYINDEX, cb.fn);
			lua_pushinteger(L, player);
			protectedCall(cb.mod, 1, 0, "onGameStart");
		}
	}

	void onBeforeGameplayStart(int player)
	{
		if ( !L || !isLocalPlayer(player) )
		{
			return;
		}
		for ( size_t i = 0; i < onBeforeGameplayStartCallbacks.size(); ++i )
		{
			const Callback cb = onBeforeGameplayStartCallbacks[i];
			lua_rawgeti(L, LUA_REGISTRYINDEX, cb.fn);
			lua_pushinteger(L, player);
			protectedCall(cb.mod, 1, 0, "onBeforeGameplayStart");
		}
	}

	void onFloorChange(int floor, int previousFloor)
	{
		if ( !L || multiplayer == CLIENT )
		{
			return;
		}
		for ( size_t i = 0; i < onFloorChangeCallbacks.size(); ++i )
		{
			const Callback cb = onFloorChangeCallbacks[i];
			lua_rawgeti(L, LUA_REGISTRYINDEX, cb.fn);
			lua_pushinteger(L, floor);
			lua_pushinteger(L, previousFloor);
			protectedCall(cb.mod, 2, 0, "onFloorChange");
		}
	}
}
