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
 *   and that includes loot the players can't see. Bots pass on quest items (Head of Onyxia),
 *   leave their own copy of the per-player ones (Mature Black Dragon Sinew), and can win a unique
 *   item they already carry, which the core then keeps on the corpse for them. So the boss never
 *   counted as looted. Once the players have taken everything they can, what is left is thrown
 *   away and the corpse becomes skinnable (GatheringTools.DiscardBotQuestLootOnBosses). A unique
 *   item a player already carries doesn't count as theirs either: they can't loot a second one.
 *
 * Released under the MIT License.
 */

#include "Config.h"
#include "Creature.h"
#include "DataMap.h"
#include "GameTime.h"
#include "Group.h"
#include "Log.h"
#include "LootMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <string>
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

    // What keeps a boss from being skinned is logged when it changes, and again after this long.
    constexpr time_t CORPSE_LOG_INTERVAL = 5 * MINUTE;

    // The spells' stock tool requirements, so turning NoToolRequired off on a reload restores them.
    std::unordered_map<uint32, std::array<uint32, 2>> stockTotemCategories;

    struct UpdateTimer : public DataMap::Base
    {
        uint32 timer = 0;
    };

    struct CorpseCheck : public DataMap::Base
    {
        uint32 timer = 0;
        std::string loggedReason;
        time_t loggedAt = 0;
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

    // Whether the player could still take this item off the corpse. Not when it is reserved for
    // the winner of its roll (the core keeps it for a winner who couldn't carry it), and not when
    // it is a unique item the player already carries.
    bool CanStillLoot(Player const* player, LootItem const& item, ObjectGuid source)
    {
        if (!item.AllowedForPlayer(player, source))
            return false;

        if (item.rollWinnerGUID && item.rollWinnerGUID != player->GetGUID())
            return false;

        ItemPosCountVec dest;
        return player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, item.itemid, item.count) != EQUIP_ERR_CANT_CARRY_MORE_OF_THIS;
    }

    std::string ItemName(uint32 itemId)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        return Acore::StringFormat("{} ({})", proto ? proto->Name1 : "unknown item", itemId);
    }

    // Whether this character may still loot the corpse at all. Someone who has left the group
    // since the kill may not, so what was theirs is nobody's.
    bool HasLootRights(Creature const* creature, Group const* group, ObjectGuid guid)
    {
        return group ? group->IsMember(guid) : creature->GetLootRecipientGUID() == guid;
    }

    // One of the loot's per-player lists (quest items, each player's own copy of a shared item,
    // items that drop on a condition). False when an entry has to stay on the corpse: a player
    // could still take it, or it belongs to someone who is offline and might be a player.
    // `reason` then says which, for the log.
    bool OnlyBotItemsLeft(Creature const* creature, Group const* group, QuestItemMap const& lists,
        std::vector<LootItem> const& items, std::string& reason)
    {
        for (auto const& [guid, list] : lists)
        {
            if (!HasLootRights(creature, group, guid))
                continue;

            Player* owner = ObjectAccessor::FindConnectedPlayer(guid);
            if (owner && IsBot(owner))
                continue;

            for (QuestItem const& entry : *list)
            {
                if (entry.index >= items.size())
                    continue;

                LootItem const& item = items[entry.index];
                if (entry.is_looted || item.is_looted)
                    continue;

                // The master looter is listed for quest items they have no quest for.
                if (owner && owner->IsInMap(creature) && !CanStillLoot(owner, item, creature->loot.sourceWorldObjectGUID))
                    continue;

                reason = owner ? Acore::StringFormat("{} can still loot {}", owner->GetName(), ItemName(item.itemid))
                    : Acore::StringFormat("{} is kept for a character who is offline", ItemName(item.itemid));
                return false;
            }
        }

        return true;
    }

    // Whether all that is left on the corpse is loot no player of the group could take. When it
    // isn't, `reason` says what is in the way, for the log; it stays empty for a group of bots.
    bool OnlyBotLootLeft(Creature const* creature, std::string& reason)
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
            {
                reason = Acore::StringFormat("{} is outside the instance", looter->GetName());
                return false;
            }

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

        // Gold is for everyone, so the players haven't finished looting while it is there.
        if (loot.gold)
        {
            reason = "the gold is still on the corpse";
            return false;
        }

        // Nothing is thrown away while the group still rolls for something on this corpse. The
        // rolls themselves are asked, not the items' "blocked" mark: with Group Loot the core
        // sets that mark on an item everyone is passed on automatically and never takes it off
        // again. One faction's Head of Onyxia is such an item on every kill by the other faction.
        // Rolls only run while the corpse's roll timer does; the group's list of them belongs to
        // every map its members are on, so it isn't read when there is no need.
        if (group && creature->m_groupLootTimer)
        {
            for (Roll const* roll : group->GetRolls())
            {
                if (roll->getTarget() != &loot)
                    continue;

                reason = Acore::StringFormat("the group is still rolling for {}", ItemName(roll->itemid));
                return false;
            }
        }

        // With a master looter, rolled items stay blocked until they are handed out.
        bool const masterLoot = group && group->GetLootMethod() == MASTER_LOOT;

        for (LootItem const& item : loot.items)
        {
            // Shared items are in the per-player lists below.
            if (item.is_looted || item.freeforall)
                continue;

            // No roll is running, so this is an item the core left blocked after a roll nobody
            // could join. It lets nobody loot a blocked item.
            if (item.is_blocked && !masterLoot)
                continue;

            for (Player* player : players)
            {
                if (!CanStillLoot(player, item, loot.sourceWorldObjectGUID))
                    continue;

                reason = Acore::StringFormat("{} can still loot {}", player->GetName(), ItemName(item.itemid));
                return false;
            }

            // Who could loot it when the boss died, for the ones who aren't here to ask now.
            for (ObjectGuid const& guid : item.GetAllowedLooters())
            {
                if (!HasLootRights(creature, group, guid))
                    continue;

                Player* looter = ObjectAccessor::FindConnectedPlayer(guid);
                if (looter && IsBot(looter))
                    continue;

                if (std::find(players.begin(), players.end(), looter) != players.end())
                    continue;

                reason = Acore::StringFormat("{} is kept for a character who is offline", ItemName(item.itemid));
                return false;
            }
        }

        return OnlyBotItemsLeft(creature, group, loot.GetPlayerQuestItems(), loot.quest_items, reason)
            && OnlyBotItemsLeft(creature, group, loot.GetPlayerFFAItems(), loot.items, reason)
            && OnlyBotItemsLeft(creature, group, loot.GetPlayerNonQuestNonFFAConditionalItems(), loot.items, reason);
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

