// Port-only debug overlays + editor widgets. pixel_renderer.cpp calls
// into pr_debug:: helpers from begin_frame / end_frame / render_tile /
// render_hud / process_mouse seams to keep the two TUs decoupled.

#include "rendering/pixel_renderer.h"
#include "rendering/sprite_atlas.h"
#include "rendering/sprite_data.h"
#include "rendering/palette.h"
#include "rendering/font8x8.h"
#include "rendering/debug_names.h"
#include "world/tile_data.h"
#include "objects/object_data.h"
#include <cstdio>
#include <cstring>
#include <ostream>

// Debug HUD-strip checkboxes — geometry constants shared between
// render_hud_panels and consume_left_click to keep hit-test in sync.
static constexpr int CHECKBOX_SIZE = 10;
static constexpr int CHECKBOX_PAD  = 4;
static constexpr int CHECKBOX_LABEL_GAP = 4;
static constexpr int CHECKBOX_SLOT_W = 110;

struct DebugCheckbox {
    const char* label;
    bool* state;
};

static int checkbox_slot_x(int idx) {
    return 10 + idx * CHECKBOX_SLOT_W;
}

static int checkbox_slot_y(int hud_y_px) {
    return hud_y_px + (16 - CHECKBOX_SIZE) / 2;
}

// Edit palette: 16x4 grid of tile types 0x00..0x3f, above HUD when "Edit" on.
static constexpr int PALETTE_COLS = 16;
static constexpr int PALETTE_ROWS = 4;
static constexpr int PALETTE_CELL_W = 64;
static constexpr int PALETTE_CELL_H = 40;
static constexpr int PALETTE_PAD_X  = 10;
static constexpr int PALETTE_PAD_Y  = 4;
static constexpr int PALETTE_W =
    PALETTE_COLS * PALETTE_CELL_W + 2 * PALETTE_PAD_X;
static constexpr int PALETTE_H =
    PALETTE_ROWS * PALETTE_CELL_H + 2 * PALETTE_PAD_Y;

static int palette_top_y(int hud_y) { return hud_y - PALETTE_H; }
static int palette_left_x()         { return 0; }

static int palette_cell_x(uint8_t t) {
    int col = t % PALETTE_COLS;
    return palette_left_x() + PALETTE_PAD_X + col * PALETTE_CELL_W;
}
static int palette_cell_y(uint8_t t, int hud_y) {
    int row = t / PALETTE_COLS;
    return palette_top_y(hud_y) + PALETTE_PAD_Y + row * PALETTE_CELL_H;
}

// Object palette (creatures/items): 16x3 grid above tile palette. Click
// switches right-click action from "paint tile" to "place object".
static constexpr int OBJ_PALETTE_COLS  = 16;
static constexpr int OBJ_PALETTE_ROWS  = 3;
static constexpr int OBJ_PALETTE_GAP_Y = 8;
static constexpr int OBJ_PALETTE_W =
    OBJ_PALETTE_COLS * PALETTE_CELL_W + 2 * PALETTE_PAD_X;
static constexpr int OBJ_PALETTE_H =
    OBJ_PALETTE_ROWS * PALETTE_CELL_H + 2 * PALETTE_PAD_Y;

static int obj_palette_top_y(int hud_y) {
    return palette_top_y(hud_y) - OBJ_PALETTE_GAP_Y - OBJ_PALETTE_H;
}
static int obj_palette_cell_x(int idx) {
    int col = idx % OBJ_PALETTE_COLS;
    return palette_left_x() + PALETTE_PAD_X + col * PALETTE_CELL_W;
}
static int obj_palette_cell_y(int idx, int hud_y) {
    int row = idx / OBJ_PALETTE_COLS;
    return obj_palette_top_y(hud_y) + PALETTE_PAD_Y + row * PALETTE_CELL_H;
}

// Curated list of placeable object types. Each row is one cell in the
// object palette panel; the index into this table is what
// consume_object_palette_click returns.
struct ObjectPaletteEntry { uint8_t object_type; const char* label; };
static const ObjectPaletteEntry OBJECT_PALETTE[] = {
    // Creatures
    { 0x09, "RSlime" },
    { 0x0a, "GSlime" },
    { 0x0b, "YSlime" },
    { 0x06, "RFrog"  },
    { 0x07, "GFrog"  },
    { 0x08, "InvFrog"},
    { 0x0c, "DenseN" },
    { 0x0d, "SuckN"  },
    { 0x0e, "Fish"   },
    { 0x0f, "Worm"   },
    { 0x10, "Piranha"},
    { 0x11, "Wasp"   },
    { 0x27, "Maggot" },
    { 0x28, "Garg"   },
    { 0x29, "Imp1"   },
    { 0x2e, "Bird1"  },
    // Robots
    { 0x21, "Hover"  },
    { 0x1c, "MRoll"  },
    { 0x1d, "RRoll"  },
    { 0x1e, "BRoll"  },
    { 0x1f, "GTurret"},
    { 0x20, "CTurret"},
    { 0x22, "MClaw"  },
    { 0x23, "CClaw"  },
    { 0x24, "GClaw"  },
    { 0x25, "RClaw"  },
    // Notable specials
    { 0x01, "Chatter"},
    { 0x38, "InCha"  },
    { 0x03, "Fluffy" },
    { 0x02, "Crew"   },
    { 0x48, "MagMch" },
    { 0x55, "CorBld" },
    { 0x43, "Piano"  },
    // Pickups / items
    { 0x4b, "PowPod" },
    { 0x58, "CoroX"  },
    { 0x4c, "FlaskE" },
    { 0x4d, "FlaskF" },
    { 0x50, "Grenade"},
    { 0x4e, "RCD"    },
    { 0x4f, "CCD"    },
    { 0x47, "AlienW" },
    { 0x33, "RMush"  },
    { 0x34, "BMush"  },
    // Weapons & gear
    { 0x5a, "Pistol" },
    { 0x5b, "Icer"   },
    { 0x5c, "Blaster"},
    { 0x5d, "Plasma" },
    { 0x5e, "Suit"   },
    { 0x5f, "FireImm"},
    { 0x60, "MushImm"},
};
static constexpr int OBJECT_PALETTE_N =
    sizeof(OBJECT_PALETTE) / sizeof(OBJECT_PALETTE[0]);

// FlipX / FlipY toggle buttons + a Detach button, in a small column
// to the right of the palette grid.
static constexpr int PALETTE_FLIP_GAP = 12;
static constexpr int PALETTE_FLIP_W   = PALETTE_CELL_W;
static constexpr int PALETTE_FLIP_H   = PALETTE_CELL_H;
static int palette_flip_x() {
    return palette_left_x() + PALETTE_W + PALETTE_FLIP_GAP;
}
static int palette_flip_y(int idx, int hud_y) {
    return palette_top_y(hud_y) + PALETTE_PAD_Y + idx * PALETTE_FLIP_H;
}

// Sprite-viewer layout — three panels: palette grid (16x8 atlas
// sprites, click selects), ROM sheet at 4x, detail at 8x.
static constexpr int SV_HEADER_Y      = 16;
static constexpr int SV_HEADER_H      = 24;
static constexpr int SV_PANELS_Y      = SV_HEADER_Y + SV_HEADER_H + 16;
static constexpr int SV_PANEL_PAD     = 16;

static constexpr int SV_GRID_COLS     = 16;
static constexpr int SV_GRID_ROWS     = 8;
static constexpr int SV_GRID_CELL     = 32;
static constexpr int SV_GRID_X        = 16;
static constexpr int SV_GRID_W        = SV_GRID_COLS * SV_GRID_CELL;
static constexpr int SV_GRID_H        = SV_GRID_ROWS * SV_GRID_CELL;

static constexpr int SV_SHEET_SCALE   = 4;
static constexpr int SV_SHEET_W_ATLAS = 128;
static constexpr int SV_SHEET_H_ATLAS = 81;
static constexpr int SV_SHEET_W       = SV_SHEET_W_ATLAS * SV_SHEET_SCALE;
static constexpr int SV_SHEET_H       = SV_SHEET_H_ATLAS * SV_SHEET_SCALE;
static constexpr int SV_SHEET_X       = SV_GRID_X + SV_GRID_W + SV_PANEL_PAD;

static constexpr int SV_DETAIL_SCALE  = 8;
static constexpr int SV_DETAIL_X      = SV_SHEET_X + SV_SHEET_W + SV_PANEL_PAD;
static constexpr int SV_DETAIL_W      = 360;

static int sv_grid_sprite_id(int cell_idx) {
    return (cell_idx >= 0 && cell_idx < 125) ? cell_idx : -1;
}

static int sv_grid_cell_x(int cell_idx) {
    return SV_GRID_X + (cell_idx % SV_GRID_COLS) * SV_GRID_CELL;
}
static int sv_grid_cell_y(int cell_idx) {
    return SV_PANELS_Y + (cell_idx / SV_GRID_COLS) * SV_GRID_CELL;
}

