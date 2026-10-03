-- mod-gathering-tools: undo the module's world database changes. Run it by hand on the world
-- database after removing the module; AzerothCore doesn't run it automatically.
--
-- Removes the two tool bonus spells. The tool requirement changes live only in the running
-- server's memory, so there's nothing else to undo. Idempotent: safe to run again.

DELETE FROM `spell_dbc` WHERE `ID` IN (90130, 90131);
