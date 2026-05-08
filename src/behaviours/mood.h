#pragma once
#include "objects/object.h"
#include "behaviours/npc_helpers.h"

// NPC mood and stimuli system - port of &27c9-&2866, &316b-&31ab
namespace Mood {

// Update NPC mood based on environmental stimuli.
// Called every 64 frames for each NPC.
void update_mood(Object& npc, UpdateContext& ctx);

// Get mood value from state byte (top 2 bits)
uint8_t get_mood(const Object& npc);

// Set mood value in state byte
void set_mood(Object& npc, uint8_t mood);

// True if the type has an entry in the &316b stimuli tables and a
// mood field worth displaying. False for projectiles, statics,
// collectables, and creatures the 6502 doesn't run check_for_npc_
// stimuli on (birds, wasps, piranhas, gargoyles, …).
bool has_category(ObjectType type);

} // namespace Mood
