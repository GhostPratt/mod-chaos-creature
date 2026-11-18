# mod-chaos-creature

An [AzerothCore](https://www.azerothcore.org) module that turns every level-up into a hunt.

## How it works

1. When a player reaches level 10 (configurable) or higher, each level-up spawns three random
   creatures around them. The creatures are drawn from every creature template in the game,
   critters and bosses included, and scaled to the player's level. The spawn happens about
   5 seconds after the level-up, and only if the character ended up exactly one level higher,
   so multi-level jumps (bot level randomisation, GM commands) do not start a challenge.
2. The player is told which creatures appeared and has 15 minutes to kill all of them.
3. Every kill reports how many creatures are left. A creature that disappears on its own
   (some creature scripts despawn themselves) counts as slain.
4. Killing all of them grants one permanent bonus talent point. The point is stored on the
   character by the core (`characters.extraBonusTalentCount`), so it survives logout and
   talent resets.
5. If the timer runs out, the remaining creatures despawn and nothing else happens.

Creatures are summoned as hostile world creatures (faction 14) regardless of their normal
faction, and the module keeps them that way if a creature script changes it back, so every
creature from the pool can be attacked. Invisible trigger NPCs, non-attackable or non-selectable
templates and vehicles are never picked.

Creatures that carry a C++ script (dungeon and raid bosses, event NPCs) are given a plain
aggressive AI instead of their script, because those scripts assume they run inside their
instance. This can be turned off with `ChaosCreature.ReplaceScriptedAI = 0` at your own risk.
Bosses that despawn and respawn when they reset always get the plain AI, because the respawned
copy would never despawn.

A level-up while a challenge is still running does not start a second one. Level-ups inside
battlegrounds and arenas are ignored. Logging out ends the challenge and despawns the creatures.

## Installation

1. Clone this repository into the `modules/` directory of your AzerothCore source tree.
2. Re-run CMake and build the worldserver.
3. Copy `conf/mod_chaos_creature.conf.dist` to your config directory as
   `mod_chaos_creature.conf` (CMake also installs it) and adjust the options.

No database changes are required.

## Configuration

| Option | Default | Description |
| --- | --- | --- |
| `ChaosCreature.Enable` | `1` | Enable the module. |
| `ChaosCreature.MinLevel` | `10` | First level whose level-up starts a challenge. |
| `ChaosCreature.LevelUpDelaySeconds` | `5` | Seconds the level must stay unchanged before a level-up starts a challenge. |
| `ChaosCreature.CreatureCount` | `3` | Creatures spawned per challenge. |
| `ChaosCreature.TimeLimitMinutes` | `15` | Time allowed to kill every creature. |
| `ChaosCreature.SpawnRadius` | `30` | Maximum spawn distance in yards. |
| `ChaosCreature.SpawnMinDistance` | `8` | Minimum spawn distance in yards. |
| `ChaosCreature.TalentPoints` | `1` | Permanent talent points rewarded per success. |
| `ChaosCreature.ReplaceScriptedAI` | `1` | Replace C++ creature scripts with a plain aggressive AI. |
| `ChaosCreature.IncludeCritters` | `1` | Allow critters in the pool. |
| `ChaosCreature.IncludeBosses` | `1` | Allow bosses in the pool. |

## License

Released under the [MIT license](LICENSE).
