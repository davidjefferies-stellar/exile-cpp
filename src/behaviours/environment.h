#pragma once
#include "objects/object.h"
#include "behaviours/npc_helpers.h"

namespace Behaviors {

void update_door(Object& obj, UpdateContext& ctx);
void update_switch(Object& obj, UpdateContext& ctx);
void update_transporter_beam(Object& obj, UpdateContext& ctx);
void update_hive(Object& obj, UpdateContext& ctx);
void update_dense_nest(Object& obj, UpdateContext& ctx);
void update_sucking_nest(Object& obj, UpdateContext& ctx);
void update_bush(Object& obj, UpdateContext& ctx);
void update_cannon(Object& obj, UpdateContext& ctx);
void update_maggot_machine(Object& obj, UpdateContext& ctx);
void update_engine_fire(Object& obj, UpdateContext& ctx);
void update_placeholder(Object& obj, UpdateContext& ctx);

// ------ Debug / overlay helpers ---------------------------------------
// Expose the static 6502 tables so the "Wiring" overlay can draw
// switch→door and transporter→destination arrows without duplicating
// data. Both return at most `max_out` tertiary data offsets (switches)
// or resolve a 16-entry destination index (transporters).

// Port of the &4958 switch_effects_table lookup: writes up to `max_out`
// tertiary data offsets that effect_id toggles into `out`. Returns the
// count written.
int  switch_effect_targets(uint8_t effect_id,
                           uint8_t* out, int max_out);

// Port of the transporter destination tables at &314a / &315a. Returns
// true and sets (out_x, out_y) if `destination` is in range [0, 16);
// false otherwise.
bool transporter_destination(uint8_t destination,
                             uint8_t& out_x, uint8_t& out_y);

} // namespace Behaviors
