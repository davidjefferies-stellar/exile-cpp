#include "game/game.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "rendering/sprite_atlas.h"
#include "world/tile_data.h"

// Port of the body of create_primary_object_from_tertiary (&4042) combined
// with the type-selection logic from the individual tile update routines
// (update_metal_door_tile &3e98, update_stone_door_tile &3e95,
// update_switch_tile &3fcd, update_tile_with_object_from_type &3fb7,
// update_tile_with_object_from_data &3fbf, update_transporter_tile &3ee3).
//
// For tiles that spawn objects from tertiary data, picks the correct object
// type, creates a primary object at (tile_x, tile_y) with a sub-tile offset
// matching the 6502's flip-aware formula, copies the tile's flip bits onto
// the object's flags, and clears bit 7 of the tertiary data byte so the
// object doesn't respawn.
void Game::spawn_tertiary_object(uint8_t tile_type, uint8_t tile_flip,
                                 uint8_t tile_x, uint8_t tile_y,
                                 int data_offset, int type_offset,
                                 uint8_t raw_tile_type) {
    // TILE_INVISIBLE_SWITCH's update routine (&3ef2) is only called from
    // collision handling — not during tile plotting — so no primary object
    // is ever spawned for it. Reaching this routine for that tile type
    // means the render loop is calling us for every visible invisible-
    // switch tile every frame; bail early so the diagnostics don't count it.
    if (tile_is(tile_type, TileType::INVISIBLE_SWITCH)) return;

    // "Redirect" case: the LANDSCAPE tile (raw_tile_type) is a
    // CHECK_TERTIARY marker (0x00..0x08) but the tertiary entry rewrites
    // the rendered / dispatched tile to something else (a door, switch,
    // transporter beam, etc.). We detect this by raw != resolved.
    //
    // Two classes of doors exist in the 6502's ROM tertiary data:
    //   - bit 7 SET in the data byte: canonical "needs spawning"; the
    //     6502's create_primary_object_from_tertiary honours this and
    //     produces a primary object with animation.
    //   - bit 7 CLEAR: the 6502 skips spawning and relies entirely on
    //     tile-level handling — a `door_tiles_table` substitution at
    //     obstruction time makes a "closed" door block and the render
    //     layer draws the door tile directly. No object is created, so
    //     the door never animates.
    //
    // Our port animates doors via primary objects, so for any tertiary
    // redirect we want to spawn regardless of bit 7. De-dup the spawns
    // via the "primary already owns this tertiary_slot" scan instead of
    // the bit-7 gate (since clearing bit 7 in redirect data would also
    // corrupt the switch-effects number packed into the same byte when
    // the landscape tile is TILE_INVISIBLE_SWITCH).
    bool redirect = (raw_tile_type != tile_type);

    // Non-redirect tertiary objects still use the 6502's bit-7 gate: the
    // value is a straight "needs-spawning" flag with no co-tenant bits.
    if (!redirect && data_offset != 0 &&
        !(object_mgr_.tertiary_data_byte(data_offset) & 0x80)) {
        return;
    }

    // Dedup for redirects: if a primary already owns this tertiary_slot,
    // don't spawn a duplicate on this render tick.
    if (redirect && data_offset > 0) {
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& p = object_mgr_.object(i);
            if (p.is_active() &&
                p.tertiary_slot == static_cast<uint8_t>(data_offset)) {
                return;
            }
        }
    }

    // Only the INVISIBLE_SWITCH redirect packs live switch-effect bits
    // into the tertiary data byte, so only that case needs to preserve
    // bit 7 when copying to the primary's data field below. Other
    // redirects (transporter → door, etc.) use the 6502's standard door
    // bit layout and can be stripped like any non-redirect.
    bool switch_redirect = tile_is(raw_tile_type, TileType::INVISIBLE_SWITCH);

    // On the BBC the visible viewport was ~8 tiles, and object spawning
    // piggybacked on tile plotting — so you could never spawn a tile that
    // was further from the player than ~4 tiles. Our viewport is much
    // wider (up to ~40 tiles at full zoom-out), and a 12-tile gate causes
    // a visible per-frame churn for tile_type 0x02:
    //   - placeholder (type 0x49) flags 0x74 → check_demotion picks
    //     X=3 → distance 4.
    //   - render spawns a placeholder anywhere within 12 tiles.
    //   - 1-in-4 frames check_demotion fires; placeholders at 5..12
    //     tiles get sent back to tertiary (bit 7 re-armed).
    //   - next render frame the same tile spawns the placeholder again.
    //
    // The 6502 doesn't see this because its viewport never put placeholders
    // beyond their demotion radius. We pick the gate distance based on the
    // tile type so it matches what `check_demotion` will actually keep:
    //   - tile 0x02 (placeholder) and 0x08 (switch): distance 4.
    //   - tile 0x01 (transporter), doors, FROM_TYPE: distance 12.
    //
    // Anything beyond the chosen radius would be demoted immediately, so
    // skipping the spawn keeps the primary list stable without changing
    // observable behaviour.
    // Almost every tile-spawned object is stationary on a tile and ends
    // up "slow + supported" once spawned, which in `check_demotion`
    // bumps X from 2 → 3 (distance 12 → distance 4):
    //   placeholder  flags 0x74 → X=3       → 4
    //   switch       flags 0x15 → X=1       → 1
    //   door         flags 0x6b → X=2 + slow → 4
    //   transporter  flags 0x6f → X=2 + slow → 4
    //   from-type    type-dependent, but stationary collectables
    //                (PROTECTION_SUIT etc.) are X=2 + slow → 4
    //
    // So a single "spawn within 4 tiles" gate matches the actual demotion
    // radius for every tile type. Spawning further out used to create a
    // primary that check_demotion immediately reaped (1-in-4 frames) and
    // the next render frame respawned — visible per-frame churn.
    //
    // The 6502 doesn't need this gate because its viewport was small
    // enough that tile plotting only ever fired within the keep range.
    {
        int8_t dx = static_cast<int8_t>(tile_x - object_mgr_.activation_anchor_x());
        int8_t dy = static_cast<int8_t>(tile_y - object_mgr_.activation_anchor_y());
        uint8_t adx = static_cast<uint8_t>(dx < 0 ? -dx : dx);
        uint8_t ady = static_cast<uint8_t>(dy < 0 ? -dy : dy);
        // Settable via exile.ini [distances] spawn_tertiary. See Game::init.
        if (adx > spawn_tertiary_distance_ ||
            ady > spawn_tertiary_distance_) return;
    }

    // Diagnostic: only count attempts that passed both gates — i.e. visible
    // tertiary tiles whose data byte is still armed AND which are close
    // enough to the anchor to actually produce a primary object. Counting
    // earlier would tick up constantly from tiles we always reject, even
    // while paused (render runs in paused mode), which tells us nothing.
    spawn_attempts_++;

    bool vertical_door = (tile_flip == TileFlip::HORIZONTAL) ||
                         (tile_flip == TileFlip::VERTICAL);

    TileType ttype = static_cast<TileType>(tile_type);
    uint8_t obj_type = 0xff;
    switch (ttype) {
        case TileType::TRANSPORTER:
            obj_type = static_cast<uint8_t>(ObjectType::TRANSPORTER_BEAM);
            break;
        case TileType::SPACE_WITH_OBJECT_FROM_DATA:
            // data_offset == 0 means "no tertiary data byte for this slot"
            // (matches the 6502's &bd == 0 sentinel at &1717). Without this
            // guard we'd read tertiary_data_byte(0) == 0 and silently try
            // to spawn an OBJECT_PLAYER (type 0) at the wrong position.
            if (data_offset == 0) return;
            obj_type = object_mgr_.tertiary_data_byte(data_offset) & 0x7f;
            if (obj_type == 0) return;     // Nothing to spawn — bail cleanly.
            break;
        case TileType::METAL_DOOR:
            obj_type = static_cast<uint8_t>(
                vertical_door ? ObjectType::VERTICAL_METAL_DOOR
                              : ObjectType::HORIZONTAL_METAL_DOOR);
            break;
        case TileType::STONE_DOOR:
            obj_type = static_cast<uint8_t>(
                vertical_door ? ObjectType::VERTICAL_STONE_DOOR
                              : ObjectType::HORIZONTAL_STONE_DOOR);
            break;
        case TileType::STONE_HALF_WITH_OBJECT_FROM_TYPE:
        case TileType::SPACE_WITH_OBJECT_FROM_TYPE:
        case TileType::GREENERY_WITH_OBJECT_FROM_TYPE:
            if (type_offset > 0 &&
                type_offset < static_cast<int>(sizeof(tertiary_objects_type_data))) {
                obj_type = tertiary_objects_type_data[type_offset];
            }
            break;
        case TileType::SWITCH:
            obj_type = static_cast<uint8_t>(ObjectType::SWITCH);
            break;
        default:
            return;  // Not an object-spawning tile type
    }

    if (obj_type >= static_cast<uint8_t>(ObjectType::COUNT)) return;

    // Compute sub-tile placement from the flip bits (matches &4069-&407e).
    // The 6502 stores (pixels-1)*16 and (rows-1)*8 in its sprite size tables;
    // we reproduce that from the atlas entry's pixel dimensions.
    uint8_t sprite_id = object_types_sprite[obj_type];
    uint8_t width_byte  = 0;
    uint8_t height_byte = 0;
    if (sprite_id <= 0x7c) {
        const SpriteAtlasEntry& e = sprite_atlas[sprite_id];
        width_byte  = static_cast<uint8_t>((e.w > 0 ? (e.w - 1) : 0) * 16);
        height_byte = static_cast<uint8_t>((e.h > 0 ? (e.h - 1) : 0) * 8);
    }
    uint8_t x_frac = (tile_flip & TileFlip::HORIZONTAL)
        ? static_cast<uint8_t>(0u - width_byte)
        : 0;
    uint8_t y_frac = (tile_flip & TileFlip::VERTICAL)
        ? 0
        : static_cast<uint8_t>(0u - height_byte);

    // TILE_SWITCH places both the box (rendered as the tile sprite) and the
    // button (OBJECT_SWITCH, drawn on top). The generic "object sits at the
    // bottom of the tile by sprite height" formula lands the button one
    // atlas row below the box; override with the tile's own y-offset so
    // the two sprites share their top edge. High nibble of
    // tiles_y_offset_and_pattern is in units of 0x10 fraction.
    //
    // Use the RESOLVED tile type (after tertiary redirect), not the
    // landscape tile. For a switch placed via TILE_INVISIBLE_SWITCH
    // redirect, raw_tile_type is 0 (y-offset 0, i.e. top of tile) but the
    // box still renders with the switch's own offset — reading the wrong
    // table row parks the button at the top while the box sits at the
    // bottom. V-flip inverts which end gets the y-offset.
    if (ttype == TileType::SWITCH) {
        uint8_t yhi = tiles_y_offset_and_pattern[tile_type & 0x3f] >> 4;
        // V-flipped switch (ceiling-mounted): box renders at the top of
        // the tile (y_off_atlas = 32 - base - h = 0), so the button
        // belongs at the top too — y_frac = 0, which is already the
        // default for the v-flipped branch above. Only override the
        // non-flipped case, where the generic formula was off by one row.
        if (!(tile_flip & TileFlip::VERTICAL)) {
            y_frac = static_cast<uint8_t>(yhi << 4);
        }
    }

    int slot = object_mgr_.create_object(
        static_cast<ObjectType>(obj_type), 0,
        tile_x, x_frac,
        tile_y, y_frac);

    if (slot >= 0) {
        spawn_created_++;
        Object& obj = object_mgr_.object(slot);
        // Copy tile flip bits to object flags.
        obj.flags = static_cast<uint8_t>(
            (obj.flags & ~(ObjectFlags::FLIP_HORIZONTAL | ObjectFlags::FLIP_VERTICAL)) |
            (tile_flip & (ObjectFlags::FLIP_HORIZONTAL | ObjectFlags::FLIP_VERTICAL)));
        // Remember the tertiary slot so return_to_tertiary can set bit 7
        // again when this object is demoted offscreen (port of &4081-&4083).
        obj.tertiary_slot = static_cast<uint8_t>(data_offset);
        // Copy the tertiary data byte into the primary's data field (&0966
        // objects_data). For doors this is locked/opening/colour flags; for
        // switches it's effect-id + toggle-mask + state; for transporter
        // beams it's stationary + destination. Spawn gate (bit 7) stripped
        // for all paths except switch-redirect, where bit 7 is part of the
        // switch-effects id rather than a spawn flag.
        if (data_offset > 0) {
            uint8_t db = object_mgr_.tertiary_data_byte(data_offset);
            obj.tertiary_data_offset = switch_redirect ? db
                                                        : static_cast<uint8_t>(db & 0x7f);
        } else {
            obj.tertiary_data_offset = 0;
        }

        // Port of &3ed1-&3edf update_metal_door_tile / update_stone_door_tile:
        // after the generic primary-from-tertiary spawn, doors need their
        // orientation, fixed-axis tile coord, and initial open fraction
        // written directly. Horizontal doors (ty=0) slide along X with
        // obj.x pinned to (state + carry); vertical doors (ty=2) along Y.
        //
        // The 6502 computes state as `tile_x - 1` (`SBC #&00` with the
        // carry left CLEAR by create_primary_object_from_tertiary's
        // success path). update_door then reconstructs obj.x as
        // `state + carry_from_(tx + 0x10)`:
        //   closed (tx=0xff): carry=1 → x = tile_x     (landscape door cell)
        //   open   (tx=0x00): carry=0 → x = tile_x - 1 (the recess to the left)
        // Without the -1 the closed door parks one tile too far along the
        // slide axis.
        //
        // Initial tx = 0x00 if DOOR_FLAG_OPENING was set in the data
        // byte, else 0xff (closed).
        if (ttype == TileType::METAL_DOOR || ttype == TileType::STONE_DOOR) {
            obj.ty    = vertical_door ? 0x02 : 0x00;
            obj.state = static_cast<uint8_t>(
                (vertical_door ? tile_y : tile_x) - 1);
            bool opening_initial = (obj.tertiary_data_offset & 0x02) != 0;
            obj.tx = opening_initial ? 0x00 : 0xff;
        }

        // Port of &3fbf..&3fcc update_tile_with_object_from_data: the tile
        // creates the object as its real type (to set up position / default
        // flags) and THEN overwrites the type with PLACEHOLDER (0x49).
        // update_placeholder keeps the object pinned and invisible-ish until
        // the anchor (player or camera centre in map mode) is close enough,
        // then restores the real type stored in tertiary_data. Without this
        // the real object falls off its TILE_SPACE under gravity before the
        // player can see it, and clear_tertiary_spawn (below) has already
        // prevented a respawn — so rolling robots and inactive chatters
        // would never appear.
        if (ttype == TileType::SPACE_WITH_OBJECT_FROM_DATA) {
            obj.type = ObjectType::PLACEHOLDER;
            obj.sprite = object_types_sprite[
                static_cast<uint8_t>(ObjectType::PLACEHOLDER)];
            obj.palette = object_types_palette_and_pickup[
                static_cast<uint8_t>(ObjectType::PLACEHOLDER)] & 0x7f;
        }
    }

    // Mark tertiary as spawned so it isn't created again every frame.
    // Only the canonical (non-redirect) path uses bit 7 as the spawn
    // gate; the redirect paths dedup via the "primary already owns this
    // slot" scan at the top. For INVISIBLE_SWITCH redirects specifically,
    // bit 7 is ALSO part of the packed switch-effects number, so clearing
    // it would corrupt the switch behaviour. Other redirects (e.g. door
    // placed via CHECK_TERTIARY marker with bit 7 already clear) also
    // skip the clear because their dedup is the primary-slot scan.
    if (!redirect) {
        object_mgr_.clear_tertiary_spawn_bit(data_offset);
    }
}