// Sprite compounds — named groups of consecutive sprite_ids that the
// BBC cycles through as animation frames for a single object kind.
// Frame counts come from the disassembly's update routines.
struct SpriteCompound {
    const char* name;
    uint8_t base;
    uint8_t count;
};
static constexpr SpriteCompound SPRITE_COMPOUNDS[] = {
    { "BULLET",       0x08, 6 },
    { "FROGMAN",      0x10, 3 },
    { "SLIME",        0x1c, 4 },
    { "WASP",         0x4f, 3 },
    { "WORM",         0x52, 3 },
    { "BIRD",         0x59, 4 },
    { "IMP_WALKING",  0x64, 3 },
    { "IMP_CLIMBING", 0x67, 2 },
    { "LIGHTNING",    0x6d, 4 },
    { "PIRANHA",      0x72, 3 },
};
static constexpr int SPRITE_COMPOUND_N =
    sizeof(SPRITE_COMPOUNDS) / sizeof(SPRITE_COMPOUNDS[0]);

static const SpriteCompound* sv_find_compound(int sid) {
    for (int i = 0; i < SPRITE_COMPOUND_N; ++i) {
        const SpriteCompound& c = SPRITE_COMPOUNDS[i];
        if (sid >= c.base && sid < c.base + c.count) {
            return &SPRITE_COMPOUNDS[i];
        }
    }
    return nullptr;
}

// Find a representative palette LUT for a sprite_id by walking the
// object and tile tables for the first reference.
static bool sv_resolve_sprite_lut(int sid, uint32_t lut[4]) {
    for (int t = 0; t < 0x65; ++t) {
        if (object_types_sprite[t] == sid) {
            uint8_t pal_byte = object_types_palette_and_pickup[t] & 0x7f;
            resolve_palette(pal_byte, /*is_tile=*/false, lut);
            return true;
        }
    }
    for (int t = 0; t < 64; ++t) {
        uint8_t entry = TILE_SPRITE_ID[t];
        if (entry != 0xff && (entry & 0x7f) == sid) {
            resolve_palette(tiles_palette_table[t & 0x3f],
                            /*is_tile=*/true, lut);
            return true;
        }
    }
    return false;
}

// =============================================================================
// PixelRenderer member methods used only by debug code — defined here
// so they live with the rest of the editor / overlay code.
// =============================================================================
void PixelRenderer::hatch_rect(int x, int y, int w, int h,
                               uint32_t color, int step) {
    int x0 = std::max(0, x), y0 = std::max(0, y);
    int x1 = std::min(f.width, x + w), y1 = std::min(f.height, y + h);
    int hud_y = hud_y_px();
    if (y1 > hud_y) y1 = hud_y;
    for (int py = y0; py < y1; py++) {
        uint32_t* row = &buf[(size_t)py * f.width];
        int phase = (py % step) ? (step - (py % step)) : 0;
        for (int px = x0 + phase; px < x1; px += step) row[px] = color;
    }
}

void PixelRenderer::blit_tile_preview(int dst_x, int dst_y,
                                      int dst_w, int dst_h,
                                      uint8_t tile_type,
                                      bool flip_h, bool flip_v) {
    uint8_t entry = TILE_SPRITE_ID[tile_type & 0x3f];
    if (entry == 0xff) return;
    uint8_t sid = entry & 0x7f;
    uint8_t palette = tiles_palette_table[tile_type & 0x3f];
    uint32_t lut[4];
    uint8_t  fg[4];
    resolve_palette_with_fg(palette, /*is_tile=*/true, lut, fg);
    blit_sprite_at_native(dst_x, dst_y, dst_w, dst_h,
                          sid, lut, flip_h, flip_v);
}

