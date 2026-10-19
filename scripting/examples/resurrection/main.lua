-- Resurrection: a custom spell that brings one fallen ally back to life next to the caster,
-- with half their HP and mana. If nobody has opened the loot bag they dropped when they died,
-- they come back with all their items, still equipped.
--
-- Besides mana, each resurrection uses one of the party's shared resurrection charges.
-- The host keeps the count (barony.setShared), it's saved with the game, and every new run
-- starts with STARTING_CHARGES. With no charges left, or nobody to revive, the cast is
-- refused before any mana is spent and the caster is told why.
--
-- Every new character learns the spell with it on the hotbar.
-- Install by copying this folder to ~/.barony/scripts/resurrection/

local MANA_COST = 10
local STARTING_CHARGES = 3
local CHARGES = "resurrection_charges" -- name of the shared value

barony.onInit(function()
	barony.registerSpell{
		id = "spell_resurrection",
		base = "spell_magicmapping",   -- a simple self-cast spell; its effect is skipped below
		name = "Resurrection",
		mana = MANA_COST,
		icon = "spell_extrahealing",
		effect = { "Revives one fallen ally", "beside you with half HP" },
		description = { "Uses one of the party's", "resurrection charges." },
		tooltipWidth = 316,
	}
end)

barony.onGameStart(function(player)
	barony.teachSpell(player, "spell_resurrection")

	-- the host sets up the party's charges once per run
	if barony.isHost() and barony.getShared(CHARGES) == nil then
		barony.setShared(CHARGES, STARTING_CHARGES)
	end
end)

-- Once the default hotbar is laid out, put Resurrection in the first free slot.
barony.onBeforeGameplayStart(function(player)
	local hotbar = barony.hotbar(player)
	local spell = barony.inventory(player):findSpell("spell_resurrection")
	local slot = hotbar:firstEmptySlot()
	if spell and slot then
		hotbar:setSlot(slot, spell)
	end
end)

-- Finds the first fallen ally who can be revived right now (players are numbered
-- 0 to maxPlayers - 1). Allies still in their death animation don't count yet.
local function findFallenAlly()
	for i = 0, barony.maxPlayers - 1 do
		local ally = barony.player(i)
		if ally and ally.canRevive then
			return ally
		end
	end
	return nil
end

-- Before any mana is spent: refuse the cast if it couldn't do anything.
-- Returning false cancels it; the message is shown to the caster.
barony.onBeforeSpellCast(function(spellName, player)
	if spellName ~= "spell_resurrection" then
		return
	end
	if (barony.getShared(CHARGES) or 0) < 1 then
		return false, "The party has no resurrection charges left."
	end
	if findFallenAlly() == nil then
		return false, "There is no fallen ally to resurrect."
	end
end)

-- The cast went ahead: revive the ally and use up a charge.
barony.onSpellCast(function(spellName, player)
	if spellName ~= "spell_resurrection" then
		return
	end

	-- monsters and traps can cast spells too; for them `player` is nil
	if player == nil then
		return false
	end

	local caster = barony.player(player)
	local ally = findFallenAlly()
	if ally and ally:revive(caster.x, caster.y) then
		local charges = barony.getShared(CHARGES) - 1
		barony.setShared(CHARGES, charges)
		barony.message(player, "Resurrection charges left: " .. charges)
		barony.log("player " .. player .. " resurrected player " .. ally.index .. ", charges left: " .. charges)
	end

	return false -- skip the base spell's effect
end)
