#include "game/game.h"
#include "behaviours/environment.h"
#include "behaviours/mood.h"
#include "objects/collision.h"
#include "objects/object_data.h"
#include "objects/object_tables.h"
#include "rendering/debug_names.h"
#include "rendering/sprite_atlas.h"
#include "world/tertiary.h"
#include "world/tile_data.h"
#include "world/water.h"
#include <cstdio>

// Per-object regen floor for Health overlay. Mirrors enforce_minimum_
// energy across behaviour TUs (frogmen: shared 0x7f wins over per-variant).
// Doors: &4cbe-&4cd5 regen to 0xff above threshold, so we display the
// actionable break threshold instead. 0 = no regen mechanism, label hidden.
static constexpr uint8_t kDoorBreakThreshold[4] = { 0x80, 0x74, 0xc0, 0x80 };

static uint8_t energy_floor_for_obj(const Object& obj) {
    switch (obj.type) {
        case ObjectType::CREW_MEMBER:           return 0x3f;
        case ObjectType::FLUFFY:                return 0x29;
        case ObjectType::SMALL_HIVE:
        case ObjectType::LARGE_HIVE:            return 0x46;
        case ObjectType::RED_FROGMAN:
        case ObjectType::GREEN_FROGMAN:
        case ObjectType::INVISIBLE_FROGMAN:     return 0x7f;
        case ObjectType::RED_SLIME:             return 0x7f;
        case ObjectType::GREEN_SLIME:           return 0x3f;
        case ObjectType::DENSE_NEST:            return 0x7f;
        case ObjectType::SUCKING_NEST:          return 0x7f;
        case ObjectType::BIG_FISH:              return 0x19;
        case ObjectType::RED_MAGENTA_IMP:       return 0x0a;
        case ObjectType::RED_YELLOW_IMP:        return 0x50;
        case ObjectType::BLUE_CYAN_IMP:         return 0x46;
        case ObjectType::CYAN_YELLOW_IMP:       return 0x14;
        case ObjectType::RED_CYAN_IMP:          return 0x13;
        case ObjectType::RED_MAGENTA_BIRD:      return 0x1e;
        case ObjectType::TRIAX:                 return 0xfd;
        case ObjectType::SWITCH:                return 0x1e;
        case ObjectType::GREEN_WHITE_TURRET:    return 0x14;
        case ObjectType::CYAN_RED_TURRET:       return 0x7f;
        case ObjectType::MAGENTA_ROLLING_ROBOT: return 0x14;
        case ObjectType::RED_ROLLING_ROBOT:     return 0x46;
        case ObjectType::BLUE_ROLLING_ROBOT:    return 0x46;
        case ObjectType::HOVERING_ROBOT:        return 0x14;
        case ObjectType::MAGENTA_CLAWED_ROBOT:  return 0x46;
        case ObjectType::CYAN_CLAWED_ROBOT:     return 0x5a;
        case ObjectType::GREEN_CLAWED_ROBOT:    return 0x80;
        case ObjectType::RED_CLAWED_ROBOT:      return 0x82;
        case ObjectType::HORIZONTAL_METAL_DOOR:
        case ObjectType::VERTICAL_METAL_DOOR:
        case ObjectType::HORIZONTAL_STONE_DOOR:
        case ObjectType::VERTICAL_STONE_DOOR: {
            // Door colour-pair lives in bits 5-4 of tertiary_data_offset
            // (the data byte). Same shift the port uses in update_door.
            uint8_t pair = (obj.tertiary_data_offset >> 4) & 0x03;
            return kDoorBreakThreshold[pair];
        }
        default:                                return 0;
    }
}

// Find live primary by tertiary_slot for Wiring overlay (animating doors
// wire from current pos, not home tile). slot must be uint16_t — entries
// at idx > 255 (redirect switches at 521 / 681) silently truncated under
// uint8_t and produced duplicate wires.
static bool find_primary_at_slot(const ObjectManager& mgr, uint16_t slot,
                                 uint8_t& out_x, uint8_t& out_y) {
    for (int j = 1; j < GameConstants::PRIMARY_OBJECT_SLOTS; j++) {
        const Object& dst = mgr.object(j);
        if (!dst.is_active()) continue;
        if (dst.tertiary_slot != slot) continue;
        out_x = dst.x.whole;
        out_y = dst.y.whole;
        return true;
    }
    return false;
}

