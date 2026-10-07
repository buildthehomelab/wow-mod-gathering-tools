# mod-gathering-tools

An AzerothCore module: Mining and Skinning no longer need a Mining Pick or Skinning Knife in your
bags. Carrying one still pays off: it gives **+10 skill** in that profession and makes gathering
**25% faster**. Players are rewarded for bringing the tool, not stopped when they forget it or need the bag space.

- **No tool needed** for every rank of Mining and Skinning, and for mining a creature's corpse.
- **+10 skill with the tool** (configurable): while the tool is in your bags or equipped, the
  skill window shows a green +10. Anything that counts as the tool works, like a Gnomish Army
  Knife, Hammer Pick or Bladed Pickaxe. It counts for which nodes and corpses you can gather, the
  same as a glove enchant. Skill-ups still come from your base skill.
- **25% faster gathering with the tool** (configurable): mining a node or skinning a corpse takes
  a quarter less time, and the cast bar shows it.

## How it works

- The tool requirement (Spell.dbc "totem category" Mining Pick 165 / Skinning Knife 166) is
  cleared in the server's in-memory spell data on spells 2575, 2576, 3564, 10248, 29354, 50310
  (Mining), 8613, 8617, 8618, 10768, 32678, 50305 (Skinning) and 32606 (creature mining).
- The bonus is a hidden passive Mod Skill aura on two new server-only spells, `spell_dbc` 90130
  (Mining) and 90131 (Skinning), copies of Finkle's Skinner's +10 Skinning. The module gives or
  removes it every `GatheringTools.UpdateInterval` ms and again right before any Mining or
  Skinning cast, so a tool you just picked up already counts.
- The faster gathering is a second effect on the same aura: cast time −25%, with an empty class
  mask, so it can't touch any other spell. Mining and Skinning are generic spells no modifier
  reaches through the usual family flags, so a `GlobalScript` (`OnIsAffectedBySpellModCheck`)
  applies it to the matching profession's spells only.

## Client patch (required for "no tool needed")

The 3.3.5a client checks a spell's tool itself and says "Requires Mining Pick" without asking the
server. `tools/patch-gathering-tools-dbc.py` clears the same fields in the client's Spell.dbc.
Start from the Spell.dbc your realm patch already ships so its other changes stay:

```bash
python3 tools/patch-gathering-tools-dbc.py --from-mpq patch-P.MPQ --out patch-P.MPQ.new
```

Packing needs StormLib (`STORMLIB=/path/to/libstorm.dylib`). The skill bonus and the faster
gathering need no client change: the client's cast bar follows the cast time the server sends.

## Requirements

- [AzerothCore](https://www.azerothcore.org/) wotlk (master) and a WoW 3.3.5a (12340) client.
- A client patch for "no tool needed" (see above). Python 3 and
  [StormLib](https://github.com/ladislav-zezula/StormLib) are needed to build it. The skill bonus
  and the faster gathering work without it.

## Install

```bash
cd modules
git clone https://github.com/buildthehomelab/wow-mod-gathering-tools.git mod-gathering-tools
```

Clone it into `mod-gathering-tools` (without `wow-`): AzerothCore names the loader after the
folder. Rebuild, copy `conf/mod_gathering_tools.conf.dist` to `mod_gathering_tools.conf`, and
start the server; the world SQL in `data/sql/db-world` is applied automatically.

## Configuration

| Setting | Default | |
|---|---|---|
| `GatheringTools.NoToolRequired` | 1 | Mining and Skinning work without the tool (needs the client patch). |
| `GatheringTools.ToolSkillBonus` | 10 | Skill bonus while carrying the tool; 0 turns it off. |
| `GatheringTools.ToolGatherSpeed` | 25 | Percent faster gathering while carrying the tool; 0 turns it off. |
| `GatheringTools.UpdateInterval` | 2000 | How often (ms) the bonus is checked against your bags. |

All four apply on `.reload config`.

## Uninstall

Remove the module and run `data/sql/uninstall/mod_gathering_tools_uninstall_world.sql` on the
world database.

## Troubleshooting

- **The client still says "Requires Mining Pick" (or Skinning Knife).** The client checks the tool
  itself, so "no tool needed" only works with the client patch. Build it from the Spell.dbc your
  realm patch already ships so its other changes stay.
- **The +10 skill or the faster gathering doesn't show right after picking up the tool.** The bags
  are checked every `GatheringTools.UpdateInterval` ms and again right before each Mining or
  Skinning cast, so it is in place by the time you gather.
- **The patch script can't pack the MPQ.** Set `STORMLIB=/path/to/libstorm.dylib` to your
  StormLib build.

## Credits

Author: [buildthehomelab](https://github.com/buildthehomelab)

## License

MIT, see [LICENSE](LICENSE).
