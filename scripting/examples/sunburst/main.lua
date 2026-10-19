-- Defines a custom spell, Sunburst: built on Light, but instead of summoning an orb
-- it permanently raises the caster's max HP by 10.
-- Every new character learns it with it on the hotbar.
-- Install by copying this folder to ~/.barony/scripts/sunburst/

barony.onInit(function()
	barony.registerSpell{
		id = "spell_sunburst",
		base = "spell_light",   -- its default effect (Light's orb) is skipped in onSpellCast below
		name = "Sunburst",
		mana = 5,               -- Light costs 1
		icon = "spell_light",   -- show Light's picture

		-- tooltip text; [brackets] highlight keywords, like the base game's tooltips
		effect = { "Permanently increases", "maximum HP by 10" },
		description = { "Press [Cast] while equipped", "to cast." },
		tooltipWidth = 316,     -- fits the "Thaumaturgy I / Cost" row
	}
end)

barony.onGameStart(function(player)
	barony.teachSpell(player, "spell_sunburst")
end)

-- Once the default hotbar is laid out, put Sunburst in the first free slot.
barony.onBeforeGameplayStart(function(player)
	local hotbar = barony.hotbar(player)
	local sunburst = barony.inventory(player):findSpell("spell_sunburst")
	local slot = hotbar:firstEmptySlot()
	if sunburst and slot then
		hotbar:setSlot(slot, sunburst)
		barony.log("put Sunburst in hotbar slot " .. slot .. " for player " .. player)
	end
end)

barony.onSpellCast(function(spellName, player)
	-- this runs for every spell cast in the game; we only care about Sunburst
	if spellName ~= "spell_sunburst" then
		return
	end

	-- monsters and traps can cast spells too; for them `player` is nil
	if player == nil then
		return false -- still skip Light's orb
	end

	local caster = barony.player(player)
	caster.maxhp = caster.maxhp + 10
	barony.log("player " .. player .. " cast Sunburst: max HP is now " .. caster.maxhp)

	return false -- skip the default effect inherited from Light: no orb is summoned
end)
