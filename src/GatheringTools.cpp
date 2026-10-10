/*
 * mod-gathering-tools
 *
 * Mining and Skinning no longer need a Mining Pick or Skinning Knife in your bags. Carrying one
 * (or anything that counts as one, like a Gnomish Army Knife or a Hammer Pick) still pays off:
 * it gives +10 skill in that profession and makes gathering 25% faster. Players are rewarded for
 * bringing the tool but not stopped when they forget it or need the bag space.
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
 * - Faster gathering: the same aura has a second effect, cast time -25%
 *   (GatheringTools.ToolGatherSpeed). Mining and Skinning are generic spells that no spell
 *   modifier can reach through the usual family flags, so the effect has an empty class mask and
 *   a GlobalScript (OnIsAffectedBySpellModCheck) applies it to the matching profession's spells
 *   only. The auras are moved to a spell family no spell uses: a generic (family 0) modifier
 *   reaches every spell whatever its mask, which made all casts faster and Lightning Bolt
 *   instant at 3 Maelstrom Weapon stacks. With an empty mask the client is told nothing; its cast
 *   bar follows the cast time the server sends, so this needs no client patch either.
 *
 * The aura is checked every few seconds (GatheringTools.UpdateInterval) and again right before
 * any Mining or Skinning cast, so the bonus is always right at the moment it counts.
 *
 * - Bosses stay skinnable in bot groups: a corpse can only be skinned once nothing is left on it,
 *   and that includes loot the players can't see. Bots pass on quest items (Head of Onyxia) and
 *   leave their own copy of the per-player ones (Mature Black Dragon Sinew), so the boss never
 *   counted as looted. Once the players have taken everything that is theirs, quest items only
 *   bots could still loot are thrown away and the corpse becomes skinnable
 *   (GatheringTools.DiscardBotQuestLootOnBosses).
 *
 * Released under the MIT License.
 */

