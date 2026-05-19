#include "game/game.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "rendering/sprite_atlas.h"
#include "world/tile_data.h"

// Port of &4042 create_primary_object_from_tertiary + the type-select
// from the per-tile update routines (&3e98 / &3e95 / &3fcd / &3fb7 /
// &3fbf / &3ee3). Picks type, creates primary, applies flip-aware
// sub-tile offset, copies flip flags, clears bit 7 to prevent respawn.
void Game::spawn_tertiary_object(uint8_t tile_type, uint8_t tile_flip,
                                 uint8_t tile_x, uint8_t tile_y,
                                 int data_offset, int type_offset,
                                 uint8_t raw_tile_type) {
    // TILE_INVISIBLE_SWITCH (&3ef2) runs from collision only; no
    // primary ever spawns. Bail early so render-loop diagnostics
    // don't count visible invisible-switch tiles.
    if (tile_is(tile_type, TileType::INVISIBLE_SWITCH)) return;

    // Switch-redirect: raw landscape tile is INVISIBLE_SWITCH (range 0)
    // AND the tertiary resolves to a visible SWITCH or door graphic.
    // Data byte = switch-effects number (bit 7 is the effect MSB, NOT a
    // spawn flag). Distinct from range-0 -> turret/nest where bit 7 is
    // still the standard spawn flag. See CLAUDE.md known-mechanisms.
    bool is_door = (tile_type == static_cast<uint8_t>(TileType::METAL_DOOR) ||
                    tile_type == static_cast<uint8_t>(TileType::STONE_DOOR));
    bool switch_redirect =
        tile_is(raw_tile_type, TileType::INVISIBLE_SWITCH) &&
        (tile_is(tile_type, TileType::SWITCH) || is_door);

    // Bit-7 spawn gate — &4050 in create_primary_object_from_tertiary.
    // Switch-redirects bypass (data byte has no spawn flag). Doors
    // honour the gate like everything else: ROM-default bit 7 set on
    // first render -> spawn -> clear bit 7 (&408a). Destroyed door's
    // write-back keeps bit 7 clear -> stays gone. return_to_tertiary
    // re-applies bit 7 on demote so re-entry re-spawns the door.
    if (!switch_redirect && data_offset != 0 &&
        !(object_mgr_.tertiary_data_byte(data_offset) & 0x80)) {
        return;
    }

    // Dedup scan for switch-redirects only — their data byte has no
    // spawn flag so without this they'd re-spawn every tile render.
    if (switch_redirect && data_offset > 0) {
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& p = object_mgr_.object(i);
            if (p.is_active() &&
                p.tertiary_slot == static_cast<uint16_t>(data_offset)) {
                return;
            }
        }
    }

    // Port-only gate: BBC viewport was ~8 tiles so spawning piggybacked
    // safely on tile plotting. Our wider viewport (up to ~40 tiles) lets
    // render spawn primaries beyond check_demotion's keep radius (4 for
    // stationary "slow+supported" types), causing per-frame churn.
    {
        int8_t dx = static_cast<int8_t>(tile_x - object_mgr_.activation_anchor_x());
        int8_t dy = static_cast<int8_t>(tile_y - object_mgr_.activation_anchor_y());
        uint8_t adx = static_cast<uint8_t>(dx < 0 ? -dx : dx);
        uint8_t ady = static_cast<uint8_t>(dy < 0 ? -dy : dy);
        // Settable via exile.ini [distances] spawn_tertiary. See Game::init.
        if (adx > spawn_tertiary_distance_ ||
            ady > spawn_tertiary_distance_) return;
    }

    // Count only attempts past both gates — earlier counting ticks up
    // constantly from rejected tiles and is useless when paused.
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
            // After Option B, the per-cell entry carries its own
            // type byte (copied from the static tertiary_objects_type_
            // data at bake) — read it directly.
            obj_type = object_mgr_.tertiary_type_byte(type_offset);
            break;
        case TileType::SWITCH:
            obj_type = static_cast<uint8_t>(ObjectType::SWITCH);
            break;
        case TileType::NEST:
        case TileType::PIPE:
            // Port of &3e1b/&3e34 update_nest_or_pipe_tile — spawn
            // BUSH primary on a NEST/PIPE cell with bit-7-armed tertiary.
            obj_type = static_cast<uint8_t>(ObjectType::BUSH);
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
    if (sprite_id <= 0x80) {
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

    // TILE_SWITCH: button (OBJECT_SWITCH) needs the tile's own y-offset
    // (high nibble of tiles_y_offset_and_pattern, 0x10 fraction units)
    // to share top-edge with the box sprite. Use RESOLVED tile_type so
    // INVISIBLE_SWITCH redirects don't read the wrong table row.
    if (ttype == TileType::SWITCH) {
        uint8_t yhi = tiles_y_offset_and_pattern[tile_type & 0x3f] >> 4;
        // V-flipped switch: box renders at tile top (y_off = 0); the
        // v-flipped branch default already matches, so only override
        // the non-flipped case.
        if (!(tile_flip & TileFlip::VERTICAL)) {
            y_frac = static_cast<uint8_t>(yhi << 4);
        }
        // Sub-pixel nudge to align the button on the left-facing tile
        // baseline. Right-facing (bit 0 of the data byte set, sprite
        // h-flipped by update_switch) needs no offset — the 6502
        // doesn't apply one and our previous -16-for-both was leaving
        // the button slightly offset from where it should sit.
        bool facing_right = data_offset > 0 &&
            (object_mgr_.tertiary_data_byte(data_offset) & 0x01);
        if (!facing_right) {
            x_frac = static_cast<uint8_t>(x_frac);
        }
    }

    // &3e39-&3e3e nest/pipe BUSH override both fractions to 0x40 after
    // create_primary_object_from_tertiary returns, regardless of flip.
    if (ttype == TileType::NEST || ttype == TileType::PIPE) {
        x_frac = 0x40;
        y_frac = 0x40;
    }

    // &3ee8-&3eed update_transporter_tile: x_frac=0x40, y_frac=0x80
    // centre the beam within its tile. The generic flip-aware default
    // (x_frac=0 unflipped) puts the beam at the left edge.
    if (ttype == TileType::TRANSPORTER) {
        x_frac = 0x40;
        y_frac = 0x80;
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
        // Remember tertiary slot for return_to_tertiary re-arm
        // (&4081-&4083). uint16_t — indices exceed 255 post-bake; a
        // uint8_t truncation breaks dedup and demote re-arm.
        obj.tertiary_slot = static_cast<uint16_t>(data_offset);
        // Copy tertiary data byte into objects_data (&0966) — door
        // flags / switch effect-id / transporter destination / turret
        // projectile type. Strip bit 7 EXCEPT for switch-redirects:
        // there bit 7 is the MSB of the switch-effects number, not a
        // spawn flag. For raw-INVISIBLE_SWITCH->turret redirects (range
        // 0 entries that aren't switches) bit 7 IS the spawn flag and
        // must be stripped — leaving it set makes update_turret's
        // `data >> 1` shift into the 0x40..0x7f range.
        if (data_offset > 0) {
            uint8_t db = object_mgr_.tertiary_data_byte(data_offset);
            obj.tertiary_data_offset = switch_redirect
                ? db : static_cast<uint8_t>(db & 0x7f);
        } else {
            obj.tertiary_data_offset = 0;
        }

        // Port of &3ed1-&3edf update_metal/stone_door_tile:
        //   &3ed1 ASL A
        //   &3ed2 STA &0936,Y ; objects_ty (0 horiz, 2 vert)
        //   &3ed5 TAX
        //   &3ed6 LDA &95,X ; tile_x   (X=0 → tile_x, X=2 → tile_y)
        //   &3ed8 SBC #&00            ; carry CLEAR → state = tile - 1
        //   &3eda STA &0976,Y ; objects_state
        //   &3edd LDA &a3 ; door_open_fraction
        //   &3edf STA &0916,Y ; objects_tx
        // update_door later rebuilds obj.x as state + carry_from_(tx +
        // 0x10) — closed tx=0xff gives carry=1 → x = tile, open tx=0x00
        // → x = tile - 1.
        if (ttype == TileType::METAL_DOOR || ttype == TileType::STONE_DOOR) {
            obj.ty    = vertical_door ? 0x02 : 0x00;
            obj.state = static_cast<uint8_t>(
                (vertical_door ? tile_y : tile_x) - 1);
            bool opening_initial = (obj.tertiary_data_offset & 0x02) != 0;
            obj.tx = opening_initial ? 0x00 : 0xff;
        }

        // Port of &3fbf-&3fcc update_tile_with_object_from_data:
        //   &3fbf LDA &0986,Y ; tertiary_objects_data
        //   &3fc2 AND #&7f             ; strip "needs creating" bit
        //   &3fc4 JSR &4042 create_primary_object_from_tertiary
        //   &3fc7 LDA #&49 ; OBJECT_PLACEHOLDER
        //   &3fc9 STA &0860,Y ; objects_type
        //   &3fcc RTS
        // Spawn as real type for position/flags, then overwrite type
        // with PLACEHOLDER so update_placeholder pins until the anchor
        // is close enough to restore the real type.
        if (ttype == TileType::SPACE_WITH_OBJECT_FROM_DATA) {
            obj.type = ObjectType::PLACEHOLDER;
            obj.sprite = object_types_sprite[
                static_cast<uint8_t>(ObjectType::PLACEHOLDER)];
            obj.palette = object_types_palette_and_pickup[
                static_cast<uint8_t>(ObjectType::PLACEHOLDER)] & 0x7f;
        }
    }

    // Mark spawned by clearing bit 7 in the tertiary table entry
    // (&408a in 6502 create_primary_object_from_tertiary). Skip for
    // switch-redirects — bit 7 there is part of the effect-id, and
    // dedup is handled via the scan above.
    if (!switch_redirect) {
        object_mgr_.clear_tertiary_spawn_bit(data_offset);
    }
}