void Game::render() {
    renderer_->begin_frame();

    // Sprite-viewer mode: replace the entire world render with the atlas
    // overlay (palette grid + ROM sheet + selected-sprite detail). The
    // checkbox sits in the same HUD strip as the others, so we still
    // need to draw the HUD so the user can toggle out. The renderer's
    // process_mouse already absorbs viewer clicks before they fall
    // through as world clicks.
    if (renderer_->sprite_viewer_enabled()) {
        renderer_->render_sprite_viewer();
        PlayerState ps_empty;
        renderer_->render_hud(ps_empty);
        renderer_->end_frame();
        return;
    }

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

    // Earthquake test-event shake (port-only). Perturb the fractional
    // viewport centre with each call; when test_shake_frames_ hits 0
    // the perturbation stops and the camera returns to player.
    uint8_t vp_fx = player_obj.x.fraction;
    uint8_t vp_fy = player_obj.y.fraction;
    if (test_shake_frames_ > 0) {
        int8_t dx = static_cast<int8_t>((rng_.next() & 0x7f) - 0x40);
        int8_t dy = static_cast<int8_t>((rng_.next() & 0x7f) - 0x40);
        vp_fx = static_cast<uint8_t>(vp_fx + dx);
        vp_fy = static_cast<uint8_t>(vp_fy + dy);
        test_shake_frames_--;
    }
    renderer_->set_viewport(camera_.center_x, camera_.center_y, vp_fx, vp_fy);

    // Mirror the current paint tile to the renderer so the editor
    // palette panel can highlight the selected cell, and pull any
    // palette click that the renderer captured this frame. The palette
    // hit-test runs before world clicks in pixel_renderer's mouse
    // handler, so a palette click never reaches the SELECT/PAINT
    // branch below.
    renderer_->set_paint_tile(editor_paint_tile_);
    renderer_->set_paint_object(editor_paint_kind_ == 1
                                    ? editor_paint_object_idx_ : -1);
    {
        uint8_t picked = 0;
        if (renderer_->consume_palette_click(picked)) {
            // Replace the type bits but preserve the current flip flags
            // — the user toggles those independently via the FlipX /
            // FlipY buttons. Switching to a tile also switches paint-
            // kind back to TILE so right-click paints tiles.
            editor_paint_tile_ = static_cast<uint8_t>(
                (editor_paint_tile_ & 0xc0) | (picked & 0x3f));
            editor_paint_kind_ = 0;
        }
        int obj_picked = -1;
        if (renderer_->consume_object_palette_click(obj_picked)) {
            editor_paint_object_idx_ = obj_picked;
            editor_paint_kind_ = 1;
        }
        int flip_which = -1;
        if (renderer_->consume_palette_flip_click(flip_which)) {
            // FlipX = bit 7 (0x80), FlipY = bit 6 (0x40).
            editor_paint_tile_ ^= (flip_which == 0 ? 0x80 : 0x40);
        }
        // Detach: clear the highlighted cell's tertiary entry index.
        // The entry itself stays in the entry pool (no GC pass yet);
        // this just stops the cell from referencing it.
        if (renderer_->consume_palette_detach_click() &&
            editor_has_highlight_) {
            landscape_.set_tertiary_index_at(
                editor_highlight_x_, editor_highlight_y_,
                Landscape::NO_TERTIARY);
        }
    }

    // Two-button editor model:
    //   * LEFT-click  -> SELECT. Highlights a cell, captures it as the
    //                   paint source, refreshes the info overlay. Works
    //                   whether or not Edit is on.
    //   * RIGHT-click -> PAINT. When Edit is on, stamps the current paint
    //                   tile at the clicked cell. Distinguished from
    //                   right-drag (camera pan) by a small motion
    //                   threshold inside pixel_renderer.
    int click_dx = 0, click_dy = 0;
    if (renderer_->consume_left_click(click_dx, click_dy)) {
        uint8_t tx = static_cast<uint8_t>(camera_.center_x + click_dx);
        uint8_t ty = static_cast<uint8_t>(camera_.center_y + click_dy);
        renderer_->set_highlighted_tile(tx, ty);
        editor_highlight_x_   = tx;
        editor_highlight_y_   = ty;
        editor_has_highlight_ = true;
        editor_paint_tile_    = landscape_.get_tile(tx, ty);
        refresh_selected_tile_info(tx, ty);
    }
    int rclick_dx = 0, rclick_dy = 0;
    if (renderer_->editor_enabled() &&
        renderer_->consume_right_click(rclick_dx, rclick_dy)) {
        uint8_t tx = static_cast<uint8_t>(camera_.center_x + rclick_dx);
        uint8_t ty = static_cast<uint8_t>(camera_.center_y + rclick_dy);

        if (editor_paint_kind_ == 1 && editor_paint_object_idx_ >= 0) {
            // Object placement: stamp SPACE_WITH_OBJECT_FROM_TYPE marker,
            // overwrite the baked tertiary's tile_and_flip/data/type so the
            // chosen object spawns (data bit 7 = needs-spawn). Inherit
            // FlipX/FlipY from editor state — &4062-&4079 propagates them.
            uint8_t flip_bits = editor_paint_tile_ & 0xc0;
            uint8_t marker_with_flip = static_cast<uint8_t>(
                static_cast<uint8_t>(TileType::SPACE_WITH_OBJECT_FROM_TYPE) |
                flip_bits);
            landscape_.set_tile(tx, ty, marker_with_flip);
            uint16_t idx = landscape_.tertiary_index_at(tx, ty);
            if (idx == Landscape::NO_TERTIARY) {
                // No source entry matched this column for marker type;
                // allocate a fresh entry on demand and point the cell
                // at it.
                TertiaryEntry e;
                e.tile_and_flip = marker_with_flip;
                e.data = 0x80;
                e.type = renderer_->object_palette_type(editor_paint_object_idx_);
                int new_idx = landscape_.add_tertiary_entry(e);
                if (new_idx >= 0) {
                    landscape_.set_tertiary_index_at(
                        tx, ty, static_cast<uint16_t>(new_idx));
                }
            } else {
                TertiaryEntry& e = landscape_.tertiary_entry_mut(idx);
                e.tile_and_flip = marker_with_flip;
                e.data = 0x80;
                e.type = renderer_->object_palette_type(editor_paint_object_idx_);
            }
        } else {
            // Tile-paint mode (default).
            landscape_.set_tile(tx, ty, editor_paint_tile_);
        }

        // Move the highlight to the painted cell so the info overlay
        // and editor controls act on it.
        renderer_->set_highlighted_tile(tx, ty);
        editor_highlight_x_   = tx;
        editor_highlight_y_   = ty;
        editor_has_highlight_ = true;
        refresh_selected_tile_info(tx, ty);
    }

    // Live refresh of the info overlay each frame an editor highlight
    // exists, so [/] data-byte bumps and Detach clicks show up
    // immediately rather than waiting for the next click.
    if (editor_has_highlight_ && renderer_->editor_enabled()) {
        refresh_selected_tile_info(editor_highlight_x_,
                                   editor_highlight_y_);
    }

    // Compose the top-right overlay: the tile-click info (if any), with a
    // "[MAP MODE]" banner + live tier contents prepended when the anchor is
    // tracking the camera. Listing the actual object-type names (not just
    // counts) makes it obvious whether the right things are spawning as you
    // scroll around.
    std::string overlay;
    if (paused_) overlay = "[PAUSED]\n";
    // Editor save-feedback banner: "Saved exile.map" / "Save FAILED" for
    // ~2-4 seconds after pressing '\'.
    if (frame_counter_ < editor_save_msg_until_frame_) {
        overlay += editor_save_msg_ok_
            ? "[Saved exile.map]\n"
            : "[Save FAILED]\n";
    }
    // Editor mode indicator + current paint tile (so the user knows what
    // they're about to stamp). Only shown when Edit is on.
    if (renderer_->editor_enabled()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "[EDIT] paint=0x%02x\n",
                      editor_paint_tile_);
        overlay += buf;
    }
    if (activation_from_camera_) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "[MAP MODE]\n"
                      "anchor %u,%u\n"
                      "try %u made %u\n"
                      "clr: ret%u rem%u dem%u\n"
                      "add: cre%u prm%u\n"
                      "switch presses: %u\n",
                      object_mgr_.activation_anchor_x(),
                      object_mgr_.activation_anchor_y(),
                      spawn_attempts_, spawn_created_,
                      object_mgr_.debug_returns_,
                      object_mgr_.debug_removes_,
                      object_mgr_.debug_demotes_,
                      object_mgr_.debug_creates_,
                      object_mgr_.debug_promotes_,
                      object_mgr_.debug_switch_presses_);
        overlay = buf;

        // Event details (cre/prm/dem/ret/rem per-frame event list) go to
        // exile-debug.log instead of the HUD so they can be grepped.
        // The HUD keeps the aggregate counters above for at-a-glance
        // "something's churning" detection.

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
                              " d=%02x %s%s%s tx=%02x tc=%02x",
                              d,
                              (d & 0x01) ? "LCK " : "    ",
                              (d & 0x02) ? "OPN " : "    ",
                              (d & 0x04) ? "MOV"  : "   ",
                              obj.tx, obj.touching);
                overlay += line;
            }
            overlay += "\n";
            primary_count++;
        }
        if (primary_count == 0) overlay += "  (empty)\n";

        // Secondary tier. Each entry shows position and signed tile offset
        // from the activation anchor, with a leading '*' for entries that
        // fall inside the promote radius (those will be promoted to primary
        // on the next promote_selective tick). Without the offset, a
        // distant ROM-seeded PIANO is visually indistinguishable from a
        // just-added runtime entry.
        overlay += "SEC:\n";
        uint8_t ax = object_mgr_.activation_anchor_x();
        uint8_t ay = object_mgr_.activation_anchor_y();
        uint8_t prom = object_mgr_.promote_distance();
        int secondary_count = 0;
        for (int i = 0; i < GameConstants::SECONDARY_OBJECT_SLOTS; i++) {
            const SecondaryObject& sec = object_mgr_.secondary(i);
            if (sec.y == 0) continue;
            int dx = static_cast<int>(static_cast<int8_t>(sec.x - ax));
            int dy = static_cast<int>(static_cast<int8_t>(sec.y - ay));
            int abs_dx = dx < 0 ? -dx : dx;
            int abs_dy = dy < 0 ? -dy : dy;
            bool in_range = abs_dx <= prom && abs_dy <= prom;
            const char* name = (sec.type < static_cast<uint8_t>(ObjectType::COUNT))
                ? object_type_name(static_cast<ObjectType>(sec.type))
                : "UNKNOWN";
            char line[96];
            std::snprintf(line, sizeof(line),
                          "  %c%s @%u,%u (%+d,%+d)\n",
                          in_range ? '*' : ' ', name,
                          sec.x, sec.y, dx, dy);
            overlay += line;
            secondary_count++;
        }
        if (secondary_count == 0) overlay += "  (empty)\n";
    }
    overlay += selected_tile_info_;
    renderer_->set_overlay_text(overlay.c_str());

    // FPS readout, independent of the debug overlay toggle. Empty when
    // [debug] show_fps is off so the renderer's box is suppressed.
    if (show_fps_) {
        char fps_buf[24];
        std::snprintf(fps_buf, sizeof(fps_buf), "%.1f fps", fps_value_);
        renderer_->set_fps_text(fps_buf);
    } else {
        renderer_->set_fps_text("");
    }

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

    bool algo_only = renderer_->algo_only_enabled();
    for (int dy = 0; dy < vp_h; dy++) {
        uint8_t wy = static_cast<uint8_t>(start_y + dy);
        for (int dx = 0; dx < vp_w; dx++) {
            uint8_t wx = static_cast<uint8_t>(start_x + dx);
            ResolvedTile res = resolve_tile_with_tertiary(landscape_, wx, wy);
            uint8_t tile      = res.tile_and_flip;
            uint8_t tile_type = tile & TileFlip::TYPE_MASK;
            uint8_t tile_flip = tile & TileFlip::MASK;

            // "Algo only" debug: replace cells sourced from
            // map_overlay_data with what the procedural generator
            // WOULD have output for the same (x, y). Bypasses the
            // tertiary attachment so authored doors / switches /
            // spawnable objects don't pop. Useful for inspecting the
            // bake's procedural output beneath the hand-authored
            // interiors.
            if (algo_only && landscape_.tile_from_map_data(wx, wy)) {
                tile = landscape_.compute_algo_tile(wx, wy);
                tile_type = tile & TileFlip::TYPE_MASK;
                tile_flip = tile & TileFlip::MASK;
                res.tertiary_index = -1;
            }

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
            info.has_tertiary =
                landscape_.tertiary_index_at(wx, wy) != Landscape::NO_TERTIARY;
            info.from_map_data = landscape_.tile_from_map_data(wx, wy);
            info.map_data_offset = landscape_.map_data_offset(wx, wy);
            info.map_data_aliased = landscape_.map_data_offset_aliased(wx, wy);
            info.tertiary_source_aliased =
                landscape_.tertiary_source_aliased(wx, wy);
            info.switch_x_aliased = landscape_.switch_x_aliased(wx, wy);
            // is_switch = resolved tile_type == 0x08 SWITCH. Catches both
            // direct (raw 0x08 + SWITCH-range tertiary) and redirect (raw
            // 0x00..0x07 marker whose tertiary tile_and_flip is rewritten,
            // e.g. METAL_DOOR @ X=227). Gate on from_map_data.
            {
                uint16_t cell_idx =
                    landscape_.tertiary_index_at(wx, wy);
                if (cell_idx != Landscape::NO_TERTIARY &&
                    landscape_.tile_from_map_data(wx, wy)) {
                    uint8_t resolved =
                        landscape_.tertiary_entry(cell_idx).tile_and_flip
                        & TileFlip::TYPE_MASK;
                    info.is_switch =
                        resolved == static_cast<uint8_t>(TileType::SWITCH);
                } else {
                    info.is_switch = false;
                }
            }

            renderer_->render_tile(wx, wy, info);
        }
    }

    // Collision-debug overlay: shade per-x-section solid region using the
    // same tile_threshold_at_x + tile_obstruction_v_flip_bit as collision
    // probes. Doors run through substitute_door_for_obstruction so closed
    // doors show their STONE_SLOPE_78 shape, matching what the AABB sees.
    if (renderer_->collision_enabled()) {
        auto& all_primaries =
            reinterpret_cast<const std::array<Object, GameConstants::PRIMARY_OBJECT_SLOTS>&>(
                object_mgr_.object(0));
        for (int dy = 0; dy < vp_h; dy++) {
            uint8_t wy = static_cast<uint8_t>(start_y + dy);
            for (int dx = 0; dx < vp_w; dx++) {
                uint8_t wx = static_cast<uint8_t>(start_x + dx);
                ResolvedTile res = resolve_tile_with_tertiary(landscape_, wx, wy);
                uint8_t subst = Collision::substitute_door_for_obstruction(
                    res.tile_and_flip, res.data_offset, all_primaries,
                    object_mgr_.tertiary_data_byte(res.data_offset));
                uint8_t type = subst & TileFlip::TYPE_MASK;
                if (!Collision::is_tile_type_solid(type)) continue;

                bool fh = (subst & TileFlip::HORIZONTAL) != 0;
                bool fv = (subst & TileFlip::VERTICAL)   != 0;
                bool coll_fv = fv ^ tile_obstruction_v_flip_bit(type);

                // 8 x-sections, sampled at centre (xs*0x20+0x10) to match
                // tile_threshold_at_x's x_frac>>5 quantisation. Two passes:
                // red fill then yellow surface line — line makes the
                // 8-section staircase explicit on adjacent same-threshold.
                uint8_t prev_threshold = 0;
                bool    prev_has_surface = false;
                for (int xs = 0; xs < 8; xs++) {
                    uint8_t xf_sample = static_cast<uint8_t>(xs * 0x20 + 0x10);
                    uint8_t threshold = tile_threshold_at_x(
                        type, fh, fv, xf_sample);
                    uint8_t y0, h;
                    bool    has_fill;
                    if (coll_fv) {
                        // Solid when y_frac <= threshold -> fill 0..threshold.
                        has_fill = (threshold != 0);
                        y0 = 0;
                        h  = threshold;
                    } else {
                        // Solid when y_frac > threshold -> fill threshold+1..0xff.
                        has_fill = (threshold < 0xff);
                        y0 = static_cast<uint8_t>(threshold + 1);
                        h  = static_cast<uint8_t>(0xff - threshold);
                    }
                    if (has_fill) {
                        renderer_->render_tile_shade_rect(
                            wx, wy,
                            static_cast<uint8_t>(xs * 0x20), y0,
                            0x20, h, 0xCC2222);
                    }
                    // Surface line + risers gated on has_fill. Empty section's
                    // threshold sits on the tile edge -> drawing produces stray
                    // horizontal stripes. Fully-solid sections keep the line
                    // (rock block floor/ceiling).
                    bool has_surface = has_fill;
                    if (has_surface) {
                        renderer_->render_tile_shade_rect(
                            wx, wy,
                            static_cast<uint8_t>(xs * 0x20), threshold,
                            0x20, 1, 0xFFEE44);
                    }
                    // Vertical riser at the section boundary, but only
                    // when both adjacent sections have a real surface
                    // and the threshold actually changed. Either side
                    // missing a surface means the "riser" would dangle
                    // into empty air or full-solid space.
                    if (prev_has_surface && has_surface &&
                        threshold != prev_threshold) {
                        uint8_t lo = (prev_threshold < threshold)
                                     ? prev_threshold : threshold;
                        uint8_t hi = (prev_threshold < threshold)
                                     ? threshold : prev_threshold;
                        renderer_->render_tile_shade_rect(
                            wx, wy,
                            static_cast<uint8_t>(xs * 0x20), lo,
                            1, static_cast<uint8_t>(hi - lo + 1),
                            0xFFEE44);
                    }
                    prev_threshold   = threshold;
                    prev_has_surface = has_surface;
                }
            }
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
        bool teleporting = (obj.flags & ObjectFlags::TELEPORTING) != 0;
        info.teleport_timer = teleporting ? obj.timer : 0;
        // &1c09 STA this_object_y with #&11: at the dematerialise frame
        // the 6502 punts the sprite offscreen for one tick before the
        // &1c1e reposition. Equivalent here is to skip drawing it.
        if (teleporting && obj.timer == 0x11) info.visible = false;

        renderer_->render_object(obj.x, obj.y, info);
    }

    // Health-bar overlay above each primary; fill = energy / max. For
    // collectables (types 0x4a..0x64) bit 7 is the "undisturbed" pin —
    // mask it off and treat 0x7f as full so pinned items don't read full.
    if (renderer_->health_bars_enabled()) {
        constexpr uint8_t BAR_PAD_X   = 8;   // 0.5 px each side
        constexpr uint8_t BAR_HEIGHT  = 16;  // ~2 BBC rows tall
        constexpr uint8_t BAR_BORDER  = 2;   // black outline thickness
        constexpr uint8_t BAR_OFFSET  = 24;  // gap above sprite top
        for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& obj = object_mgr_.object(i);
            if (!obj.is_active()) continue;
            uint8_t sid = obj.sprite;
            if (sid > 0x80) continue;
            int w_pix = sprite_atlas[sid].w;
            int sprite_w_frac = (w_pix > 0 ? (w_pix - 1) : 0) * 16;
            int outer_w_frac = sprite_w_frac + BAR_PAD_X * 2;
            // Collectables 0x4a..0x64 store undisturbed pin in energy bit 7;
            // mask it off and use 0x7f as full. Doors use full 8 bits — else
            // 0x80->0x7f looked like wrap back to full instead of near-zero.
            uint8_t tidx = static_cast<uint8_t>(obj.type);
            bool collectable_pin = (tidx >= 0x4a && tidx <= 0x64);
            uint8_t energy = collectable_pin ? (obj.energy & 0x7f) : obj.energy;
            int     e_max  = collectable_pin ? 0x7f : 0xff;
            int inner_w_frac = outer_w_frac - BAR_BORDER * 2;
            int fill_w_frac = (inner_w_frac * energy + e_max / 2) / e_max;

            // Position above sprite. Use 16-bit math so the offset can
            // straddle a whole-tile boundary cleanly.
            int y_combined = static_cast<int>(obj.y.whole) * 256 +
                             static_cast<int>(obj.y.fraction) - BAR_OFFSET;
            uint8_t bar_y      = static_cast<uint8_t>((y_combined >> 8) & 0xff);
            uint8_t bar_y_frac = static_cast<uint8_t>(y_combined & 0xff);

            // X offset: shift left by BAR_PAD_X so the bar centres on
            // the sprite. straddle whole-tile boundary same way as Y.
            int x_combined = static_cast<int>(obj.x.whole) * 256 +
                             static_cast<int>(obj.x.fraction) - BAR_PAD_X;
            uint8_t bar_x      = static_cast<uint8_t>((x_combined >> 8) & 0xff);
            uint8_t bar_x_frac = static_cast<uint8_t>(x_combined & 0xff);

            // 1. Black border (full outer rect).
            renderer_->render_tile_shade_rect(
                bar_x, bar_y, bar_x_frac, bar_y_frac,
                static_cast<uint8_t>(outer_w_frac), BAR_HEIGHT, 0x000000);

            // 2. Dim-grey background, inset by BAR_BORDER.
            int inner_x = x_combined + BAR_BORDER;
            int inner_y = y_combined + BAR_BORDER;
            uint8_t in_x      = static_cast<uint8_t>((inner_x >> 8) & 0xff);
            uint8_t in_x_frac = static_cast<uint8_t>(inner_x & 0xff);
            uint8_t in_y      = static_cast<uint8_t>((inner_y >> 8) & 0xff);
            uint8_t in_y_frac = static_cast<uint8_t>(inner_y & 0xff);
            renderer_->render_tile_shade_rect(
                in_x, in_y, in_x_frac, in_y_frac,
                static_cast<uint8_t>(inner_w_frac),
                static_cast<uint8_t>(BAR_HEIGHT - BAR_BORDER * 2),
                0x444444);

            // 3. HP-coloured fill (green / yellow / red gradient).
            uint32_t fill_rgb = (energy > 0x55) ? 0x33ff44
                              : (energy > 0x2a) ? 0xffee33
                                                : 0xff3333;
            if (fill_w_frac > 0) {
                renderer_->render_tile_shade_rect(
                    in_x, in_y, in_x_frac, in_y_frac,
                    static_cast<uint8_t>(fill_w_frac),
                    static_cast<uint8_t>(BAR_HEIGHT - BAR_BORDER * 2),
                    fill_rgb);
            }

            // Numeric energy right of bar, floor left. Right anchor in
            // WORLD coords (left+outer_w) so world_to_screen scales under
            // zoom — pixel-dx alone lands inside the bar when tile_px_x
            // grows.
            {
                int right_combined = x_combined + outer_w_frac;
                uint8_t right_x      = static_cast<uint8_t>((right_combined >> 8) & 0xff);
                uint8_t right_x_frac = static_cast<uint8_t>(right_combined & 0xff);

                char val_buf[8];
                std::snprintf(val_buf, sizeof(val_buf), "%u",
                              static_cast<unsigned>(energy));
                renderer_->render_world_label(
                    right_x, bar_y, right_x_frac, bar_y_frac,
                    /*dx=*/2, /*dy=*/0,
                    val_buf, 0xffffff, 0x000000);

                uint8_t floor = energy_floor_for_obj(obj);
                if (floor > 0) {
                    char floor_buf[8];
                    std::snprintf(floor_buf, sizeof(floor_buf), "%u",
                                  static_cast<unsigned>(floor));
                    int floor_len = 0;
                    for (const char* p = floor_buf; *p; ++p) ++floor_len;
                    renderer_->render_world_label(
                        bar_x, bar_y, bar_x_frac, bar_y_frac,
                        /*dx=*/-(floor_len * 8 + 2),
                        /*dy=*/0,
                        floor_buf, 0x888888, 0x000000);
                }
            }
        }
    }

    // "Mood" overlay — independent of the Health bar. Shows the
    // mood word + FED! status text above each stimuli-eligible NPC.
    if (renderer_->mood_overlay_enabled()) {
        for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& obj = object_mgr_.object(i);
            if (!obj.is_active()) continue;
            if (!Mood::has_category(obj.type)) continue;

            // Mood word, colour-coded so negative reads red and
            // positive reads green at a glance.
            uint8_t mood = Mood::get_mood(obj);
            const char* label = "????";
            uint32_t mood_rgb = 0x888888;
            switch (mood) {
                case NPCMood::PLUS_ONE:  label = "HAPPY"; mood_rgb = 0x33ff44; break;
                case NPCMood::ZERO:      label = "CALM";  mood_rgb = 0xcccccc; break;
                case NPCMood::MINUS_ONE: label = "ANGRY"; mood_rgb = 0xffaa33; break;
                case NPCMood::MINUS_TWO: label = "FURY";  mood_rgb = 0xff3333; break;
                default: break;
            }
            renderer_->render_world_label(obj.x.whole, obj.y.whole,
                                          obj.x.fraction, obj.y.fraction,
                                          /*dx=*/0, /*dy=*/-12,
                                          label, mood_rgb, 0x000000);

            // "FED!" status — sits above the mood badge while the
            // WAS_FED bit (0x10 of state) is latched. Cleared by the
            // at-home gift drop in update_imp.
            constexpr uint8_t kNPC_WAS_FED = 0x10;
            if (obj.state & kNPC_WAS_FED) {
                renderer_->render_world_label(
                    obj.x.whole, obj.y.whole,
                    obj.x.fraction, obj.y.fraction,
                    /*dx=*/0, /*dy=*/-22,
                    "FED!", 0x33ff44, 0x000000);
            }
        }
    }

    // "Damage" overlay — paint per-frame damage numbers + explosion radius
    // rings recorded by the various behaviours during this tick. Refresh
    // the recorded target position from each event's slot first so the
    // number tracks moving NPCs over its TTL window instead of being
    // stranded at the hit-frame coordinates. Each push is rendered as
    // its own floating number — no per-slot coalescing — so a target
    // hit twice shows two distinct numbers rising at different ages.
    if (renderer_->damage_overlay_enabled()) {
        for (DamageVisual& ev : damage_events_) {
            if (ev.tgt_slot < 0 ||
                ev.tgt_slot >= GameConstants::PRIMARY_OBJECT_SLOTS) continue;
            const Object& tgt = object_mgr_.object(ev.tgt_slot);
            if (!tgt.is_active()) continue;
            ev.tgt_x = tgt.x.whole;
            ev.tgt_y = tgt.y.whole;
            ev.tgt_x_frac = tgt.x.fraction;
            ev.tgt_y_frac = tgt.y.fraction;
        }
        renderer_->render_damage_events(damage_events_);
    }

    // Always-on floating-label notifications (e.g. an imp absorbing
    // food). Drawn unconditionally so the player sees rare events
    // without having to enable a debug overlay; TTL fades them out
    // automatically.
    for (const FloatingLabel& f : floating_labels_) {
        renderer_->render_world_label(f.world_x, f.world_y,
                                      f.x_frac, f.y_frac,
                                      /*dx=*/0,
                                      /*dy=*/-20,
                                      f.text, f.rgb, 0x000000);
    }

    // Debug AABB overlay — pixel boxes used by object-object collision
    // (see collision.cpp sprite_*_units). Toggled by the "Collision"
    // HUD checkbox. Player cyan, weight-7 statics red, else yellow.
    if (renderer_->collision_enabled()) {
        for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
            const Object& obj = object_mgr_.object(i);
            if (!obj.is_active()) continue;
            if (obj.sprite > 0x80) continue;
            const SpriteAtlasEntry& e = sprite_atlas[obj.sprite];
            // Visual extent: W*16 / H*8 covers all atlas pixels. The 6502
            // collision math uses (W-1)*16 / (H-1)*8 (distance between
            // first and last pixel centres), which leaves the last column
            // / row outside the drawn box.
            int w_u = e.w * 16;
            int h_u = e.h * 8;
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

    // Render particles (on top of tiles/objects, below HUD). Descending
    // walk so swap-remove for the &2118-&2120 foreground-kill (particle
    // landed on a colour-8..15 tile pixel and lacks PARTICLE_FLAG_
    // FOREGROUND) doesn't skip slots — particles_.remove does the same
    // shuffle as the 6502's remove_particle at &213a.
    for (int i = particles_.count() - 1; i >= 0; i--) {
        const Particle& p = particles_.get(i);
        renderer_->render_particle(p.x, p.x_fraction,
                                   p.y, p.y_fraction,
                                   p.colour_and_flags & 0x07);
        if ((p.colour_and_flags & ParticleFlag::FOREGROUND) == 0 &&
            renderer_->query_fg_at(p.x, p.x_fraction,
                                   p.y, p.y_fraction)) {
            particles_.remove(i);
        }
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
            char prim_buf[32];
            // DENSE_NEST / SUCKING_NEST that have been promoted to a
            // primary still carry the same creature counter in
            // tertiary_objects_data — append it the same way the
            // tertiary loop below does.
            if ((obj.type == ObjectType::DENSE_NEST ||
                 obj.type == ObjectType::SUCKING_NEST) &&
                obj.tertiary_data_offset != 0) {
                uint8_t data = object_mgr_.tertiary_data_byte(obj.tertiary_data_offset);
                int remaining = (data & 0x7c) >> 2;
                std::snprintf(prim_buf, sizeof(prim_buf),
                              "%s x%d", label, remaining);
                label = prim_buf;
            }
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
                        // After Option B, the entry carries its own
                        // type byte; tertiary_objects_type_data is no
                        // longer indexed at runtime.
                        obj_type = object_mgr_.tertiary_type_byte(res.type_offset);
                        break;
                    case 0x08: obj_type = 0x42; break; // SWITCH
                }

                const char* label;
                char label_buf[32];
                if (obj_type < static_cast<uint8_t>(ObjectType::COUNT)) {
                    label = object_type_name(static_cast<ObjectType>(obj_type));
                } else {
                    // Skip common environmental tile types — they'd flood
                    // the overlay with POSSIBLE_LEAF / CONSTANT_WIND on
                    // nearly every tile and drown out the interesting
                    // tertiary entries (doors, switches, pipes, etc.).
                    if (ttype == static_cast<uint8_t>(TileType::POSSIBLE_LEAF) ||
                        ttype == static_cast<uint8_t>(TileType::CONSTANT_WIND)) {
                        continue;
                    }
                    label = tile_type_name(ttype);
                }
                // Nests and pipes carry their remaining-creature count
                // in bits 6..2 of the tertiary data byte (port of &3e54-
                // &3e6f spawn check: ASL; CMP #8 -> ≥1 creature; SBC #4
                // per spawn). Append it to the label so the overlay
                // shows e.g. "PIPE x7" or "NEST x0".
                if (ttype == static_cast<uint8_t>(TileType::NEST) ||
                    ttype == static_cast<uint8_t>(TileType::PIPE)) {
                    uint8_t data = object_mgr_.tertiary_data_byte(res.data_offset);
                    int remaining = (data & 0x7c) >> 2;
                    std::snprintf(label_buf, sizeof(label_buf),
                                  "%s x%d", label, remaining);
                    label = label_buf;
                }
                renderer_->render_debug_marker(wx, wy, 0xDD3333, label);
            }
        }
    }

    // Wiring overlay: switch/transporter sources from active primaries
    // (live pos) AND tertiary entries (recover tile_y by column scan).
    // Green = switch->door, cyan = transporter->destination. O(n*256), gated
    // on the checkbox.
    {
        bool show_switches   = renderer_->switches_enabled();
        bool show_transports = renderer_->transports_enabled();
        if (show_switches || show_transports) {
            // Click-to-isolate: if the user has clicked a tile that's a
            // switch (or transporter), restrict the overlay to wires
            // sourced at that one cell. Otherwise show every wire as
            // before. We resolve "is the click a switch / xport" once
            // up front via the cell's tertiary entry's tile_and_flip
            // type — same definition the wiring loops use below.
            bool filter_single = false;
            uint8_t single_x = 0, single_y = 0;
            if (editor_has_highlight_) {
                uint16_t cell_idx = landscape_.tertiary_index_at(
                    editor_highlight_x_, editor_highlight_y_);
                if (cell_idx != Landscape::NO_TERTIARY) {
                    uint8_t resolved =
                        landscape_.tertiary_entry(cell_idx).tile_and_flip
                        & TileFlip::TYPE_MASK;
                    if ((show_switches &&
                         resolved == static_cast<uint8_t>(TileType::SWITCH)) ||
                        (show_transports &&
                         resolved == static_cast<uint8_t>(TileType::TRANSPORTER))) {
                        filter_single = true;
                        single_x = editor_highlight_x_;
                        single_y = editor_highlight_y_;
                    }
                }
            }

            // --- Sources from active primaries ---------------------------
            for (int i = 0; i < GameConstants::PRIMARY_OBJECT_SLOTS; i++) {
                const Object& src = object_mgr_.object(i);
                if (!src.is_active()) continue;
                if (filter_single &&
                    (src.x.whole != single_x || src.y.whole != single_y)) {
                    continue;
                }

                if (show_switches && src.type == ObjectType::SWITCH) {
                    uint8_t effect_id = static_cast<uint8_t>(
                        src.tertiary_data_offset >> 3);
                    uint8_t targets[8];
                    int n = Behaviors::switch_effect_targets(
                        effect_id, targets, 8);
                    for (int t = 0; t < n; t++) {
                        uint8_t dx, dy;
                        if (find_primary_at_slot(object_mgr_, targets[t],
                                                  dx, dy) ||
                            resolve_data_offset_to_tile(
                                landscape_, targets[t], dx, dy)) {
                            renderer_->render_wire(src.x.whole, src.y.whole,
                                                   dx, dy, 0x33DD33);
                        }
                    }
                } else if (show_transports &&
                           src.type == ObjectType::TRANSPORTER_BEAM) {
                    uint8_t dest = static_cast<uint8_t>(
                        (src.tertiary_data_offset >> 1) & 0x0f);
                    uint8_t dx, dy;
                    if (Behaviors::transporter_destination(dest, dx, dy)) {
                        renderer_->render_wire(src.x.whole, src.y.whole,
                                               dx, dy, 0x33CCDD);
                    }
                }
            }

            // Sources still in tertiary storage. Walk every entry and
            // dispatch on resolved tile_and_flip type — keeps REDIRECT
            // switches/transporters visible (e.g. raw METAL_DOOR cells at
            // (227,156)/(227,188) rewritten to SWITCH 0x08).
            int n_entries = landscape_.tertiary_count();
            for (int idx = 1; idx < n_entries; ++idx) {
                const TertiaryEntry& e = landscape_.tertiary_entry(idx);
                uint8_t resolved_type = e.tile_and_flip & TileFlip::TYPE_MASK;
                bool is_switch =
                    resolved_type == static_cast<uint8_t>(TileType::SWITCH);
                bool is_xport =
                    resolved_type == static_cast<uint8_t>(TileType::TRANSPORTER);
                if (!(show_switches && is_switch) &&
                    !(show_transports && is_xport)) continue;

                // If a primary already owns this entry, the first loop
                // drew the wire from the live primary position — skip
                // here to avoid double-wiring. Pass the full uint16_t
                // idx; truncating to uint8_t would miss any entry
                // beyond slot 255 (most redirect switches sit there).
                uint8_t sx_primary, sy_primary;
                if (find_primary_at_slot(object_mgr_,
                                          static_cast<uint16_t>(idx),
                                          sx_primary, sy_primary)) continue;

                uint8_t sx, sy;
                if (!find_tertiary_tile(landscape_, /*tile_type unused*/ 0,
                                         idx, sx, sy)) continue;
                if (filter_single &&
                    (sx != single_x || sy != single_y)) continue;

                uint8_t data = static_cast<uint8_t>(e.data & 0x7f);
                uint32_t rgb = is_switch ? 0x33DD33 : 0x33CCDD;

                if (is_switch) {
                    uint8_t effect_id = static_cast<uint8_t>(data >> 3);
                    uint8_t targets[8];
                    int n = Behaviors::switch_effect_targets(
                        effect_id, targets, 8);
                    for (int t = 0; t < n; t++) {
                        uint8_t dx, dy;
                        if (find_primary_at_slot(object_mgr_,
                                                  targets[t],
                                                  dx, dy) ||
                            resolve_data_offset_to_tile(
                                landscape_, targets[t], dx, dy)) {
                            renderer_->render_wire(sx, sy, dx, dy, rgb);
                        }
                    }
                } else {
                    // Transporter beam: data byte's bits 1-4 are the
                    // destination index in transporter_destinations_xy.
                    uint8_t dest =
                        static_cast<uint8_t>((data >> 1) & 0x0f);
                    uint8_t dx, dy;
                    if (Behaviors::transporter_destination(
                            dest, dx, dy)) {
                        renderer_->render_wire(sx, sy, dx, dy, rgb);
                    }
                }
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
    ps.has_jetpack_booster = false;
    for (int i = 0; i < 5; i++) ps.pockets[i] = pockets_[i];
    ps.pockets_used = pockets_used_;
    for (int i = 0; i < 8; i++) ps.keys[i] = player_keys_collected_[i];
    for (int i = 0; i < 6; i++) ps.weapon_energy[i] = weapon_energy_[i];
    renderer_->render_hud(ps);

    renderer_->end_frame();
}

// =============================================================================
// Build the top-right tile-info overlay for cell (tx, ty). Called from
// the click handler and (when an editor highlight is active) every
// frame, so live tertiary mutations show up without re-clicking.
// =============================================================================
void Game::refresh_selected_tile_info(uint8_t tx, uint8_t ty) {
    uint8_t raw = landscape_.get_tile(tx, ty);
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

    uint8_t db = object_mgr_.tertiary_data_byte(res.data_offset);
    uint8_t tb = object_mgr_.tertiary_type_byte(res.data_offset);
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
                  "tert idx=%d\n"
                  "data byte=0x%02x bit7=%s\n"
                  "type byte=0x%02x\n"
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
                  res.tertiary_index,
                  db, (db & 0x80) ? "SET" : "clear",
                  tb,
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
