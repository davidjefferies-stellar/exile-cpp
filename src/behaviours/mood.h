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

} // namespace Mood