// =============================================================================
// pr_debug:: hooks invoked from pixel_renderer.cpp
// =============================================================================
namespace pr_debug {

static bool try_events_panel_click(PixelRenderer& r);
static bool try_grid_panel_click  (PixelRenderer& r);
static bool try_saves_panel_click (PixelRenderer& r);

bool consume_left_click(PixelRenderer& r) {
    // Side panels first — both sit over the world so the buttons
    // must claim the click before tile-select gets a chance.
    if (try_events_panel_click(r)) return true;
    if (try_grid_panel_click(r))   return true;
    if (try_saves_panel_click(r))  return true;

    // Main bottom-strip checkboxes. Map mode, Object lbl, Switches,
    // Transports, Rings, Tertiary and Placed Tiles all live in the
    // Tiles submenu (see render_grid_panel) — keep them out of here.
    DebugCheckbox boxes[11] = {
        { "Tiles",      &r.tile_outline_on   },
        { "Debug",      &r.debug_text_on     },
        { "Collision",  &r.collision_on      },
        { "Edit",       &r.editor_on         },
        { "Algo only",  &r.algo_only_on      },
        { "Sprites",    &r.sprite_viewer_on  },
        { "Health",     &r.health_bars_on    },
        { "Damage",     &r.damage_overlay_on },
        { "Mood",       &r.mood_overlay_on   },
        { "Events",     &r.events_panel_on   },
        { "Saves",      &r.saves_panel_on    },
    };
    int hud_y = r.hud_y_px();
    for (int i = 0; i < 11; i++) {
        int cx = checkbox_slot_x(i);
        // Generous hit-area: the whole label's slot width so users
        // can click the text too.
        int hx_end = cx + CHECKBOX_SLOT_W;
        int hy_end = hud_y + 16;
        if (r.f.x >= cx && r.f.x < hx_end &&
            r.f.y >= hud_y && r.f.y < hy_end) {
            *boxes[i].state = !*boxes[i].state;
            return true;
        }
    }

    // Object palette cells (object placement) — sits above the tile
    // palette.
    if (r.editor_on && !r.sprite_viewer_on) {
        int op_top  = obj_palette_top_y(hud_y);
        int op_left = palette_left_x();
        if (r.f.x >= op_left && r.f.x < op_left + OBJ_PALETTE_W &&
            r.f.y >= op_top  && r.f.y < op_top  + OBJ_PALETTE_H) {
            int rel_x = r.f.x - op_left - PALETTE_PAD_X;
            int rel_y = r.f.y - op_top  - PALETTE_PAD_Y;
            if (rel_x >= 0 && rel_y >= 0) {
                int col = rel_x / PALETTE_CELL_W;
                int row = rel_y / PALETTE_CELL_H;
                int idx = row * OBJ_PALETTE_COLS + col;
                if (col >= 0 && col < OBJ_PALETTE_COLS &&
                    row >= 0 && row < OBJ_PALETTE_ROWS &&
                    idx >= 0 && idx < OBJECT_PALETTE_N) {
                    r.has_pending_object_click = true;
                    r.pending_object_click_idx = idx;
                    return true;
                }
            }
        }
    }

    // Tile palette cells.
    if (r.editor_on && !r.sprite_viewer_on) {
        int p_top = palette_top_y(hud_y);
        int p_bot = p_top + PALETTE_H;
        int p_left = palette_left_x();
        int p_right = p_left + PALETTE_W;
        if (r.f.x >= p_left && r.f.x < p_right &&
            r.f.y >= p_top  && r.f.y < p_bot) {
            int rel_x = r.f.x - p_left - PALETTE_PAD_X;
            int rel_y = r.f.y - p_top  - PALETTE_PAD_Y;
            if (rel_x >= 0 && rel_y >= 0) {
                int col = rel_x / PALETTE_CELL_W;
                int row = rel_y / PALETTE_CELL_H;
                if (col >= 0 && col < PALETTE_COLS &&
                    row >= 0 && row < PALETTE_ROWS) {
                    r.has_pending_palette_click = true;
                    r.pending_palette_click_type =
                        static_cast<uint8_t>(row * PALETTE_COLS + col);
                    return true;
                }
            }
        }
    }

    // FlipX / FlipY toggle buttons + Detach button.
    if (r.editor_on && !r.sprite_viewer_on) {
        int fx = palette_flip_x();
        for (int i = 0; i < 2; ++i) {
            int fy = palette_flip_y(i, hud_y);
            if (r.f.x >= fx && r.f.x < fx + PALETTE_FLIP_W &&
                r.f.y >= fy && r.f.y < fy + PALETTE_FLIP_H) {
                r.pending_palette_flip = i;
                return true;
            }
        }
        int fy = palette_flip_y(2, hud_y);
        if (r.f.x >= fx && r.f.x < fx + PALETTE_FLIP_W &&
            r.f.y >= fy && r.f.y < fy + PALETTE_FLIP_H) {
            r.pending_detach_click = true;
            return true;
        }
    }

    // Sprite-viewer clicks: palette grid selects, ROM sheet enters
    // pixel-mode, elsewhere absorb so the click doesn't hit the world.
    if (r.sprite_viewer_on) {
        int gx = r.f.x - SV_GRID_X;
        int gy = r.f.y - SV_PANELS_Y;
        if (gx >= 0 && gx < SV_GRID_W &&
            gy >= 0 && gy < SV_GRID_H) {
            int col = gx / SV_GRID_CELL;
            int row = gy / SV_GRID_CELL;
            int idx = row * SV_GRID_COLS + col;
            int sid = sv_grid_sprite_id(idx);
            if (sid >= 0) {
                r.sprite_viewer_selected = sid;
                r.sprite_viewer_pixel_x = -1;
                r.sprite_viewer_pixel_y = -1;
            }
        } else {
            int sx = r.f.x - SV_SHEET_X;
            int sy = r.f.y - SV_PANELS_Y;
            if (sx >= 0 && sx < SV_SHEET_W &&
                sy >= 0 && sy < SV_SHEET_H) {
                int atlas_x = sx / SV_SHEET_SCALE;
                int atlas_y = sy / SV_SHEET_SCALE;
                r.sprite_viewer_pixel_x = atlas_x;
                r.sprite_viewer_pixel_y = atlas_y;
                for (int sid = 124; sid >= 0; --sid) {
                    const SpriteAtlasEntry& a = sprite_atlas[sid];
                    if (atlas_x >= a.x && atlas_x < a.x + a.w &&
                        atlas_y >= a.y && atlas_y < a.y + a.h) {
                        r.sprite_viewer_selected = sid;
                        break;
                    }
                }
            }
        }
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------
// Grid submenu layout (constants + height helper only). The full
// implementation lives further down; these are forward-hoisted so the
// Events panel can sit immediately below the Grid panel on the right
// edge without referencing forward-declared functions.
// ---------------------------------------------------------------------
static constexpr int kGridSubCount      = 7;
static constexpr int GRID_PANEL_W       = 150;
static constexpr int GRID_PANEL_PAD     = 4;
static constexpr int GRID_BUTTON_H      = 16;
static constexpr int GRID_BUTTON_GAP    = 2;
static constexpr int GRID_PANEL_TOP     = 120;  // below the overlay text

static int grid_panel_height() {
    return GRID_PANEL_PAD * 2 +
           kGridSubCount * (GRID_BUTTON_H + GRID_BUTTON_GAP) -
           GRID_BUTTON_GAP;
}

// ---------------------------------------------------------------------
// Events panel — right-side test triggers. Labels here match the
// EventId enum in game.h; Game::trigger_event dispatches each click.
// Keep the table dense enough that adding a new event is a one-line
// change in two places (this table + the enum + the dispatch).
// ---------------------------------------------------------------------
struct EventButton {
    const char* label;
};
static constexpr EventButton kEventButtons[] = {
    { "Spawn Triax"        },
    { "Spawn Maggot"       },
    { "Spawn Clawed Robot" },
    { "Flood (toggle)"     },
    { "Earthquake (toggle)"},
    { "Damage Player -10"  },
    { "Heal Player +0x40"  },
};
static constexpr int kEventCount =
    sizeof(kEventButtons) / sizeof(kEventButtons[0]);

static constexpr int EVENTS_PANEL_W      = 150;
static constexpr int EVENTS_PANEL_PAD    = 4;
static constexpr int EVENTS_BUTTON_H     = 16;
static constexpr int EVENTS_BUTTON_GAP   = 2;
static constexpr int EVENTS_PANEL_GAP    = 6;   // vertical gap below Grid panel

static int events_panel_x(const PixelRenderer& r) {
    return r.f.width - EVENTS_PANEL_W - EVENTS_PANEL_PAD;
}
// Events panel sits directly under the Grid submenu on the right edge.
// Grid panel always-allocates its slot at the top of the right column,
// so events_panel_top is offset by Grid's full height + gap whether or
// not the Grid checkbox is on (avoids the events panel jumping up and
// down as the user toggles Grid).
static int events_panel_top() {
    return GRID_PANEL_TOP + grid_panel_height() + EVENTS_PANEL_GAP;
}
static int events_button_y(int idx) {
    return events_panel_top() + EVENTS_PANEL_PAD +
           idx * (EVENTS_BUTTON_H + EVENTS_BUTTON_GAP);
}

void render_events_panel(PixelRenderer& r) {
    if (!r.events_panel_on) return;
    int x = events_panel_x(r);
    int y = events_panel_top();
    int h = EVENTS_PANEL_PAD * 2 +
            kEventCount * (EVENTS_BUTTON_H + EVENTS_BUTTON_GAP) -
            EVENTS_BUTTON_GAP;
    r.fill_rect  (x, y, EVENTS_PANEL_W, h, 0x101018);
    r.stroke_rect(x, y, EVENTS_PANEL_W, h, 0x4488cc);
    r.draw_text  (x + EVENTS_PANEL_PAD, y + 2,
                  "Test events", 0xcceeff, 0x101018);
    bool flashing = (r.event_flash_remaining > 0);
    for (int i = 0; i < kEventCount; i++) {
        int by = events_button_y(i);
        int bx = x + EVENTS_PANEL_PAD;
        int bw = EVENTS_PANEL_W - EVENTS_PANEL_PAD * 2;
        bool pressed = flashing && (i == r.last_event_clicked);
        uint32_t fill   = pressed ? 0x44cc44 : 0x222a33;
        uint32_t border = pressed ? 0xffffff : 0x4488cc;
        r.fill_rect  (bx, by, bw, EVENTS_BUTTON_H, fill);
        r.stroke_rect(bx, by, bw, EVENTS_BUTTON_H, border);
        r.draw_text  (bx + 4, by + (EVENTS_BUTTON_H - 8) / 2,
                      kEventButtons[i].label,
                      pressed ? 0x000000 : 0xffffff, fill);
    }
    // Tick the flash counter once per render-frame so the green pulse
    // fades automatically without needing a per-frame game callback.
    if (r.event_flash_remaining > 0) r.event_flash_remaining--;
}

// Write one line to Game's debug_log_ via the plumbed pointer. No-op if
// Game hasn't wired it yet — keeps tests and headless renderers safe.
static void debug_log_line(const PixelRenderer& r, const char* msg) {
    if (r.debug_log_) {
        *r.debug_log_ << msg << "\n";
        r.debug_log_->flush();
    }
}

// Called from consume_left_click — returns true if the click hit a
// panel button (so the caller doesn't fall through to tile-select).
static bool try_events_panel_click(PixelRenderer& r) {
    if (!r.events_panel_on) return false;
    int x = events_panel_x(r);
    int bw = EVENTS_PANEL_W - EVENTS_PANEL_PAD * 2;
    int bx = x + EVENTS_PANEL_PAD;
    char buf[160];
    std::snprintf(buf, sizeof(buf),
        "[events] click f=(%d,%d) panel=[%d..%d] width=%d on=%d",
        r.f.x, r.f.y, bx, bx + bw, r.f.width, r.events_panel_on ? 1 : 0);
    debug_log_line(r, buf);
    if (r.f.x < bx || r.f.x >= bx + bw) return false;
    for (int i = 0; i < kEventCount; i++) {
        int by = events_button_y(i);
        if (r.f.y >= by && r.f.y < by + EVENTS_BUTTON_H) {
            std::snprintf(buf, sizeof(buf), "[events] HIT button %d", i);
            debug_log_line(r, buf);
            r.has_pending_event_click = true;
            r.pending_event_id        = i;
            r.last_event_clicked      = i;
            r.event_flash_remaining   = 30;  // ~0.6s @ 50fps
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------
// Saves panel — left-side scrollable file browser. Game scans
// save_disks/ and pushes paths via set_save_files; this panel renders
// the list, tracks a highlight (mouse hover or keyboard), and posts a
// pending load on click / Enter.
// ---------------------------------------------------------------------
static constexpr int SAVES_PANEL_W       = 360;
static constexpr int SAVES_PANEL_PAD     = 6;
static constexpr int SAVES_ROW_H         = 14;
static constexpr int SAVES_PANEL_TOP     = 60;
static constexpr int SAVES_PANEL_BOTTOM_MARGIN = 24; // gap above HUD strip

static int saves_panel_left() { return 10; }
static int saves_panel_top()  { return SAVES_PANEL_TOP; }
static int saves_panel_height(const PixelRenderer& r) {
    int max = r.hud_y_px() - SAVES_PANEL_BOTTOM_MARGIN - SAVES_PANEL_TOP;
    if (max < SAVES_ROW_H * 4) max = SAVES_ROW_H * 4;
    return max;
}
static int saves_visible_rows(const PixelRenderer& r) {
    int header = SAVES_ROW_H + 4; // title row
    return (saves_panel_height(r) - SAVES_PANEL_PAD * 2 - header) / SAVES_ROW_H;
}
static int saves_first_row_y() {
    return saves_panel_top() + SAVES_PANEL_PAD + SAVES_ROW_H + 4;
}

void render_saves_panel(PixelRenderer& r) {
    if (!r.saves_panel_on) return;
    int x = saves_panel_left();
    int y = saves_panel_top();
    int w = SAVES_PANEL_W;
    int h = saves_panel_height(r);
    r.fill_rect  (x, y, w, h, 0x101820);
    r.stroke_rect(x, y, w, h, 0x66aaff);

    char title[80];
    int total = static_cast<int>(r.saves_list_.size());
    if (total == 0) {
        std::snprintf(title, sizeof(title), "Saves (no files in save_disks/)");
    } else {
        std::snprintf(title, sizeof(title),
                      "Saves  %d/%d   arrows + enter, click to load",
                      r.saves_highlight_ + 1, total);
    }
    r.draw_text(x + SAVES_PANEL_PAD, y + SAVES_PANEL_PAD,
                title, 0xcceeff, 0x101820);

    int rows = saves_visible_rows(r);
    if (rows < 1) return;
    if (r.saves_highlight_ < r.saves_scroll_) r.saves_scroll_ = r.saves_highlight_;
    if (r.saves_highlight_ >= r.saves_scroll_ + rows) {
        r.saves_scroll_ = r.saves_highlight_ - rows + 1;
    }
    if (r.saves_scroll_ < 0) r.saves_scroll_ = 0;

    int row_y = saves_first_row_y();
    for (int i = 0; i < rows; i++) {
        int idx = r.saves_scroll_ + i;
        if (idx >= total) break;
        int ry = row_y + i * SAVES_ROW_H;
        bool selected = (idx == r.saves_highlight_);
        uint32_t bg = selected ? 0x2a5577 : 0x101820;
        uint32_t fg = selected ? 0xffffff : 0xbbccdd;
        r.fill_rect(x + 2, ry, w - 4, SAVES_ROW_H, bg);
        // Strip the "save_disks/" prefix when present so the path fits;
        // truncate to the panel's interior width (8px / glyph) with an
        // ellipsis so long paths don't bleed past the right border.
        const std::string& full = r.saves_list_[idx];
        const char* label = full.c_str();
        if (full.rfind("save_disks/", 0) == 0) label += 11;
        else if (full.rfind("save_disks\\", 0) == 0) label += 11;
        constexpr int kMaxChars = (SAVES_PANEL_W - SAVES_PANEL_PAD * 2 - 8) / 8;
        char buf[96];
        int n = static_cast<int>(std::strlen(label));
        if (n <= kMaxChars) {
            std::snprintf(buf, sizeof(buf), "%s", label);
        } else {
            std::snprintf(buf, sizeof(buf), "...%s",
                          label + (n - (kMaxChars - 3)));
        }
        r.draw_text(x + SAVES_PANEL_PAD, ry + 3, buf, fg, bg);
    }

    // Scrollbar hint when overflow.
    if (total > rows) {
        int bar_x = x + w - 6;
        int bar_h = h - SAVES_PANEL_PAD * 2 - (SAVES_ROW_H + 4);
        int bar_y = saves_first_row_y();
        r.fill_rect(bar_x, bar_y, 2, bar_h, 0x223344);
        int thumb_h = std::max(8, bar_h * rows / total);
        int thumb_y = bar_y + (bar_h - thumb_h) * r.saves_scroll_ /
                      std::max(1, total - rows);
        r.fill_rect(bar_x, thumb_y, 2, thumb_h, 0x66aaff);
    }
}

// Hover: while the cursor is over a list row, move the highlight there
// so keyboard Enter loads the row under the cursor. Idempotent — does
// nothing when the panel is off or the cursor is outside the panel.
void saves_panel_hover(PixelRenderer& r) {
    if (!r.saves_panel_on) return;
    int x = saves_panel_left();
    int y = saves_panel_top();
    int w = SAVES_PANEL_W;
    int h = saves_panel_height(r);
    if (r.f.x < x || r.f.x >= x + w) return;
    if (r.f.y < y || r.f.y >= y + h) return;
    int row_y = saves_first_row_y();
    if (r.f.y < row_y) return;
    int rows  = saves_visible_rows(r);
    int local = (r.f.y - row_y) / SAVES_ROW_H;
    if (local < 0 || local >= rows) return;
    int idx = r.saves_scroll_ + local;
    if (idx < 0 || idx >= (int)r.saves_list_.size()) return;
    r.saves_highlight_ = idx;
}

// Returns true iff the click was inside the saves panel (so the caller
// doesn't fall through to a world click). Updates highlight on any
// in-panel click; posts a pending load when the click lands on a row.
static bool try_saves_panel_click(PixelRenderer& r) {
    if (!r.saves_panel_on) return false;
    int x = saves_panel_left();
    int y = saves_panel_top();
    int w = SAVES_PANEL_W;
    int h = saves_panel_height(r);
    if (r.f.x < x || r.f.x >= x + w) return false;
    if (r.f.y < y || r.f.y >= y + h) return false;

    int row_y = saves_first_row_y();
    int rows  = saves_visible_rows(r);
    if (r.f.y >= row_y && r.saves_highlight_ < (int)r.saves_list_.size()) {
        int local = (r.f.y - row_y) / SAVES_ROW_H;
        if (local >= 0 && local < rows) {
            int idx = r.saves_scroll_ + local;
            if (idx >= 0 && idx < (int)r.saves_list_.size()) {
                r.saves_highlight_ = idx;
                r.has_pending_save_load   = true;
                r.pending_save_load_path_ = r.saves_list_[idx];
            }
        }
    }
    return true; // swallow click regardless so it doesn't hit the world
}

// ---------------------------------------------------------------------
// Tiles submenu — right-side panel of click-to-toggle checkboxes that
// were lifted out of the main HUD strip. Visible only while the
// main-strip Tiles box is on. Mirrors the events-panel layout so users
// see one consistent "expanded panel" idiom.
// ---------------------------------------------------------------------
struct GridSubItem {
    const char* label;
    bool*       state;
};
static GridSubItem grid_sub_items(PixelRenderer& r, int i) {
    static const char* kLabels[kGridSubCount] = {
        "Map mode", "Rings", "Object lbl",
        "Switches", "Transports",
        "Tertiary", "Placed Tiles",
    };
    bool* states[kGridSubCount] = {
        &r.map_mode_on, &r.rings_on, &r.object_tiers_on,
        &r.switches_on, &r.transports_on,
        &r.tertiary_overlay_on, &r.placed_tiles_on,
    };
    return { kLabels[i], states[i] };
}

static int grid_panel_x(const PixelRenderer& r) {
    return r.f.width - GRID_PANEL_W - GRID_PANEL_PAD;
}
static int grid_button_y(int idx) {
    return GRID_PANEL_TOP + GRID_PANEL_PAD +
           idx * (GRID_BUTTON_H + GRID_BUTTON_GAP);
}

void render_grid_panel(PixelRenderer& r) {
    if (!r.tile_outline_on) return;
    int x = grid_panel_x(r);
    int y = GRID_PANEL_TOP;
    int h = grid_panel_height();
    r.fill_rect  (x, y, GRID_PANEL_W, h, 0x101018);
    r.stroke_rect(x, y, GRID_PANEL_W, h, 0x4488cc);
    r.draw_text  (x + GRID_PANEL_PAD, y + 2,
                  "Tiles", 0xcceeff, 0x101018);
    for (int i = 0; i < kGridSubCount; i++) {
        GridSubItem item = grid_sub_items(r, i);
        int by = grid_button_y(i);
        int bx = x + GRID_PANEL_PAD;
        int bw = GRID_PANEL_W - GRID_PANEL_PAD * 2;
        bool on = *item.state;
        uint32_t fill   = on ? 0x44cc44 : 0x222a33;
        uint32_t border = on ? 0xffffff : 0x4488cc;
        r.fill_rect  (bx, by, bw, GRID_BUTTON_H, fill);
        r.stroke_rect(bx, by, bw, GRID_BUTTON_H, border);
        r.draw_text  (bx + 4, by + (GRID_BUTTON_H - 8) / 2,
                      item.label, on ? 0x000000 : 0xffffff, fill);
    }
}

static bool try_grid_panel_click(PixelRenderer& r) {
    if (!r.tile_outline_on) return false;
    int x = grid_panel_x(r);
    int bw = GRID_PANEL_W - GRID_PANEL_PAD * 2;
    int bx = x + GRID_PANEL_PAD;
    if (r.f.x < bx || r.f.x >= bx + bw) return false;
    for (int i = 0; i < kGridSubCount; i++) {
        int by = grid_button_y(i);
        if (r.f.y >= by && r.f.y < by + GRID_BUTTON_H) {
            GridSubItem item = grid_sub_items(r, i);
            *item.state = !*item.state;
            return true;
        }
    }
    return false;
}

void render_overlay_text(PixelRenderer& r) {
    // Top-right corner overlay text — visible whenever Grid or Debug is
    // on. Grid alone draws the cell outlines + tile-tier swatches, and
    // having the tile-info banner alongside is almost always what you
    // want for "what is this tile?" inspection.
    if ((!r.debug_text_on && !r.tile_outline_on) || r.overlay.empty()) return;

    int line_count = 1;
    int max_line_w = 0;
    int cur_w = 0;
    for (char c : r.overlay) {
        if (c == '\n') {
            line_count++;
            max_line_w = std::max(max_line_w, cur_w);
            cur_w = 0;
        } else cur_w += 8;
    }
    max_line_w = std::max(max_line_w, cur_w);
    int pad = 4;
    int bx = r.f.width - max_line_w - pad * 2;
    // Shift down past the FPS readout when both are active so they
    // don't collide in the corner. FPS box = 9 px text + 2*pad + 2 gap.
    int by = r.fps_text_.empty() ? 2 : (2 + 9 + pad * 2 + 2);
    int bh = line_count * 9 + pad;
    r.fill_rect(bx, by, max_line_w + pad * 2, bh, 0x000000);
    for (int x = bx; x < bx + max_line_w + pad * 2; ++x) {
        r.put_pixel(x, by, 0x666666);
        r.put_pixel(x, by + bh - 1, 0x666666);
    }
    for (int y = by; y < by + bh; ++y) {
        r.put_pixel(bx, y, 0x666666);
        r.put_pixel(bx + max_line_w + pad * 2 - 1, y, 0x666666);
    }
    r.draw_text(bx + pad, by + pad, r.overlay.c_str(),
                0xFFFFFF, 0xFF000000);
}

void render_fps_text(PixelRenderer& r) {
    if (r.fps_text_.empty()) return;
    const int text_w = static_cast<int>(r.fps_text_.size()) * 8;
    const int pad = 4;
    const int bx = r.f.width - text_w - pad * 2;
    const int by = 2;
    const int bh = 9 + pad;
    r.fill_rect(bx, by, text_w + pad * 2, bh, 0x000000);
    for (int x = bx; x < bx + text_w + pad * 2; ++x) {
        r.put_pixel(x, by, 0x666666);
        r.put_pixel(x, by + bh - 1, 0x666666);
    }
    for (int y = by; y < by + bh; ++y) {
        r.put_pixel(bx, y, 0x666666);
        r.put_pixel(bx + text_w + pad * 2 - 1, y, 0x666666);
    }
    r.draw_text(bx + pad, by + pad, r.fps_text_.c_str(),
                0xFFFFFF, 0xFF000000);
}

void render_tile_overlay(PixelRenderer& r,
                         int sx, int sy,
                         uint8_t world_x, uint8_t world_y,
                         const TileRenderInfo& info) {
    if (!r.tile_outline_on) return;

    int tpx = r.tile_px_x();
    int tpy = r.tile_px_y();
    bool highlighted = r.has_highlight
                       && r.highlight_x == world_x
                       && r.highlight_y == world_y;
    // Tertiary visuals (magenta/yellow/hatches) gated on Tertiary
    // sub-toggle. Map-data colours (cyan/red) gated on Placed Tiles —
    // when off, those cells just render with the default grey grid.
    bool show_tert  = r.tertiary_overlay_on;
    bool show_place = r.placed_tiles_on;
    uint32_t map_data_rgb = info.map_data_aliased ? 0xCC3333 : 0x33CCCC;
    uint32_t base = (show_place && info.map_data_aliased)   ? 0xCC3333
                  : (show_tert  && info.is_switch)          ? 0xFFEE33
                  : (show_tert  && info.has_tertiary)       ? 0xCC44CC
                  : (show_place && info.from_map_data)      ? map_data_rgb
                                                            : 0x404040;
    if (show_tert && info.tertiary_source_aliased) {
        r.hatch_rect(sx, sy, tpx, tpy, 0xCC3333, 3);
    }
    if (show_tert && info.switch_x_aliased) {
        r.hatch_rect(sx, sy, tpx, tpy, 0xFFEE33, 3);
    }
    r.stroke_rect(sx, sy, tpx, tpy, base);
    if (highlighted) {
        r.stroke_rect(sx,     sy,     tpx,     tpy,     0xFFEE33);
        r.stroke_rect(sx + 1, sy + 1, tpx - 2, tpy - 2, 0xFFEE33);
    }

    // For map-data cells, label the cell with its slot in the
    // 1024-byte map_overlay_data table. Only worth showing when the
    // cell is big enough that the 8×8 font is legible.
    if (show_place && info.from_map_data && tpx >= 24 && tpy >= 12) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%u",
                      static_cast<unsigned>(info.map_data_offset));
        r.draw_text(sx + 2, sy + 2, buf, map_data_rgb, 0x000000);
    }
}

bool render_hud_panels(PixelRenderer& r) {
    int hud_y = r.hud_y_px();

    // Bottom-strip checkboxes. Map mode / Object lbl / Switches /
    // Transports / Rings / Tertiary / Placed Tiles all live in the
    // Tiles submenu (render_grid_panel) — keep this in sync with the
    // hit-test array in consume_left_click.
    r.fill_rect(0, hud_y, r.f.width, 16, 0x151515);
    DebugCheckbox boxes[11] = {
        { "Tiles",      &r.tile_outline_on   },
        { "Debug",      &r.debug_text_on     },
        { "Collision",  &r.collision_on      },
        { "Edit",       &r.editor_on         },
        { "Algo only",  &r.algo_only_on      },
        { "Sprites",    &r.sprite_viewer_on  },
        { "Health",     &r.health_bars_on    },
        { "Damage",     &r.damage_overlay_on },
        { "Mood",       &r.mood_overlay_on   },
        { "Events",     &r.events_panel_on   },
        { "Saves",      &r.saves_panel_on    },
    };
    int cy = checkbox_slot_y(hud_y);
    for (int i = 0; i < 11; i++) {
        int cx = checkbox_slot_x(i);
        uint32_t border = *boxes[i].state ? 0xffffff : 0x666666;
        r.stroke_rect(cx, cy, CHECKBOX_SIZE, CHECKBOX_SIZE, border);
        if (*boxes[i].state) {
            r.fill_rect(cx + 2, cy + 2,
                        CHECKBOX_SIZE - 4, CHECKBOX_SIZE - 4,
                        0x44CC44);
        }
        r.draw_text(cx + CHECKBOX_SIZE + CHECKBOX_LABEL_GAP,
                    cy + (CHECKBOX_SIZE - 8) / 2,
                    boxes[i].label, 0xdddddd, 0x000000);
    }

    // Sprite-viewer mode owns the whole window above the HUD strip,
    // so signal the caller to skip the inventory HUD.
    if (r.sprite_viewer_on) return true;

    if (!r.editor_on) return false;

    // Object palette panel (above the tile palette).
    {
        int op_x = palette_left_x();
        int op_y = obj_palette_top_y(hud_y);
        r.fill_rect  (op_x, op_y, OBJ_PALETTE_W, OBJ_PALETTE_H, 0x101010);
        r.stroke_rect(op_x, op_y, OBJ_PALETTE_W, OBJ_PALETTE_H, 0x444444);
        for (int i = 0; i < OBJECT_PALETTE_N; ++i) {
            int cx2 = obj_palette_cell_x(i);
            int cy2 = obj_palette_cell_y(i, hud_y);
            bool selected = (i == r.paint_object_idx);
            uint32_t border = selected ? 0xffffff : 0x555555;
            uint32_t fill   = selected ? 0x225522 : 0x000000;
            r.fill_rect  (cx2, cy2,
                          PALETTE_CELL_W - 1, PALETTE_CELL_H - 1, fill);
            uint8_t obj_type = OBJECT_PALETTE[i].object_type;
            uint8_t sprite_id = (obj_type < 0x65)
                ? object_types_sprite[obj_type] : 0xff;
            if (sprite_id != 0xff && sprite_id <= 0x80) {
                uint8_t pal_byte =
                    object_types_palette_and_pickup[obj_type] & 0x7f;
                uint32_t lut[4]; uint8_t fg[4];
                resolve_palette_with_fg(pal_byte,
                                        /*is_tile=*/false, lut, fg);
                r.blit_sprite_at_native(
                    cx2 + 1, cy2 + 1,
                    PALETTE_CELL_W - 3, PALETTE_CELL_H - 3,
                    sprite_id, lut, /*flip_h=*/false, /*flip_v=*/false);
            }
            r.stroke_rect(cx2, cy2,
                          PALETTE_CELL_W - 1, PALETTE_CELL_H - 1, border);
            r.draw_text(cx2 + 2, cy2 + PALETTE_CELL_H - 11,
                        OBJECT_PALETTE[i].label,
                        selected ? 0xffffff : 0xbbbbbb, 0x000000);
        }
    }

    // Tile palette panel + flip / detach buttons.
    int panel_x = palette_left_x();
    int panel_y = palette_top_y(hud_y);
    int panel_total_w = PALETTE_W + PALETTE_FLIP_GAP + PALETTE_FLIP_W +
                        PALETTE_PAD_X;
    r.fill_rect(panel_x, panel_y, panel_total_w, PALETTE_H, 0x101010);
    r.stroke_rect(panel_x, panel_y, panel_total_w, PALETTE_H, 0x444444);
    bool paint_flip_h = (r.paint_tile_ & 0x80) != 0;
    bool paint_flip_v = (r.paint_tile_ & 0x40) != 0;
    uint8_t paint_type = r.paint_tile_ & 0x3f;
    for (int t = 0; t < PALETTE_COLS * PALETTE_ROWS; ++t) {
        int cx_t = palette_cell_x(static_cast<uint8_t>(t));
        int cy_t = palette_cell_y(static_cast<uint8_t>(t), hud_y);
        bool selected = (t == paint_type);
        uint32_t border  = selected ? 0xffffff : 0x555555;
        uint32_t fill    = selected ? 0x225522 : 0x000000;
        r.fill_rect  (cx_t, cy_t,
                      PALETTE_CELL_W - 1, PALETTE_CELL_H - 1, fill);
        r.blit_tile_preview(cx_t + 1, cy_t + 1,
                            PALETTE_CELL_W - 3,
                            PALETTE_CELL_H - 3,
                            static_cast<uint8_t>(t),
                            selected && paint_flip_h,
                            selected && paint_flip_v);
        r.stroke_rect(cx_t, cy_t,
                      PALETTE_CELL_W - 1, PALETTE_CELL_H - 1, border);
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02x", t);
        r.draw_text(cx_t + PALETTE_CELL_W - 14,
                    cy_t + PALETTE_CELL_H - 11,
                    hex, 0xffffff, 0x000000);
    }

    struct PaletteBtn { const char* label; bool on; bool toggle; };
    PaletteBtn btns[3] = {
        { "FlipX",  paint_flip_h, true  },
        { "FlipY",  paint_flip_v, true  },
        { "Detach", false,        false },
    };
    for (int i = 0; i < 3; ++i) {
        int bx = palette_flip_x();
        int by = palette_flip_y(i, hud_y);
        uint32_t fill   = btns[i].on ? 0x225522 : 0x000000;
        uint32_t border = btns[i].on ? 0xffffff
                                      : (btns[i].toggle ? 0x555555
                                                        : 0x884444);
        r.fill_rect  (bx, by, PALETTE_FLIP_W, PALETTE_FLIP_H, fill);
        r.stroke_rect(bx, by, PALETTE_FLIP_W, PALETTE_FLIP_H, border);
        r.draw_text(bx + (PALETTE_FLIP_W - 36) / 2,
                    by + (PALETTE_FLIP_H - 8) / 2,
                    btns[i].label,
                    btns[i].on ? 0xffffff : 0xbbbbbb,
                    0x000000);
    }

    return false;
}

}  // namespace pr_debug

// =============================================================================
// PixelRenderer — debug-overlay member methods
// =============================================================================

void PixelRenderer::set_overlay_text(const char* text) {
    overlay = text ? text : "";
}

void PixelRenderer::set_fps_text(const char* text) {
    fps_text_ = text ? text : "";
}

void PixelRenderer::set_highlighted_tile(uint8_t world_x, uint8_t world_y) {
    has_highlight = true;
    highlight_x = world_x;
    highlight_y = world_y;
}

void PixelRenderer::render_activation_overlay(uint8_t anchor_x,
                                              uint8_t anchor_y) {
    if (!rings_on) return;

    // Ring radii taken from ObjectManager's lifecycle decisions:
    //   r = 1  — KEEP_AS_TERTIARY without KEEP_AS_PRIMARY_FOR_LONGER
    //   r = 4  — standard demotion / promotion distance
    //   r = 12 — KEEP_AS_PRIMARY_FOR_LONGER extended range
    struct Ring { int r; uint32_t rgb; const char* label; };
    static constexpr Ring RINGS[] = {
        { 1,  0xFF3333, "1" },
        { 4,  0xFFDD33, "4" },
        { 12, 0x33DD55, "12" },
    };

    int tpx = tile_px_x();
    int tpy = tile_px_y();
    int sx_anchor, sy_anchor;
    (void)world_to_screen(anchor_x, anchor_y, sx_anchor, sy_anchor);

    for (const Ring& ring : RINGS) {
        int w = (2 * ring.r + 1) * tpx;
        int h = (2 * ring.r + 1) * tpy;
        int x = sx_anchor - ring.r * tpx;
        int y = sy_anchor - ring.r * tpy;
        stroke_rect(x, y, w, h, ring.rgb);
        stroke_rect(x + 1, y + 1, w - 2, h - 2, ring.rgb);
        draw_text(x + 2, y + 2, ring.label, ring.rgb, 0x000000);
    }

    fill_rect(sx_anchor + tpx / 2 - 1, sy_anchor, 2, tpy, 0xFFFFFF);
    fill_rect(sx_anchor, sy_anchor + tpy / 2 - 1, tpx, 2, 0xFFFFFF);
}

bool PixelRenderer::tile_grid_enabled()    const { return tile_outline_on; }
bool PixelRenderer::object_tiers_enabled() const { return object_tiers_on; }
bool PixelRenderer::map_mode_enabled()     const { return map_mode_on;     }
bool PixelRenderer::switches_enabled()     const { return switches_on;     }
bool PixelRenderer::transports_enabled()   const { return transports_on;   }
bool PixelRenderer::algo_only_enabled()    const { return algo_only_on;    }
bool PixelRenderer::collision_enabled()    const { return collision_on;    }
bool PixelRenderer::editor_enabled()       const { return editor_on;       }
bool PixelRenderer::sprite_viewer_enabled() const { return sprite_viewer_on; }
bool PixelRenderer::health_bars_enabled()  const { return health_bars_on;   }
bool PixelRenderer::damage_overlay_enabled() const { return damage_overlay_on; }
bool PixelRenderer::mood_overlay_enabled()   const { return mood_overlay_on; }

// Atlas debug overlay (replaces world when "Sprites" is on): palette
// grid, spritesheet at 4x, and 8x detail panel with type mappings.
void PixelRenderer::render_sprite_viewer() {
    int hud_y = hud_y_px();
    fill_rect(0, 0, f.width, hud_y, 0x101018);

    int sel = sprite_viewer_selected;
    if (sel < 0 || sel > 0x80) sel = 0;
    const SpriteAtlasEntry& e = sprite_atlas[sel];
    const SpriteCompound* compound = sv_find_compound(sel);

    char header[256];
    int frame_idx = compound ? (sel - compound->base) : 0;
    if (compound) {
        std::snprintf(header, sizeof(header),
                      "Sprite 0x%02x   atlas=(%3u,%3u)   size=%2ux%-2u   "
                      "intrinsic_flip=%s%s   compound=%s frame %d/%d",
                      sel, e.x, e.y, e.w, e.h,
                      (e.intrinsic_flip & 1) ? "H" : "-",
                      (e.intrinsic_flip & 2) ? "V" : "-",
                      compound->name, frame_idx + 1, compound->count);
    } else {
        std::snprintf(header, sizeof(header),
                      "Sprite 0x%02x   atlas=(%3u,%3u)   size=%2ux%-2u   "
                      "intrinsic_flip=%s%s",
                      sel, e.x, e.y, e.w, e.h,
                      (e.intrinsic_flip & 1) ? "H" : "-",
                      (e.intrinsic_flip & 2) ? "V" : "-");
    }
    draw_text(SV_GRID_X, SV_HEADER_Y, header, 0xffffff, 0x101018);

    static constexpr uint32_t neutral_lut[4] =
        {0x000000, 0x404040, 0x888888, 0xeeeeee};
    uint32_t sprite_lut[125][4];
    bool     sprite_has_lut[125];
    for (int sid = 0; sid < 125; ++sid) {
        sprite_has_lut[sid] =
            sv_resolve_sprite_lut(sid, sprite_lut[sid]);
        if (!sprite_has_lut[sid]) {
            for (int k = 0; k < 4; ++k) sprite_lut[sid][k] = neutral_lut[k];
        }
    }

    static int8_t pixel_owner[81][128];
    for (int y = 0; y < 81; ++y) {
        for (int x = 0; x < 128; ++x) pixel_owner[y][x] = -1;
    }
    for (int sid = 0; sid < 125; ++sid) {
        const SpriteAtlasEntry& s = sprite_atlas[sid];
        for (int dy = 0; dy < s.h; ++dy) {
            int sy = s.y + dy;
            if (sy < 0 || sy >= 81) continue;
            for (int dx = 0; dx < s.w; ++dx) {
                int sx = s.x + dx;
                if (sx < 0 || sx >= 128) continue;
                pixel_owner[sy][sx] = static_cast<int8_t>(sid);
            }
        }
    }

    // --- 1. Palette grid -----------------------------------------------------
    fill_rect(SV_GRID_X - 4, SV_PANELS_Y - 4,
              SV_GRID_W + 8, SV_GRID_H + 8, 0x000000);
    for (int idx = 0; idx < SV_GRID_COLS * SV_GRID_ROWS; ++idx) {
        int sid = sv_grid_sprite_id(idx);
        int cx = sv_grid_cell_x(idx);
        int cy = sv_grid_cell_y(idx);
        if (sid < 0) {
            fill_rect(cx, cy, SV_GRID_CELL - 1, SV_GRID_CELL - 1, 0x080810);
            continue;
        }
        bool selected = (sid == sel);
        bool sibling = compound && !selected &&
                       sid >= compound->base &&
                       sid <  compound->base + compound->count;
        uint32_t fill_col = selected ? 0x223344
                                     : sibling ? 0x1a2228 : 0x101018;
        fill_rect(cx, cy, SV_GRID_CELL - 1, SV_GRID_CELL - 1, fill_col);
        blit_sprite_at_native(cx, cy, SV_GRID_CELL - 1, SV_GRID_CELL - 1,
                              static_cast<uint8_t>(sid),
                              sprite_lut[sid],
                              /*flip_h=*/false, /*flip_v=*/false);
        uint32_t border = selected ? 0xffee33
                                   : sibling ? 0x886633 : 0x444444;
        stroke_rect(cx, cy, SV_GRID_CELL - 1, SV_GRID_CELL - 1, border);
    }

    // --- 2. Full ROM spritesheet --------------------------------------------
    fill_rect(SV_SHEET_X - 4, SV_PANELS_Y - 4,
              SV_SHEET_W + 8, SV_SHEET_H + 8, 0x000000);
    for (int sy = 0; sy < SV_SHEET_H_ATLAS; ++sy) {
        for (int sx = 0; sx < SV_SHEET_W_ATLAS; ++sx) {
            uint8_t idx = bbc_sprite_pixel(sx, sy);
            int owner = pixel_owner[sy][sx];
            uint32_t col;
            if (owner >= 0) {
                col = sprite_lut[owner][idx];
                if (idx == 0) col = 0x101820;
            } else {
                col = neutral_lut[idx];
            }
            int dst_x = SV_SHEET_X + sx * SV_SHEET_SCALE;
            int dst_y = SV_PANELS_Y + sy * SV_SHEET_SCALE;
            fill_rect(dst_x, dst_y, SV_SHEET_SCALE, SV_SHEET_SCALE, col);
        }
    }

    int pix_x = sprite_viewer_pixel_x;
    int pix_y = sprite_viewer_pixel_y;
    bool pixel_mode = (pix_x >= 0 && pix_y >= 0 &&
                       pix_x < SV_SHEET_W_ATLAS &&
                       pix_y < SV_SHEET_H_ATLAS);
    int hl_ids[125];
    int hl_n = 0;
    if (pixel_mode) {
        for (int sid = 0; sid < 125; ++sid) {
            const SpriteAtlasEntry& a = sprite_atlas[sid];
            if (pix_x >= a.x && pix_x < a.x + a.w &&
                pix_y >= a.y && pix_y < a.y + a.h) {
                hl_ids[hl_n++] = sid;
            }
        }
    } else if (compound) {
        for (int i = 0; i < compound->count; ++i) {
            hl_ids[hl_n++] = compound->base + i;
        }
    } else {
        hl_ids[hl_n++] = sel;
    }
    for (int i = 0; i < hl_n; ++i) {
        int sid = hl_ids[i];
        if (sid < 0 || sid >= 125) continue;
        const SpriteAtlasEntry& s = sprite_atlas[sid];
        int rx = SV_SHEET_X + s.x * SV_SHEET_SCALE;
        int ry = SV_PANELS_Y + s.y * SV_SHEET_SCALE;
        int rw = s.w * SV_SHEET_SCALE;
        int rh = s.h * SV_SHEET_SCALE;
        bool is_sel = (sid == sel);
        uint32_t border = is_sel ? 0xffee33 : 0xaa6633;
        stroke_rect(rx, ry, rw, rh, border);
        if (is_sel) {
            stroke_rect(rx + 1, ry + 1, rw - 2, rh - 2, border);
        }
        char tag[8];
        if (!pixel_mode && compound) {
            std::snprintf(tag, sizeof(tag), "%d", sid - compound->base);
        } else {
            std::snprintf(tag, sizeof(tag), "%02x", sid);
        }
        draw_text(rx + 2, ry + 2, tag,
                  is_sel ? 0xffee33 : 0xddaa66, 0x000000);
    }
    if (pixel_mode) {
        int rx = SV_SHEET_X + pix_x * SV_SHEET_SCALE;
        int ry = SV_PANELS_Y + pix_y * SV_SHEET_SCALE;
        stroke_rect(rx, ry, SV_SHEET_SCALE, SV_SHEET_SCALE, 0xff3333);
    }

    char sheet_label[64];
    if (compound) {
        std::snprintf(sheet_label, sizeof(sheet_label),
                      "ROM sheet 128x81 @ %dx   compound=%s (%d frames)",
                      SV_SHEET_SCALE, compound->name, compound->count);
    } else {
        std::snprintf(sheet_label, sizeof(sheet_label),
                      "ROM sheet 128x81 @ %dx", SV_SHEET_SCALE);
    }
    draw_text(SV_SHEET_X, SV_PANELS_Y + SV_SHEET_H + 8,
              sheet_label, 0xbbbbbb, 0x101018);

    // --- 3. Detail panel ----------------------------------------------------
    constexpr int SV_DETAIL_SCALE_X = SV_DETAIL_SCALE * 2;
    int dx = SV_DETAIL_X;
    int dy = SV_PANELS_Y;
    fill_rect(dx - 4, dy - 4, SV_DETAIL_W + 8, hud_y - dy + 4 - 4, 0x000000);

    if (pixel_mode) {
        constexpr int SV_PIX_SCALE   = 3;
        constexpr int SV_PIX_SCALE_X = SV_PIX_SCALE * 2;
        constexpr int SV_PIX_GAP     = 8;
        constexpr int SV_PIX_LABEL_X = 16 * SV_PIX_SCALE_X + 12;
        char hdr[96];
        std::snprintf(hdr, sizeof(hdr),
                      "Sprites at pixel (%d, %d):  %d hit%s",
                      pix_x, pix_y, hl_n, hl_n == 1 ? "" : "s");
        draw_text(dx, dy, hdr, 0xffffff, 0x101018);
        int entry_y = dy + 16;
        if (hl_n == 0) {
            draw_text(dx, entry_y, "  (none — empty atlas)",
                      0x888888, 0x101018);
            return;
        }
        for (int i = 0; i < hl_n; ++i) {
            int sid = hl_ids[i];
            const SpriteAtlasEntry& a = sprite_atlas[sid];
            int sw = a.w * SV_PIX_SCALE_X;
            int sh = a.h * SV_PIX_SCALE;
            int row_h = std::max(sh, 22) + SV_PIX_GAP;
            if (entry_y + row_h > hud_y - 8) {
                char more[64];
                std::snprintf(more, sizeof(more),
                              "  +%d more (resize window to see)",
                              hl_n - i);
                draw_text(dx, entry_y, more, 0x888888, 0x101018);
                break;
            }
            for (int py = 0; py < a.h; ++py) {
                int src_y = a.y + py;
                for (int px = 0; px < a.w; ++px) {
                    int src_x = a.x + px;
                    uint8_t idx = bbc_sprite_pixel(src_x, src_y);
                    uint32_t col;
                    if (src_x == pix_x && src_y == pix_y) {
                        col = 0xff3333;
                    } else if (idx == 0) {
                        bool light = (((px >> 1) ^ (py >> 1)) & 1) != 0;
                        col = light ? 0x202028 : 0x101018;
                    } else {
                        col = sprite_lut[sid][idx];
                    }
                    int ox = dx + px * SV_PIX_SCALE_X;
                    int oy = entry_y + py * SV_PIX_SCALE;
                    fill_rect(ox, oy, SV_PIX_SCALE_X, SV_PIX_SCALE, col);
                }
            }
            uint32_t border = (sid == sel) ? 0xffee33 : 0x666666;
            stroke_rect(dx, entry_y, sw, sh, border);

            const char* owner_name = nullptr;
            char tile_buf[80];
            for (int t = 0; t < 0x65 && !owner_name; ++t) {
                if (object_types_sprite[t] == sid) {
                    owner_name = object_type_name(static_cast<ObjectType>(t));
                }
            }
            if (!owner_name) {
                for (int t = 0; t < 64; ++t) {
                    uint8_t entry = TILE_SPRITE_ID[t];
                    if (entry != 0xff && (entry & 0x7f) == sid) {
                        std::snprintf(tile_buf, sizeof(tile_buf), "%s",
                                      tile_type_name(static_cast<uint8_t>(t)));
                        owner_name = tile_buf;
                        break;
                    }
                }
            }
            const SpriteCompound* sc = sv_find_compound(sid);
            char line1[80];
            char line2[80];
            std::snprintf(line1, sizeof(line1), "%c0x%02x  %ux%u",
                          (sid == sel) ? '>' : ' ', sid, a.w, a.h);
            if (sc) {
                std::snprintf(line2, sizeof(line2), " %s frame %d",
                              sc->name, sid - sc->base);
            } else {
                std::snprintf(line2, sizeof(line2), " %s",
                              owner_name ? owner_name : "(unused)");
            }
            uint32_t text_col = (sid == sel) ? 0xffee33 : 0xddddee;
            draw_text(dx + SV_PIX_LABEL_X, entry_y,
                      line1, text_col, 0x101018);
            draw_text(dx + SV_PIX_LABEL_X, entry_y + 11,
                      line2, text_col, 0x101018);
            entry_y += row_h;
        }
        return;
    }

    int blit_w = e.w * SV_DETAIL_SCALE_X;
    int blit_h = e.h * SV_DETAIL_SCALE;
    for (int py = 0; py < e.h; ++py) {
        int src_y = e.y + py;
        for (int px = 0; px < e.w; ++px) {
            int src_x = e.x + px;
            uint8_t idx = bbc_sprite_pixel(src_x, src_y);
            uint32_t col;
            if (idx == 0) {
                bool light = (((px >> 1) ^ (py >> 1)) & 1) != 0;
                col = light ? 0x202028 : 0x101018;
            } else {
                col = sprite_lut[sel][idx];
            }
            int ox = dx + px * SV_DETAIL_SCALE_X;
            int oy = dy + py * SV_DETAIL_SCALE;
            fill_rect(ox, oy, SV_DETAIL_SCALE_X, SV_DETAIL_SCALE, col);
        }
    }
    stroke_rect(dx, dy, blit_w, blit_h, 0xffee33);

    int label_y = dy + blit_h + 16;

    // Animation frames beyond frame 0 aren't named in
    // object_types_sprite[]; search against the compound's BASE id so
    // every frame reports the same owners.
    int lookup_sid = compound ? compound->base : sel;
    if (compound && sel != compound->base) {
        char line[80];
        std::snprintf(line, sizeof(line), "Used by (via %s base 0x%02x):",
                      compound->name, compound->base);
        draw_text(dx, label_y, line, 0xffffff, 0x101018);
    } else {
        draw_text(dx, label_y, "Used by:", 0xffffff, 0x101018);
    }
    label_y += 12;
    int matches = 0;
    for (int t = 0; t < 0x65; ++t) {
        if (object_types_sprite[t] == lookup_sid) {
            char line[80];
            std::snprintf(line, sizeof(line), "  %s",
                          object_type_name(static_cast<ObjectType>(t)));
            draw_text(dx, label_y, line, 0xbbbbff, 0x101018);
            label_y += 11;
            matches++;
            if (label_y > hud_y - 60) break;
        }
    }
    for (int t = 0; t < 64; ++t) {
        uint8_t entry = TILE_SPRITE_ID[t];
        if (entry != 0xff && (entry & 0x7f) == lookup_sid) {
            char line[80];
            std::snprintf(line, sizeof(line), "  %s",
                          tile_type_name(static_cast<uint8_t>(t)));
            draw_text(dx, label_y, line, 0xbbffbb, 0x101018);
            label_y += 11;
            matches++;
            if (label_y > hud_y - 60) break;
        }
    }
    if (matches == 0) {
        if (compound) {
            char line[80];
            std::snprintf(line, sizeof(line),
                          "  (animation frame of %s)", compound->name);
            draw_text(dx, label_y, line, 0xddaa66, 0x101018);
        } else {
            draw_text(dx, label_y, "  (unused)", 0x888888, 0x101018);
        }
    }
}

void PixelRenderer::set_paint_tile(uint8_t tile_type) {
    paint_tile_ = tile_type;
}

bool PixelRenderer::consume_palette_click(uint8_t& tile_type) {
    if (!has_pending_palette_click) { tile_type = 0; return false; }
    tile_type = pending_palette_click_type;
    has_pending_palette_click = false;
    return true;
}

bool PixelRenderer::consume_event_click(int& event_id) {
    if (!has_pending_event_click) { event_id = 0; return false; }
    event_id = pending_event_id;
    has_pending_event_click = false;
    return true;
}

void PixelRenderer::set_save_files(const std::vector<std::string>& paths) {
    saves_list_ = paths;
    if (saves_highlight_ >= (int)saves_list_.size()) {
        saves_highlight_ = static_cast<int>(saves_list_.size()) - 1;
    }
    if (saves_highlight_ < 0) saves_highlight_ = 0;
}

bool PixelRenderer::consume_save_load_request(std::string& path) {
    if (!has_pending_save_load) { path.clear(); return false; }
    path = pending_save_load_path_;
    has_pending_save_load = false;
    pending_save_load_path_.clear();
    return true;
}

bool PixelRenderer::consume_palette_flip_click(int& which) {
    if (pending_palette_flip < 0) { which = -1; return false; }
    which = pending_palette_flip;
    pending_palette_flip = -1;
    return true;
}

bool PixelRenderer::consume_palette_detach_click() {
    if (!pending_detach_click) return false;
    pending_detach_click = false;
    return true;
}

void PixelRenderer::set_paint_object(int idx) {
    paint_object_idx = idx;
}

bool PixelRenderer::consume_object_palette_click(int& idx) {
    if (!has_pending_object_click) { idx = -1; return false; }
    idx = pending_object_click_idx;
    has_pending_object_click = false;
    return true;
}

uint8_t PixelRenderer::object_palette_type(int idx) const {
    if (idx < 0 || idx >= OBJECT_PALETTE_N) return 0;
    return OBJECT_PALETTE[idx].object_type;
}

void PixelRenderer::render_tile_shade_rect(uint8_t world_x, uint8_t world_y,
                                           uint8_t x_frac, uint8_t y_frac,
                                           uint8_t w_frac, uint8_t h_frac,
                                           uint32_t rgb) {
    int sx0, sy0;
    if (!world_to_screen(world_x, world_y, sx0, sy0,
                         x_frac, y_frac)) return;
    int tpx = tile_px_x();
    int tpy = tile_px_y();
    int w_px = (static_cast<int>(w_frac) * tpx + 128) / 256;
    int h_px = (static_cast<int>(h_frac) * tpy + 128) / 256;
    if (w_px < 1) w_px = 1;
    if (h_px < 1) h_px = 1;
    fill_rect(sx0, sy0, w_px, h_px, rgb);
}

// Bresenham between the centres of two world tiles, plus a small
// arrowhead at (x2, y2).
void PixelRenderer::render_wire(uint8_t x1, uint8_t y1,
                                uint8_t x2, uint8_t y2, uint32_t rgb) {
    int tpx = tile_px_x();
    int tpy = tile_px_y();
    int sx1, sy1, sx2, sy2;
    (void)world_to_screen(x1, y1, sx1, sy1);
    (void)world_to_screen(x2, y2, sx2, sy2);
    int cx1 = sx1 + tpx / 2;
    int cy1 = sy1 + tpy / 2;
    int cx2 = sx2 + tpx / 2;
    int cy2 = sy2 + tpy / 2;

    int hud_y = hud_y_px();
    int dx = std::abs(cx2 - cx1);
    int dy = -std::abs(cy2 - cy1);
    int sx_step = (cx1 < cx2) ? 1 : -1;
    int sy_step = (cy1 < cy2) ? 1 : -1;
    int err = dx + dy;
    int x = cx1, y = cy1;
    int budget = f.width + hud_y + 4;
    while (budget-- > 0) {
        if (x >= 0 && x < f.width && y >= 0 && y < hud_y) {
            put_pixel(x,     y,     rgb);
            put_pixel(x + 1, y,     rgb);
            put_pixel(x,     y + 1, rgb);
        }
        if (x == cx2 && y == cy2) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x += sx_step; }
        if (e2 <= dx) { err += dx; y += sy_step; }
    }

    int hs = 3;
    fill_rect(cx2 - hs, cy2 - hs, 2 * hs + 1, 2 * hs + 1, rgb);
}

// Damage overlay: floating amount per victim + ring per explosion.
// Circle = ~24 segments via fill_rect; numbers use the 8x8 font.
void PixelRenderer::render_damage_events(const std::vector<DamageVisual>& events) {
    if (!damage_overlay_on || events.empty()) return;
    int tpx = tile_px_x();
    int tpy = tile_px_y();
    int hud  = hud_y_px();
    constexpr uint32_t kRingRGB = 0xFF6633;   // orange
    constexpr uint32_t kTextRGB = 0xFFEE33;   // yellow
    // 32-step sine table covering [0, 2π) — cos(i) = sin(i + N/4 mod N).
    static constexpr int N = 32;
    static const int kSin[N] = {
            0,  1951,  3827,  5556,  7071,  8314,  9239,  9808,
        10000,  9808,  9239,  8314,  7071,  5556,  3827,  1951,
            0, -1951, -3827, -5556, -7071, -8314, -9239, -9808,
       -10000, -9808, -9239, -8314, -7071, -5556, -3827, -1951,
    };
    for (const DamageVisual& ev : events) {
        // Radius ring at the source — drawn first so numbers sit on top.
        if (ev.radius_tiles > 0) {
            int sx, sy;
            if (world_to_screen(ev.src_x, ev.src_y, sx, sy,
                                ev.src_x_frac, ev.src_y_frac)) {
                int cx = sx + tpx / 2;
                int cy = sy + tpy / 2;
                int rpx = std::min(tpx, tpy) * ev.radius_tiles;
                if (rpx < 1) rpx = 1;
                for (int i = 0; i < N; i++) {
                    int s = kSin[i];
                    int c = kSin[(i + N / 4) % N];
                    int px = cx + (rpx * c) / 10000;
                    int py = cy + (rpx * s) / 10000;
                    if (px >= 0 && px + 1 < f.width &&
                        py >= 0 && py + 1 < hud) {
                        fill_rect(px, py, 2, 2, kRingRGB);
                    }
                }
            }
        }
        // Damage number pinned just above the victim's tile, prefixed
        // with '-' so it reads as "energy lost". Stays at a fixed offset
        // for its TTL — TTL only controls fade-out timing now.
        if (ev.amount > 0) {
            int tx, ty;
            if (world_to_screen(ev.tgt_x, ev.tgt_y, tx, ty,
                                ev.tgt_x_frac, ev.tgt_y_frac)) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "-%u",
                              static_cast<unsigned>(ev.amount));
                draw_text(tx + tpx / 2 - 6, ty - 10,
                          buf, kTextRGB, 0x000000);
            }
        }
    }
}