#include "Config.h"
#include "Creature.h"
#include "DataMap.h"
#include "Group.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace
{
    // Must match data/sql/db-world/updates and tools/patch-gathering-tools-dbc.py.
    constexpr uint32 SPELL_MINING_PICK_BONUS    = 90130;
    constexpr uint32 SPELL_SKINNING_KNIFE_BONUS = 90131;

    // No spell has this family (SharedDefines: "16 - unused"), so the tool auras' modifiers reach
    // nothing on their own. In family 0 they would reach every spell (SpellInfo::IsAffected).
    constexpr uint32 SPELLFAMILY_TOOL_BONUS = 16;

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
        int32 gatherSpeed = 25;
        uint32 updateInterval = 2000;
        bool discardBotQuestLoot = true;
    } config;

    // How often a boss's corpse is checked for loot only bots could still take.
    constexpr uint32 CORPSE_CHECK_INTERVAL = 1000;

    // The spells' stock tool requirements, so turning NoToolRequired off on a reload restores them.
    std::unordered_map<uint32, std::array<uint32, 2>> stockTotemCategories;

    struct UpdateTimer : public DataMap::Base
    {
        uint32 timer = 0;
    };

    // Which tool aura speeds up a gathering spell, from the spell's stock tool (its live tool
    // requirement may be cleared). 0 for any other spell.
    uint32 BonusSpellFor(uint32 spellId)
    {
        auto itr = stockTotemCategories.find(spellId);
        if (itr == stockTotemCategories.end())
            return 0;

        switch (itr->second[0])
        {
            case TOTEM_CATEGORY_MINING_PICK:    return SPELL_MINING_PICK_BONUS;
            case TOTEM_CATEGORY_SKINNING_KNIFE: return SPELL_SKINNING_KNIFE_BONUS;
            default:                            return 0;
        }
    }

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

        // The bonus amounts: skill, then cast time in percent (negative is faster). DieSides is 1,
        // so each effect gives BasePoints + 1.
        for (ToolBonus const& bonus : TOOL_BONUSES)
        {
            if (SpellInfo* spellInfo = const_cast<SpellInfo*>(sSpellMgr->GetSpellInfo(bonus.spellId)))
            {
                spellInfo->SpellFamilyName = SPELLFAMILY_TOOL_BONUS;
                spellInfo->Effects[EFFECT_0].BasePoints = config.skillBonus - 1;
                spellInfo->Effects[EFFECT_1].BasePoints = -config.gatherSpeed - 1;
            }
        }
    }

    // Give or take away each tool's skill aura so it matches what the player carries.
    void UpdateToolBonus(Player* player)
    {
        for (ToolBonus const& bonus : TOOL_BONUSES)
        {
            bool const wanted = (config.skillBonus > 0 || config.gatherSpeed > 0) && player->HasSkill(bonus.skill)
                && player->HasItemTotemCategory(bonus.totemCategory);
            bool const has = player->HasAura(bonus.spellId);

            if (wanted && !has)
                player->AddAura(bonus.spellId, player);
            else if (!wanted && has)
                player->RemoveAurasDueToSpell(bonus.spellId);
        }
    }

    // Bot sessions: IsHeadless() on current playerbots cores, IsBot() on older ones.
    template <typename Session, typename = void>
    struct HasIsHeadless : std::false_type { };

    template <typename Session>
    struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

    template <typename Session, typename = void>
    struct HasIsBot : std::false_type { };

    template <typename Session>
    struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

    template <typename Session>
    bool IsBotSession(Session* session)
    {
        if constexpr (HasIsHeadless<Session>::value)
            return session->IsHeadless();
        else if constexpr (HasIsBot<Session>::value)
            return session->IsBot();
        else
            return false;
    }

    bool IsBot(Player const* player)
    {
        WorldSession* session = player->GetSession();
        return session && IsBotSession(session);
    }

    bool IsQuestItem(LootItem const& item)
    {
        if (item.needs_quest)
            return true;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.itemid);
        return proto && (proto->Class == ITEM_CLASS_QUEST || proto->StartQuest);
    }

    // One of the loot's per-player lists (quest items, each player's own copy of a shared item).
    // False when an entry has to stay on the corpse: it belongs to a player, or to someone who is
    // offline and might be one, or it is a bot's but not a quest item.
    bool OnlyBotQuestItemsLeft(Creature const* creature, QuestItemMap const& lists, std::vector<LootItem> const& items,
        bool recheckPlayers)
    {
        for (auto const& [guid, list] : lists)
        {
            Player* owner = ObjectAccessor::FindConnectedPlayer(guid);
            bool const bot = owner && IsBot(owner);

            for (QuestItem const& entry : *list)
            {
                if (entry.index >= items.size())
                    continue;

                LootItem const& item = items[entry.index];
                if (entry.is_looted || item.is_looted)
                    continue;

                if (bot)
                {
                    if (!IsQuestItem(item))
                        return false;

                    continue;
                }

                // The master looter is listed for quest items they have no quest for.
                if (recheckPlayers && owner && owner->IsInMap(creature)
                    && !item.AllowedForPlayer(owner, creature->loot.sourceWorldObjectGUID))
                    continue;

                return false;
            }
        }

        return true;
    }

    // Whether all that is left on the corpse is quest items only bots could loot.
    bool OnlyBotQuestLootLeft(Creature const* creature)
    {
        Loot const& loot = creature->loot;
        Group const* group = creature->GetLootRecipientGroup();

        // The players who can loot this corpse. Bot groups are left alone, and nothing is thrown
        // away while a player is out of the instance: they could come back for it.
        std::vector<Player*> players;
        auto addLooter = [&](Player* looter)
        {
            if (!looter || IsBot(looter))
                return true;

            if (!looter->IsInMap(creature))
                return false;

            players.push_back(looter);
            return true;
        };

        if (group)
        {
            for (GroupReference const* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
                if (!addLooter(itr->GetSource()))
                    return false;
        }
        else if (!addLooter(creature->GetLootRecipient()))
            return false;

        if (players.empty())
            return false;

        // With a master looter, rolled items stay blocked until they are handed out; with any
        // other loot method a blocked item means a roll is still running.
        bool const masterLoot = group && group->GetLootMethod() == MASTER_LOOT;

        for (LootItem const& item : loot.items)
        {
            // Shared items are in the per-player lists below.
            if (item.is_looted || item.freeforall)
                continue;

            if (item.is_blocked && !masterLoot)
                return false;

            for (Player* player : players)
                if (item.AllowedForPlayer(player, loot.sourceWorldObjectGUID))
                    return false;

            // Who could loot it when the boss died, for the ones who aren't here to ask now.
            bool forBot = false;
            for (ObjectGuid const& guid : item.GetAllowedLooters())
            {
                Player* looter = ObjectAccessor::FindConnectedPlayer(guid);
                if (looter && IsBot(looter))
                    forBot = true;
                else if (std::find(players.begin(), players.end(), looter) == players.end())
                    return false;
            }

            if (forBot && !IsQuestItem(item))
                return false;
        }

        return OnlyBotQuestItemsLeft(creature, loot.GetPlayerQuestItems(), loot.quest_items, true)
            && OnlyBotQuestItemsLeft(creature, loot.GetPlayerFFAItems(), loot.items, false)
            && OnlyBotQuestItemsLeft(creature, loot.GetPlayerNonQuestNonFFAConditionalItems(), loot.items, false);
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
        config.gatherSpeed    = std::clamp(sConfigMgr->GetOption<int32>("GatheringTools.ToolGatherSpeed", 25), 0, 100);
        config.updateInterval = sConfigMgr->GetOption<uint32>("GatheringTools.UpdateInterval", 2000);
        config.discardBotQuestLoot = sConfigMgr->GetOption<bool>("GatheringTools.DiscardBotQuestLootOnBosses", true);

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

// Lets the tool auras' cast time modifier reach the gathering spells. Returning false means
// "affected"; true leaves the decision to the family and class mask, and no spell is in these
// auras' family, so they touch nothing else.
class GatheringToolsGlobalScript : public GlobalScript
{
public:
    GatheringToolsGlobalScript() : GlobalScript("GatheringToolsGlobalScript", { GLOBALHOOK_ON_IS_AFFECTED_BY_SPELL_MOD_CHECK }) { }

    bool OnIsAffectedBySpellModCheck(SpellInfo const* affectSpell, SpellInfo const* checkSpell, SpellModifier const* mod) override
    {
        if (mod->op != SPELLMOD_CASTING_TIME)
            return true;

        if (affectSpell->Id != SPELL_MINING_PICK_BONUS && affectSpell->Id != SPELL_SKINNING_KNIFE_BONUS)
            return true;

        return BonusSpellFor(checkSpell->Id) != affectSpell->Id;
    }
};

// A boss's corpse becomes skinnable once the players have looted what is theirs, even if quest
// items only bots could loot are still on it. The core only counts a corpse as looted when nothing
// at all is left, and bots leave quest items behind.
class GatheringToolsCreatureScript : public AllCreatureScript
{
public:
    GatheringToolsCreatureScript() : AllCreatureScript("GatheringToolsCreatureScript") { }

    void OnAllCreatureUpdate(Creature* creature, uint32 diff) override
    {
        if (!config.discardBotQuestLoot || creature->getDeathState() != DeathState::Corpse)
            return;

        if (!creature->IsDungeonBoss() && !creature->isWorldBoss())
            return;

        if (creature->HasUnitFlag(UNIT_FLAG_SKINNABLE) || !creature->HasDynamicFlag(UNIT_DYNFLAG_LOOTABLE))
            return;

        // Gold is for everyone, so the players haven't finished looting while it is there.
        Loot& loot = creature->loot;
        if (loot.loot_type == LOOT_SKINNING || loot.loot_type == LOOT_PICKPOCKETING || loot.gold || loot.isLooted())
            return;

        uint32 const skinLoot = creature->GetCreatureTemplate()->SkinLootId;
        if (!skinLoot || !LootTemplates_Skinning.HaveLootFor(skinLoot))
            return;

        UpdateTimer* state = creature->CustomData.GetDefault<UpdateTimer>("mod-gathering-tools");
        state->timer += diff;
        if (state->timer < CORPSE_CHECK_INTERVAL)
            return;

        state->timer = 0;
        if (!OnlyBotQuestLootLeft(creature))
            return;

        // What the core does when the last item is looted.
        creature->AllLootRemovedFromCorpse();
        creature->RemoveDynamicFlag(UNIT_DYNFLAG_LOOTABLE);
        loot.clear();
    }
};

void AddGatheringToolsScripts()
{
    new GatheringToolsWorldScript();
    new GatheringToolsGlobalScript();
    new GatheringToolsPlayerScript();
    new GatheringToolsSpellScript();
    new GatheringToolsCreatureScript();
}
