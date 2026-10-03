# mod-gathering-tools

An AzerothCore module: Mining and Skinning no longer need a Mining Pick or Skinning Knife in your
bags. Carrying one still pays off: it gives **+10 skill** in that profession. Players are
rewarded for bringing the tool, not stopped when they forget it or need the bag space.

- **No tool needed** for every rank of Mining and Skinning, and for mining a creature's corpse.
- **+10 skill with the tool** (configurable): while the tool is in your bags or equipped, the
  skill window shows a green +10. Anything that counts as the tool works, like a Gnomish Army
  Knife, Hammer Pick or Bladed Pickaxe. It counts for which nodes and corpses you can gather, the
  same as a glove enchant. Skill-ups still come from your base skill.

## How it works

- The tool requirement (Spell.dbc "totem category" Mining Pick 165 / Skinning Knife 166) is
  cleared in the server's in-memory spell data on spells 2575, 2576, 3564, 10248, 29354, 50310
  (Mining), 8613, 8617, 8618, 10768, 32678, 50305 (Skinning) and 32606 (creature mining).
- The bonus is a hidden passive Mod Skill aura on two new server-only spells, `spell_dbc` 90130
  (Mining) and 90131 (Skinning), copies of Finkle's Skinner's +10 Skinning. The module gives or
  removes it every `GatheringTools.UpdateInterval` ms and again right before any Mining or
  Skinning cast, so a tool you just picked up already counts.

## Client patch (required for "no tool needed")

The 3.3.5a client checks a spell's tool itself and says "Requires Mining Pick" without asking the
server. `tools/patch-gathering-tools-dbc.py` clears the same fields in the client's Spell.dbc.
Start from the Spell.dbc your realm patch already ships so its other changes stay:

```bash
python3 tools/patch-gathering-tools-dbc.py --from-mpq patch-P.MPQ --out patch-P.MPQ.new
```

Packing needs StormLib (`STORMLIB=/path/to/libstorm.dylib`). The skill bonus needs no client
change.

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
| `GatheringTools.UpdateInterval` | 2000 | How often (ms) the bonus is checked against your bags. |

All three apply on `.reload config`.

## Uninstall

Remove the module and run `data/sql/uninstall/mod_gathering_tools_uninstall_world.sql` on the
world database.

## License

MIT
