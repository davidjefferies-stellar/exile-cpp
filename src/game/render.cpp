#include "game/game.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "rendering/debug_names.h"
#include "rendering/sprite_atlas.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/water.h"
#include <cstdio>

void Game::render() {
    renderer_->begin_frame();

    // Apply right-drag pan from the renderer (if any).
    int pan_dx = 0, pan_dy = 0;
    if (renderer_->consume_pan_tiles(pan_dx, pan_dy)) {
        camera_.apply_pan(pan_dx, pan_dy);
    }
    // Re-derive view center (player pos + pan). Clamp to map extents so
    // map-mode panning can't scroll past the 256×256 world and show
    // wrapped-around territory at the edges.
    const Object& player_obj = object_mgr_.player();
    int vp_w_half = renderer_->viewport_width_tiles() / 2;
    int vp_h_half = renderer_->viewport_height_tiles() / 2;
    camera_.follow_player(player_obj.x.whole, player_obj.y.whole,
                          vp_w_half, vp_h_half);

    renderer_->set_viewport(camera_.center_x, camera_.center_y,
                            player_obj.x.fraction, player_obj.y.fraction);

    // Handle left-click to select a tile and build info overlay.
    int click_dx = 0, click_dy = 0;
    if (renderer_->consume_left_click(click_dx, click_dy)) {
        uint8_t tx = static_cast<uint8_t>(camera_.center_x + click_dx);
        uint8_t ty = static_cast<uint8_t>(camera_.center_y + click_dy);
        renderer_->set_highlighted_tile(tx, ty);
        uint8_t raw  = landscape_.get_tile(tx, ty);
        ResolvedTile res = resolve_tile_with_tertiary(landscape_, tx, ty);
        uint8_t tile = res.tile_and_flip;
        uint8_t ttype = tile & 0x3f;
        bool flip_h = (tile & TileFlip::HORIZONTAL) != 0;
        bool flip_v = (tile & TileFlip::VERTICAL)   != 0;
        bool collision_flip_v = flip_v ^ tile_obstruction_v_flip_bit(ttype);
        uint8_t base_y_frac  = static_cast<uint8_t>(
            tiles_y_offset_and_pattern[ttype] & 0xf0);
        uint8_t obs_top_frac = get_tile_y_offset(ttype, flip_v);
        uint8_t pattern_grp  = tiles_y_offset_and_pattern[ttype] & 0x0f;

        // Tertiary diagnostics — useful when a tile "should" spawn an object
        // but doesn't. Tells us whether resolve found an entry, the data-byte
        // offset, its current value (bit 7 = "needs spawning"), and the
        // Chebyshev distance from the anchor to the activation/12-tile gate.
        uint8_t db = (res.data_offset > 0)
            ? object_mgr_.tertiary_data_byte(res.data_offset) : 0;
        int8_t adx = static_cast<int8_t>(tx - object_mgr_.activation_anchor_x());
        int8_t ady = static_cast<int8_t>(ty - object_mgr_.activation_anchor_y());
        int abs_adx = adx < 0 ? -adx : adx;
        int abs_ady = ady < 0 ? -ady : ady;

        char buf[640];
        std::snprintf(buf, sizeof(buf),
                      "Tile (%u,%u)\n%s (0x%02x)\n"
                      "raw=0x%02x resolved=0x%02x\n"
                      "flip: %s%s%s\n"
                      "collision: %s\n"
                      "sprite offset y=0x%02x\n"
                      "obstruction top=0x%02x pattern=%u\n"
                      "tert idx=%d data_off=0x%02x\n"
                      "data byte=0x%02x bit7=%s\n"
                      "anchor dist %d,%d (gate=12)",
                      tx, ty, tile_type_name(ttype), ttype,
                      raw, tile,
                      flip_h ? "H " : "",
                      flip_v ? "V " : "",
                      (!flip_h && !flip_v) ? "none" : "",
                      collision_flip_v ? "top-solid (ceiling)"
                                       : "bottom-solid (ground)",
                      base_y_frac,
                      obs_top_frac, pattern_grp,
                      res.tertiary_index, res.data_offset,
                      db, (db & 0x80) ? "SET" : "clear",
                      abs_adx, abs_ady);
        std::string text(buf);

        int object_count = 0;
        for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& o = object_mgr_.object(i);
            if (!o.is_active()) continue;
            if (o.x.whole != tx || o.y.whole != ty) continue;
            if (object_count == 0) text += "\nObjects:";
            text += "\n  ";
            text += object_type_name(o.type);
            object_count++;
            if (object_count >= 6) { text += "\n  ..."; break; }
        }
        selected_tile_info_ = text;
    }

    // Compose the top-right overlay: the tile-click info (if any), with a
    // "[MAP MODE]" banner + live tier contents prepended when the anchor is
    // tracking the camera. Listing the actual object-type names (not just
    // counts) makes it obvious whether the right things are spawning as you
    // scroll around.
    std::string overlay;
    if (paused_) overlay = "[PAUSED]\n";
    if (activation_from_camera_) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "[MAP MODE]\n"
                      "anchor %u,%u\n"
                      "try %u made %u\n"
                      "clr: ret%u rem%u dem%u\n"
                      "switch presses: %u\n",
                      object_mgr_.activation_anchor_x(),
                      object_mgr_.activation_anchor_y(),
                      spawn_attempts_, spawn_created_,
                      object_mgr_.debug_returns_,
                      object_mgr_.debug_removes_,
                      object_mgr_.debug_demotes_,
                      object_mgr_.debug_switch_presses_);
        overlay = buf;

        // Primary tier — full names. Slot 0 is the player; skip it since it's
        // always present.
        overlay += "PRIM:\n";
        int primary_count = 0;
        for (int i = 1; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& obj = object_mgr_.object(i);
            if (!obj.is_active()) continue;
            overlay += "  ";
            overlay += object_type_name(obj.type);
            // Extra state for switches/doors so we can tell at a glance
            // whether a press actually landed. SWITCH (0x42) shows data
            // byte + leading-edge register + touching slot; doors show
            // locked/opening/moving bits.
            char line[96];
            if (obj.type == ObjectType::SWITCH) {
                std::snprintf(line, sizeof(line),
                              " d=%02x tx=%02x fh=%d touch=%02x",
                              obj.tertiary_data_offset, obj.tx,
                              (obj.flags & ObjectFlags::FLIP_HORIZONTAL) ? 1 : 0,
                              obj.touching);
                overlay += line;
            } else if (obj.type == ObjectType::HORIZONTAL_METAL_DOOR ||
                       obj.type == ObjectType::VERTICAL_METAL_DOOR ||
                       obj.type == ObjectType::HORIZONTAL_STONE_DOOR ||
                       obj.type == ObjectType::VERTICAL_STONE_DOOR) {
                uint8_t d = obj.tertiary_data_offset;
                std::snprintf(line, sizeof(line),
                              " d=%02x %s%s%s",
                              d,
                              (d & 0x01) ? "LCK " : "    ",
                              (d & 0x02) ? "OPN " : "    ",
                              (d & 0x04) ? "MOV"  : "   ");
                overlay += line;
            }
            overlay += "\n";
            primary_count++;
        }
        if (primary_count == 0) overlay += "  (empty)\n";

        // Secondary tier.
        overlay += "SEC:\n";
        int secondary_count = 0;
        for (int i = 0; i < GameConstants::SECONDARY_OBJECT_SLOTS; i++) {
            const SecondaryObject& sec = object_mgr_.secondary(i);
            if (sec.y == 0) continue;
            overlay += "  ";
            if (sec.type < static_cast<uint8_t>(ObjectType::COUNT)) {
                overlay += object_type_name(static_cast<ObjectType>(sec.type));
            } else {
                overlay += "UNKNOWN";
            }
            overlay += "\n";
            secondary_count++;
        }
        if (secondary_count == 0) overlay += "  (empty)\n";
    }
    overlay += selected_tile_info_;
    renderer_->set_overlay_text(overlay.c_str());

    int vp_w = renderer_->viewport_width_tiles();
    int vp_h = renderer_->viewport_height_tiles();

    // Render visible tiles
    uint8_t start_x = camera_.center_x - static_cast<uint8_t>(vp_w / 2);
    uint8_t start_y = camera_.center_y - static_cast<uint8_t>(vp_h / 2);

    // Water backdrop: emulates the 6502 raster palette swap that turns
    // background colour 0 from black to blue below the waterline and cyan
    // on the waterline itself (&12a6-&12d8). Must run before the tile
    // blits so colour-0 transparent pixels reveal the water colour.
    for (int dx = 0; dx < vp_w; dx++) {
        uint8_t wx = static_cast<uint8_t>(start_x + dx);
        uint8_t wy = Water::get_waterline_y(wx);
        renderer_->render_water_column(wx, wy);
    }

    for (int dy = 0; dy < vp_h; dy++) {
        uint8_t wy = static_cast<uint8_t>(start_y + dy);
        for (int dx = 0; dx < vp_w; dx++) {
            uint8_t wx = static_cast<uint8_t>(start_x + dx);
            ResolvedTile res = resolve_tile_with_tertiary(landscape_, wx, wy);
            uint8_t tile      = res.tile_and_flip;
            uint8_t tile_type = tile & TileFlip::TYPE_MASK;
            uint8_t tile_flip = tile & TileFlip::MASK;

            // For tiles that spawn objects from tertiary data, create the
            // primary object on first render (the original does this
            // during tile plotting). The door tile additionally swaps in
            // an equivalent wall tile for the underlying geometry.
            if (res.tertiary_index >= 0) {
                spawn_tertiary_object(tile_type, tile_flip,
                                      wx, wy,
                                      res.data_offset, res.type_offset,
                                      res.raw_tile_type);
            }

            // Door tiles (METAL_DOOR &03, STONE_DOOR &04) are declared
            // SPRITE_NONE in the tile-sprite table (&04ae / &04af). The
            // 6502's door_tiles_table (0x17/0x2a/0x19) is only consulted
            // during obstruction checks at &3ebd-&3ec2; plotting skips
            // that substitution (&3eb9 BMI skip_setting_tile when
            // TILE_PROCESSING_FLAG_PLOTTING is set). Leave tile_type as
            // the door so the renderer's SPRITE_NONE path draws nothing
            // behind the door object.

            // resolve_tile_palette may mutate tile_type (TILE_POSSIBLE_LEAF
            // can resolve to TILE_SPACE) and tile_flip (leaf vertical flip
            // toggle), so compute the palette first and THEN populate the
            // render info with the post-mutation values.
            uint8_t palette = resolve_tile_palette(tile_type, wx, wy, tile_flip);

            TileRenderInfo info;
            info.tile_type = tile_type;
            info.flip_h = (tile_flip & TileFlip::HORIZONTAL) != 0;
            info.flip_v = (tile_flip & TileFlip::VERTICAL) != 0;
            info.palette = palette;

            renderer_->render_tile(wx, wy, info);
        }
    }

    // Render active objects
    for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
        const Object& obj = object_mgr_.object(i);
        if (!obj.is_active()) continue;

        SpriteRenderInfo info;
        info.sprite_id = obj.sprite;
        info.palette = obj.palette;
        info.flip_h = obj.is_flipped_h();
        info.flip_v = obj.is_flipped_v();
        info.visible = true;
        info.type = obj.type;

        renderer_->render_object(obj.x, obj.y, info);
    }

    // Debug AABB overlay — pixel-precise bounding boxes used by object-object
    // collision (see sprite_width_units / sprite_height_units in collision.cpp:
    // width = (pe.w-1)*16, height = (pe.h-1)*8, in 1/256-of-a-tile units).
    // Toggle with 'B'. Player is drawn in cyan; weight-7 statics (doors,
    // switches) in red so blocking boxes stand out; everything else yellow.
    if (renderer_->aabb_overlay_enabled()) {
        for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& obj = object_mgr_.object(i);
            if (!obj.is_active()) continue;
            if (obj.sprite > 0x7c) continue;
            const SpriteAtlasEntry& e = sprite_atlas[obj.sprite];
            int w_u = (e.w > 0 ? e.w - 1 : 0) * 16;
            int h_u = (e.h > 0 ? e.h - 1 : 0) * 8;
            uint32_t col = (i == 0) ? 0x33CCFF : 0xFFDD33;
            uint8_t idx = static_cast<uint8_t>(obj.type);
            if (idx < static_cast<uint8_t>(ObjectType::COUNT)) {
                uint8_t tflags = object_types_flags[idx];
                if ((tflags & ObjectTypeFlags::WEIGHT_MASK) >= 7 &&
                    !(tflags & ObjectTypeFlags::INTANGIBLE)) {
                    col = 0xFF3333;
                }
            }
            renderer_->render_aabb(obj.x, obj.y, w_u, h_u, col);
        }
    }

    // Render particles (on top of tiles/objects, below HUD).
    for (int i = 0; i < particles_.count(); i++) {
        const Particle& p = particles_.get(i);
        renderer_->render_particle(p.x, p.x_fraction,
                                   p.y, p.y_fraction,
                                   p.colour_and_flags & 0x07);
    }

    // Debug: primary / secondary / tertiary tier overlay. The renderer
    // gates drawing on its own toggle (key 'T'); we can just unconditionally
    // enumerate here and let calls be no-ops when the overlay is off.
    {
        // Primary — green. Slot 0 is always the player; other slots label
        // with their object-type name so the tier overlay is self-explanatory.
        for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& obj = object_mgr_.object(i);
            if (!obj.is_active()) continue;
            const char* label = object_type_name(obj.type);
            renderer_->render_debug_marker(obj.x.whole, obj.y.whole,
                                           0x22DD22, label);
        }
        // Secondary — yellow. y == 0 means "empty slot". sec.type is a
        // raw uint8_t so we cast into ObjectType for the name lookup.
        for (int i = 0; i < GameConstants::SECONDARY_OBJECT_SLOTS; i++) {
            const SecondaryObject& sec = object_mgr_.secondary(i);
            if (sec.y == 0) continue;
            const char* label =
                (sec.type < static_cast<uint8_t>(ObjectType::COUNT))
                    ? object_type_name(static_cast<ObjectType>(sec.type))
                    : "UNKNOWN";
            renderer_->render_debug_marker(sec.x, sec.y, 0xEECC22, label);
        }
        // Tertiary — red. Only draw for tiles currently in the viewport.
        // Label shows the spawned OBJECT's name for tile types that spawn
        // (&1715 -> &4042 path), otherwise the resolved tile-type's name.
        for (int dy = 0; dy < vp_h; dy++) {
            uint8_t wy = static_cast<uint8_t>(start_y + dy);
            for (int dx = 0; dx < vp_w; dx++) {
                uint8_t wx = static_cast<uint8_t>(start_x + dx);
                ResolvedTile res = resolve_tile_with_tertiary(landscape_, wx, wy);
                if (res.tertiary_index < 0) continue;

                uint8_t ttype = res.tile_and_flip & TileFlip::TYPE_MASK;
                uint8_t tflip = res.tile_and_flip & TileFlip::MASK;
                uint8_t obj_type = 0xff;
                switch (ttype) {
                    case 0x01: obj_type = 0x41; break; // TRANSPORTER_BEAM
                    case 0x02:                         // FROM_DATA
                        obj_type = object_mgr_.tertiary_data_byte(res.data_offset) & 0x7f;
                        break;
                    case 0x03: {                       // METAL_DOOR
                        bool vert = (tflip == TileFlip::HORIZONTAL) ||
                                    (tflip == TileFlip::VERTICAL);
                        obj_type = static_cast<uint8_t>(0x3c + (vert ? 1 : 0));
                        break;
                    }
                    case 0x04: {                       // STONE_DOOR
                        bool vert = (tflip == TileFlip::HORIZONTAL) ||
                                    (tflip == TileFlip::VERTICAL);
                        obj_type = static_cast<uint8_t>(0x3e + (vert ? 1 : 0));
                        break;
                    }
                    case 0x05:                         // STONE_HALF_WITH_OBJECT_FROM_TYPE
                    case 0x06:                         // SPACE_WITH_OBJECT_FROM_TYPE
                    case 0x07:                         // GREENERY_WITH_OBJECT_FROM_TYPE
                        if (res.type_offset > 0 &&
                            res.type_offset < static_cast<int>(sizeof(tertiary_objects_type_data))) {
                            obj_type = tertiary_objects_type_data[res.type_offset];
                        }
                        break;
                    case 0x08: obj_type = 0x42; break; // SWITCH
                }

                const char* label;
                if (obj_type < static_cast<uint8_t>(ObjectType::COUNT)) {
                    label = object_type_name(static_cast<ObjectType>(obj_type));
                } else {
                    label = tile_type_name(ttype);
                }
                renderer_->render_debug_marker(wx, wy, 0xDD3333, label);
            }
        }
    }

    // Draw the activation-distance rings (1, 4, 12 tile boxes) around the
    // anchor. The renderer gates on its own tile-grid toggle ('G'), so
    // turning on the grid also turns on this visualisation.
    renderer_->render_activation_overlay(
        object_mgr_.activation_anchor_x(),
        object_mgr_.activation_anchor_y());

    // Render HUD
    PlayerState ps;
    ps.energy = object_mgr_.player().energy;
    ps.weapon = player_weapon_;
    ps.keys_collected = 0;
    ps.has_jetpack_booster = false;
    for (int i = 0; i < 5; i++) ps.pockets[i] = pockets_[i];
    ps.pockets_used = pockets_used_;
    renderer_->render_hud(ps);

    renderer_->end_frame();
}
