-- mod-gathering-tools: faster gathering with the tool.
--
-- Gives the tool bonus auras (90130 Mining Pick, 90131 Skinning Knife) a second effect: Add
-- Percent Modifier (108) to cast time (SPELLMOD_CASTING_TIME, 10), with an empty class mask. The
-- module sets the amount from GatheringTools.ToolGatherSpeed at startup and applies it to the
-- matching profession's spells itself; with the empty mask it touches no other spell.

UPDATE `spell_dbc` SET `Effect_2` = 6, `EffectDieSides_2` = 1, `EffectBasePoints_2` = -26,
    `ImplicitTargetA_2` = 1, `EffectAura_2` = 108, `EffectMiscValue_2` = 10
WHERE `ID` IN (90130, 90131);