// A boss's corpse becomes skinnable once the players have looted what they can, even if loot only
// bots could take is still on it. The core only counts a corpse as looted when nothing at all is
// left, and bots leave quest items and unique items they already carry behind.
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

        Loot& loot = creature->loot;
        if (loot.loot_type == LOOT_SKINNING || loot.loot_type == LOOT_PICKPOCKETING)
            return;

        uint32 const skinLoot = creature->GetCreatureTemplate()->SkinLootId;
        if (!skinLoot || !LootTemplates_Skinning.HaveLootFor(skinLoot))
            return;

        CorpseCheck* state = creature->CustomData.GetDefault<CorpseCheck>("mod-gathering-tools");
        state->timer += diff;
        if (state->timer < CORPSE_CHECK_INTERVAL)
            return;

        state->timer = 0;

        // The last item can also leave the corpse through a roll, when its winner gets it. The
        // core only notices an empty corpse when someone closes its loot window, and by then
        // nobody has a reason to open it: the corpse would stay lootable and unskinnable.
        std::string reason;
        if (!loot.isLooted() && !OnlyBotLootLeft(creature, reason))
        {
            // So a boss that stays unskinnable can be explained from Server.log.
            time_t const now = GameTime::GetGameTime().count();
            if (!reason.empty() && (reason != state->loggedReason || now >= state->loggedAt + CORPSE_LOG_INTERVAL))
            {
                LOG_INFO("module", "mod-gathering-tools: {} can't be skinned yet: {}", creature->GetName(), reason);
                state->loggedReason = reason;
                state->loggedAt = now;
            }

            return;
        }

        if (loot.isLooted())
            LOG_DEBUG("module", "mod-gathering-tools: {} can be skinned now, its last loot went to a roll winner", creature->GetName());
        else
            LOG_INFO("module", "mod-gathering-tools: {} can be skinned now, what only bots could loot was thrown away",
                creature->GetName());

        state->loggedReason.clear();

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
