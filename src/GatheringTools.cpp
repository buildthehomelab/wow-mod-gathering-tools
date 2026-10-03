/*
 * mod-gathering-tools
 *
 * Mining and Skinning no longer need a Mining Pick or Skinning Knife in your bags. Carrying one
 * (or anything that counts as one, like a Gnomish Army Knife or a Hammer Pick) still pays off:
 * it gives +10 skill in that profession. Players are rewarded for bringing the tool but not
 * stopped when they forget it or need the bag space.
 *
 * - No tool needed: the tool requirement (Spell.dbc "totem category" Mining Pick 165 / Skinning
 *   Knife 166) is cleared in the server's copy of every Mining and Skinning rank, and of the
 *   spell that mines a creature's corpse. The client checks the tool itself before it asks the
 *   server, so this part needs the client patch (tools/patch-gathering-tools-dbc.py).
 *
 * - Tool bonus: while the player has the profession and the tool is in their bags or equipped,
 *   they carry a hidden passive aura that raises the skill, so the skill window shows it as a
 *   green "+10". It's the same kind of bonus as a glove enchant, so it counts for which nodes and
 *   corpses they can gather but not for skill-ups, which the core works out from the base skill.
 *   The auras are two new server-only spells (spell_dbc 90130 Mining, 90131 Skinning); the client
 *   never sees them, so this part needs no client patch.
 *
 * The aura is checked every few seconds (GatheringTools.UpdateInterval) and again right before
 * any Mining or Skinning cast, so the bonus is always right at the moment it counts.
 *
 * Released under the MIT License.
 */

#include "Config.h"
#include "DataMap.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <array>
#include <unordered_map>

namespace
{
    // Must match data/sql/db-world/updates and tools/patch-gathering-tools-dbc.py.
    constexpr uint32 SPELL_MINING_PICK_BONUS    = 90130;
    constexpr uint32 SPELL_SKINNING_KNIFE_BONUS = 90131;

    constexpr uint32 TOTEM_CATEGORY_MINING_PICK    = 165;
    constexpr uint32 TOTEM_CATEGORY_SKINNING_KNIFE = 166;

    // Every Mining and Skinning rank, and creature mining (32606). Must match the client patch.
    constexpr uint32 TOOL_SPELLS[] =
    {
        2575, 2576, 3564, 10248, 29354, 50310,  // Mining, Apprentice to Grand Master
        8613, 8617, 8618, 10768, 32678, 50305,  // Skinning, Apprentice to Grand Master
        32606,                                  // Mining a creature's corpse
    };

    struct ToolBonus
    {
        uint32 skill;
        uint32 totemCategory;
        uint32 spellId;
    };

    constexpr ToolBonus TOOL_BONUSES[] =
    {
        { SKILL_MINING,   TOTEM_CATEGORY_MINING_PICK,    SPELL_MINING_PICK_BONUS },
        { SKILL_SKINNING, TOTEM_CATEGORY_SKINNING_KNIFE, SPELL_SKINNING_KNIFE_BONUS },
    };

    struct
    {
        bool noToolRequired = true;
        int32 skillBonus = 10;
        uint32 updateInterval = 2000;
    } config;

    // The spells' stock tool requirements, so turning NoToolRequired off on a reload restores them.
    std::unordered_map<uint32, std::array<uint32, 2>> stockTotemCategories;

    struct UpdateTimer : public DataMap::Base
    {
        uint32 timer = 0;
    };

    bool IsToolSpell(uint32 spellId)
    {
        for (uint32 id : TOOL_SPELLS)
            if (id == spellId)
                return true;

        return false;
    }

    void ApplySpellChanges()
    {
        for (uint32 spellId : TOOL_SPELLS)
        {
            SpellInfo* spellInfo = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(spellId));
            if (!spellInfo)
                continue;

            auto const& stock = stockTotemCategories.try_emplace(spellId, spellInfo->TotemCategory).first->second;
            if (config.noToolRequired)
                spellInfo->TotemCategory = { 0, 0 };
            else
                spellInfo->TotemCategory = stock;
        }

        // The bonus amount. DieSides is 1, so the aura gives BasePoints + 1.
        for (ToolBonus const& bonus : TOOL_BONUSES)
            if (SpellInfo* spellInfo = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(bonus.spellId)))
                spellInfo->Effects[EFFECT_0].BasePoints = config.skillBonus - 1;
    }

    // Give or take away each tool's skill aura so it matches what the player carries.
    void UpdateToolBonus(Player* player)
    {
        for (ToolBonus const& bonus : TOOL_BONUSES)
        {
            bool const wanted = config.skillBonus > 0 && player->HasSkill(bonus.skill)
                && player->HasItemTotemCategory(bonus.totemCategory);
            bool const has = player->HasAura(bonus.spellId);

            if (wanted && !has)
                player->AddAura(bonus.spellId, player);
            else if (!wanted && has)
                player->RemoveAurasDueToSpell(bonus.spellId);
        }
    }
}

class GatheringToolsWorldScript : public WorldScript
{
public:
    GatheringToolsWorldScript() : WorldScript("GatheringToolsWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        config.noToolRequired = sConfigMgr->GetOption<bool>("GatheringTools.NoToolRequired", true);
        config.skillBonus     = sConfigMgr->GetOption<int32>("GatheringTools.ToolSkillBonus", 10);
        config.updateInterval = sConfigMgr->GetOption<uint32>("GatheringTools.UpdateInterval", 2000);

        // At startup the spells aren't loaded yet; OnBeforeWorldInitialized does it then.
        if (!reload)
            return;

        ApplySpellChanges();

        // A running aura keeps the amount it was cast with, so take them all off; the next update
        // puts them back with the new amount.
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            for (ToolBonus const& bonus : TOOL_BONUSES)
                player->RemoveAurasDueToSpell(bonus.spellId);
    }

    void OnBeforeWorldInitialized() override
    {
        ApplySpellChanges();
    }
};

class GatheringToolsPlayerScript : public PlayerScript
{
public:
    GatheringToolsPlayerScript() : PlayerScript("GatheringToolsPlayerScript",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_UPDATE }) { }

    void OnPlayerLogin(Player* player) override
    {
        UpdateToolBonus(player);
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        UpdateTimer* state = player->CustomData.GetDefault<UpdateTimer>("mod-gathering-tools");
        state->timer += diff;
        if (state->timer < config.updateInterval)
            return;

        state->timer = 0;
        UpdateToolBonus(player);
    }
};

// Right before the cast is checked (this hook runs ahead of the skill and tool checks), so a tool
// picked up a moment ago already counts.
class GatheringToolsSpellScript : public AllSpellScript
{
public:
    GatheringToolsSpellScript() : AllSpellScript("GatheringToolsSpellScript", { ALLSPELLHOOK_ON_SPELL_CHECK_CAST }) { }

    void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& /*res*/) override
    {
        if (!IsToolSpell(spell->GetSpellInfo()->Id))
            return;

        if (Player* player = spell->GetCaster()->ToPlayer())
            UpdateToolBonus(player);
    }
};

void AddGatheringToolsScripts()
{
    new GatheringToolsWorldScript();
    new GatheringToolsPlayerScript();
    new GatheringToolsSpellScript();
}