void PixelRenderer::render_world_label(uint8_t world_x, uint8_t world_y,
                                        uint8_t x_frac, uint8_t y_frac,
                                        int pixel_dx, int pixel_dy,
                                        const char* text,
                                        uint32_t fg, uint32_t bg) {
    int sx, sy;
    if (!world_to_screen(world_x, world_y, sx, sy, x_frac, y_frac)) return;
    draw_text(sx + pixel_dx, sy + pixel_dy, text, fg, bg);
}

void PixelRenderer::render_aabb(Fixed8_8 world_x, Fixed8_8 world_y,
                                int w_units, int h_units, uint32_t rgb) {
    // w_units / h_units are in 1/256 of a tile (matches x.fraction /
    // y.fraction arithmetic).
    int sx0, sy0;
    if (!world_to_screen(world_x.whole, world_y.whole, sx0, sy0,
                         world_x.fraction, world_y.fraction)) return;
    int tpx = tile_px_x();
    int tpy = tile_px_y();
    int w_px = w_units * tpx / 256;
    int h_px = h_units * tpy / 256;
    if (w_px < 1) w_px = 1;
    if (h_px < 1) h_px = 1;
    stroke_rect(sx0, sy0, w_px, h_px, rgb);
    stroke_rect(sx0 + 1, sy0 + 1, w_px - 2, h_px - 2, rgb);
}

void PixelRenderer::render_debug_marker(uint8_t world_x, uint8_t world_y,
                                        uint32_t rgb, const char* label) {
    if (!object_tiers_on) return;
    int sx, sy;
    if (!world_to_screen(world_x, world_y, sx, sy)) return;
    int sz = 6 + scale;
    fill_rect(sx + 2, sy + 2, sz, sz, rgb);
    stroke_rect(sx + 2, sy + 2, sz, sz, 0x000000);
    if (label && *label) {
        draw_text(sx + 2 + sz + 2, sy + 2, label, 0xFFFFFF, 0x000000);
    }
}
