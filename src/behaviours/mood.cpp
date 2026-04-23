#include "behaviours/mood.h"
#include "core/types.h"

// NPC stimuli response tables from &316b-&31ab.
// Each NPC type maps to a stimulus category (0-9) via object_type_ranges.
// Each stimulus category has response flags determining mood changes.

// Stimulus categories: which object types are phobias, targets, food, home
// From &316b (10 entries each)
static constexpr uint8_t npc_phobia_table[] = {
    0x37, 0x12, 0x00, 0x13, 0x11, 0x00, 0x0e, 0x37, 0x13, 0x00
};
static constexpr uint8_t npc_target_table[] = {
    0x00, 0x11, 0x00, 0x0a, 0x10, 0x00, 0x10, 0x00, 0x0f, 0x00
};
static constexpr uint8_t npc_food_table[] = {
    0x4b, 0x00, 0x00, 0x58, 0x00, 0x00, 0x00, 0x4b, 0x00, 0x00
};
static constexpr uint8_t npc_home_table[] = {
    0x0a, 0x0a, 0x00, 0x0a, 0x04, 0x00, 0x04, 0x0a, 0x0a, 0x00
};

// Response bits: determines whether each stimulus improves or worsens mood
// Bit mapping: 7=time, 6=flood, 5=damage, 4=explode, 3=eating, 2=player, 1=phobia, 0=target
static constexpr uint8_t npc_response_table[] = {
    0x36, 0x55, 0x00, 0x0e, 0x34, 0x00, 0x24, 0x26, 0x1a, 0x00
};

namespace Mood {

uint8_t get_mood(const Object& npc) {
    return npc.state & NPCMood::MASK;
}

void set_mood(Object& npc, uint8_t mood) {
    npc.state = (npc.state & ~NPCMood::MASK) | (mood & NPCMood::MASK);
}

void update_mood(Object& npc, UpdateContext& ctx) {
    // Only update every 64 frames
    if (!ctx.every_sixty_four_frames) return;

    // Determine NPC's stimulus category from type range
    uint8_t type_idx = static_cast<uint8_t>(npc.type);
    int category = 0;
    // Use same range lookup as energy system
    static constexpr uint8_t ranges[] = {0x00, 0x04, 0x06, 0x0f, 0x12, 0x1c, 0x22, 0x32, 0x38, 0x4a};
    for (int i = 9; i >= 0; i--) {
        if (type_idx >= ranges[i]) {
            category = i;
            break;
        }
    }

    uint8_t response = npc_response_table[category];
    if (response == 0) return; // No mood system for this category

    uint8_t current_mood = get_mood(npc);

    // Check stimuli and adjust mood
    uint8_t stimuli_present = 0;

    // Check if player is nearby (within ~8 tiles)
    const Object& player = ctx.mgr.player();
    int8_t dx = static_cast<int8_t>(npc.x.whole - player.x.whole);
    int8_t dy = static_cast<int8_t>(npc.y.whole - player.y.whole);
    if (std::abs(dx) < 8 && std::abs(dy) < 8) {
        stimuli_present |= 0x04; // Player nearby
    }

    // Check if recently damaged (top bit of state used as damage flag)
    if (npc.state & 0x08) {
        stimuli_present |= 0x20; // Damage stimulus
        npc.state &= ~0x08; // Clear damage flag
    }

    // Random stimulus (1 in 2 chance)
    if (ctx.rng.next() & 0x01) {
        stimuli_present |= 0x01; // Random target
    }

    // Time stimulus (every 256 frames)
    if ((ctx.frame_counter & 0xFF) == 0) {
        stimuli_present |= 0x80; // Time
    }

    // For each active stimulus, check if it matches the response pattern
    uint8_t positive_stimuli = stimuli_present & response;
    uint8_t negative_stimuli = stimuli_present & ~response;

    // Mood adjustment: positive stimuli increase mood, negative decrease
    if (positive_stimuli && !negative_stimuli) {
        // Improve mood (toward PLUS_ONE)
        if (current_mood == NPCMood::MINUS_TWO) set_mood(npc, NPCMood::MINUS_ONE);
        else if (current_mood == NPCMood::MINUS_ONE) set_mood(npc, NPCMood::ZERO);
        else if (current_mood == NPCMood::ZERO) set_mood(npc, NPCMood::PLUS_ONE);
    } else if (negative_stimuli && !positive_stimuli) {
        // Worsen mood (toward MINUS_TWO)
        if (current_mood == NPCMood::PLUS_ONE) set_mood(npc, NPCMood::ZERO);
        else if (current_mood == NPCMood::ZERO) set_mood(npc, NPCMood::MINUS_ONE);
        else if (current_mood == NPCMood::MINUS_ONE) set_mood(npc, NPCMood::MINUS_TWO);
    }
}

} // namespace Mood
