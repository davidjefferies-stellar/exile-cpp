#include "rendering/ascii/ascii_renderer.h"
#include <algorithm>

// Color pair IDs (1-8, 0 is default)
enum ColorPairs {
    CP_DEFAULT = 0,
    CP_RED     = 1,
    CP_GREEN   = 2,
    CP_YELLOW  = 3,
    CP_BLUE    = 4,
    CP_MAGENTA = 5,
    CP_CYAN    = 6,
    CP_WHITE   = 7,
    CP_RED_ON_BLACK = 8,
};

AsciiRenderer::~AsciiRenderer() {
    shutdown();
}

bool AsciiRenderer::init() {
    if (initialized_) return true;

    initscr();
    if (has_colors()) {
        start_color();
        init_color_pairs();
    }
    cbreak();
    noecho();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    curs_set(0); // Hide cursor

    getmaxyx(stdscr, term_height_, term_width_);

    initialized_ = true;
    return true;
}

void AsciiRenderer::shutdown() {
    if (initialized_) {
        endwin();
        initialized_ = false;
    }
}

void AsciiRenderer::init_color_pairs() {
    init_pair(CP_RED,     COLOR_RED,     COLOR_BLACK);
    init_pair(CP_GREEN,   COLOR_GREEN,   COLOR_BLACK);
    init_pair(CP_YELLOW,  COLOR_YELLOW,  COLOR_BLACK);
    init_pair(CP_BLUE,    COLOR_BLUE,    COLOR_BLACK);
    init_pair(CP_MAGENTA, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(CP_CYAN,    COLOR_CYAN,    COLOR_BLACK);
    init_pair(CP_WHITE,   COLOR_WHITE,   COLOR_BLACK);
    init_pair(CP_RED_ON_BLACK, COLOR_RED, COLOR_BLACK);
}

void AsciiRenderer::begin_frame() {
    erase();
}

void AsciiRenderer::end_frame() {
    refresh();
}

void AsciiRenderer::set_viewport(uint8_t center_x, uint8_t center_y,
                                  uint8_t /*frac_x*/, uint8_t /*frac_y*/) {
    vp_center_x_ = center_x;
    vp_center_y_ = center_y;
}

int AsciiRenderer::viewport_width_tiles() const {
    return term_width_;
}

int AsciiRenderer::viewport_height_tiles() const {
    return term_height_ - 2; // Reserve 2 rows for HUD
}

bool AsciiRenderer::world_to_screen(uint8_t wx, uint8_t wy, int& sx, int& sy) const {
    int vp_w = viewport_width_tiles();
    int vp_h = viewport_height_tiles();

    // Signed distance from viewport center (handling 256-wrapping)
    int dx = static_cast<int8_t>(wx - vp_center_x_);
    int dy = static_cast<int8_t>(wy - vp_center_y_);

    sx = vp_w / 2 + dx;
    sy = vp_h / 2 + dy;

    return sx >= 0 && sx < vp_w && sy >= 0 && sy < vp_h;
}

char AsciiRenderer::tile_to_char(uint8_t tile_type, bool flip_h, bool flip_v) {
    // Tile type is the low 6 bits
    uint8_t t = tile_type & 0x3f;
    switch (t) {
        case 0x19: return ' ';  // TILE_SPACE
        case 0x2d: return '#';  // TILE_EARTH
        case 0x1e: return '%';  // TILE_STONE_TWO
        case 0x12: return '%';  // TILE_STONE_ONE
        case 0x2e: return (flip_h != flip_v) ? '\\' : '/'; // TILE_EARTH_SLOPE_FORTY_FIVE
        case 0x2f: return (flip_h != flip_v) ? '\\' : '/'; // TILE_EARTH_SLOPE_TWENTY_TWO_ONE
        case 0x23: return (flip_h != flip_v) ? '\\' : '/'; // TILE_STONE_SLOPE_FORTY_FIVE
        case 0x0f: return flip_v ? 'v' : '^'; // TILE_MUSHROOMS
        case 0x1b: return 'T';  // TILE_TALL_BUSH
        case 0x1a: return 't';  // TILE_SHORT_BUSH
        case 0x09: return 'O';  // TILE_NEST
        case 0x0a: return 'P';  // TILE_PIPE
        case 0x0e: return '~';  // TILE_VARIABLE_WIND
        case 0x0b: return '=';  // TILE_CONSTANT_WIND
        case 0x0d: return '~';  // TILE_WATER
        case 0x0c: return '&';  // TILE_ENGINE
        case 0x21: return '|';  // TILE_COLUMN
        case 0x08: return ':';  // TILE_CHECK_TERTIARY (vertical shaft)
        case 0x2b: return '_';  // TILE_EARTH_EDGE
        case 0x2c: return '-';  // TILE_EARTH_HORIZONTAL_QUARTER_WITH_EDGE
        case 0x10: return '-';  // TILE_GREEN_HORIZONTAL_QUARTER
        case 0x13: return '/';  // TILE_STONE_SLOPE_FORTY_FIVE_FULL
        case 0x24: return '/';  // TILE_STONE_SLOPE_TWENTY_TWO_ONE
        default:
            // Unknown tile - use fill character if likely solid
            if (t == 0x00) return ':';  // Tertiary object check ranges
            return '?';
    }
}

int AsciiRenderer::tile_to_color(uint8_t tile_type) {
    uint8_t t = tile_type & 0x3f;
    switch (t) {
        case 0x19: return CP_DEFAULT;    // Space
        case 0x2d: return CP_YELLOW;     // Earth
        case 0x1e: case 0x12: return CP_WHITE; // Stone
        case 0x2e: case 0x2f: return CP_YELLOW; // Earth slopes
        case 0x23: case 0x13: case 0x24: return CP_WHITE; // Stone slopes
        case 0x0f: return CP_RED;        // Mushrooms
        case 0x1b: case 0x1a: return CP_GREEN; // Bushes
        case 0x09: return CP_CYAN;       // Nest
        case 0x0a: return CP_CYAN;       // Pipe
        case 0x0e: return CP_CYAN;       // Wind
        case 0x0d: return CP_BLUE;       // Water
        case 0x0c: return CP_RED;        // Engine
        default:   return CP_WHITE;
    }
}

char AsciiRenderer::object_to_char(ObjectType type) {
    switch (type) {
        case ObjectType::PLAYER:                  return '@';
        case ObjectType::ACTIVE_CHATTER:          return 'c';
        case ObjectType::CREW_MEMBER:             return 'C';
        case ObjectType::FLUFFY:                  return 'f';
        case ObjectType::RED_FROGMAN:
        case ObjectType::GREEN_FROGMAN:
        case ObjectType::INVISIBLE_FROGMAN:       return 'F';
        case ObjectType::RED_SLIME:
        case ObjectType::GREEN_SLIME:
        case ObjectType::YELLOW_SLIME:            return 's';
        case ObjectType::WORM:
        case ObjectType::MAGGOT:                  return 'w';
        case ObjectType::PIRANHA:                 return 'p';
        case ObjectType::WASP:                    return 'W';
        case ObjectType::BIG_FISH:                return 'F';
        case ObjectType::RED_MAGENTA_IMP:
        case ObjectType::RED_YELLOW_IMP:
        case ObjectType::BLUE_CYAN_IMP:
        case ObjectType::CYAN_YELLOW_IMP:
        case ObjectType::RED_CYAN_IMP:            return 'i';
        case ObjectType::GREEN_YELLOW_BIRD:
        case ObjectType::WHITE_YELLOW_BIRD:
        case ObjectType::RED_MAGENTA_BIRD:
        case ObjectType::INVISIBLE_BIRD:          return 'b';
        case ObjectType::MAGENTA_ROLLING_ROBOT:
        case ObjectType::RED_ROLLING_ROBOT:
        case ObjectType::BLUE_ROLLING_ROBOT:      return 'R';
        case ObjectType::HOVERING_ROBOT:          return 'H';
        case ObjectType::MAGENTA_CLAWED_ROBOT:
        case ObjectType::CYAN_CLAWED_ROBOT:
        case ObjectType::GREEN_CLAWED_ROBOT:
        case ObjectType::RED_CLAWED_ROBOT:        return 'X';
        case ObjectType::GREEN_WHITE_TURRET:
        case ObjectType::CYAN_RED_TURRET:         return 'T';
        case ObjectType::TRIAX:                   return 'Z';
        case ObjectType::GARGOYLE:                return 'G';
        case ObjectType::ICER_BULLET:
        case ObjectType::TRACER_BULLET:
        case ObjectType::RED_BULLET:
        case ObjectType::PISTOL_BULLET:           return '.';
        case ObjectType::PLASMA_BALL:
        case ObjectType::CANNONBALL:
        case ObjectType::BLUE_DEATH_BALL:         return 'o';
        case ObjectType::ACTIVE_GRENADE:
        case ObjectType::INACTIVE_GRENADE:        return '*';
        case ObjectType::EXPLOSION:               return '#';
        case ObjectType::FIREBALL:
        case ObjectType::MOVING_FIREBALL:
        case ObjectType::ENGINE_FIRE:             return '~';
        case ObjectType::HORIZONTAL_METAL_DOOR:
        case ObjectType::HORIZONTAL_STONE_DOOR:   return '-';
        case ObjectType::VERTICAL_METAL_DOOR:
        case ObjectType::VERTICAL_STONE_DOOR:     return '|';
        case ObjectType::SWITCH:                  return '!';
        case ObjectType::BOULDER:
        case ObjectType::CORONIUM_BOULDER:        return 'O';
        case ObjectType::CYAN_YELLOW_GREEN_KEY:
        case ObjectType::RED_YELLOW_GREEN_KEY:
        case ObjectType::GREEN_YELLOW_RED_KEY:
        case ObjectType::YELLOW_WHITE_RED_KEY:
        case ObjectType::RED_MAGENTA_RED_KEY:
        case ObjectType::BLUE_CYAN_GREEN_KEY:     return 'k';
        case ObjectType::PISTOL:
        case ObjectType::ICER:
        case ObjectType::BLASTER:
        case ObjectType::PLASMA_GUN:
        case ObjectType::ALIEN_WEAPON:            return 'g';
        case ObjectType::POWER_POD:               return '+';
        case ObjectType::DESTINATOR:              return 'D';
        case ObjectType::PROTECTION_SUIT:         return 'S';
        case ObjectType::JETPACK_BOOSTER:         return 'J';
        default:                                  return '?';
    }
}

int AsciiRenderer::object_to_color(ObjectType type) {
    switch (type) {
        case ObjectType::PLAYER:                  return CP_YELLOW;
        case ObjectType::TRIAX:                   return CP_GREEN;
        case ObjectType::RED_FROGMAN:             return CP_RED;
        case ObjectType::GREEN_FROGMAN:           return CP_GREEN;
        case ObjectType::RED_SLIME:               return CP_RED;
        case ObjectType::GREEN_SLIME:             return CP_GREEN;
        case ObjectType::YELLOW_SLIME:            return CP_YELLOW;
        case ObjectType::MAGENTA_CLAWED_ROBOT:    return CP_MAGENTA;
        case ObjectType::CYAN_CLAWED_ROBOT:       return CP_CYAN;
        case ObjectType::GREEN_CLAWED_ROBOT:      return CP_GREEN;
        case ObjectType::RED_CLAWED_ROBOT:        return CP_RED;
        case ObjectType::EXPLOSION:               return CP_RED;
        case ObjectType::FIREBALL:
        case ObjectType::MOVING_FIREBALL:         return CP_RED;
        case ObjectType::DESTINATOR:              return CP_MAGENTA;
        case ObjectType::POWER_POD:               return CP_CYAN;
        default:                                  return CP_WHITE;
    }
}

void AsciiRenderer::render_tile(uint8_t world_x, uint8_t world_y,
                                 const TileRenderInfo& info) {
    int sx, sy;
    if (!world_to_screen(world_x, world_y, sx, sy)) return;

    char ch = tile_to_char(info.tile_type, info.flip_h, info.flip_v);
    int color = tile_to_color(info.tile_type);

    if (color != CP_DEFAULT) attron(COLOR_PAIR(color));
    mvaddch(sy, sx, ch);
    if (color != CP_DEFAULT) attroff(COLOR_PAIR(color));
}

void AsciiRenderer::render_object(Fixed8_8 world_x, Fixed8_8 world_y,
                                   const SpriteRenderInfo& info) {
    if (!info.visible) return;

    int sx, sy;
    if (!world_to_screen(world_x.whole, world_y.whole, sx, sy)) return;

    char ch = object_to_char(info.type);
    int color = object_to_color(info.type);

    if (color != CP_DEFAULT) attron(COLOR_PAIR(color) | A_BOLD);
    mvaddch(sy, sx, ch);
    if (color != CP_DEFAULT) attroff(COLOR_PAIR(color) | A_BOLD);
}

void AsciiRenderer::render_hud(const PlayerState& player) {
    int hud_y = term_height_ - 2;

    // Energy bar
    attron(COLOR_PAIR(CP_GREEN));
    mvprintw(hud_y, 0, "Energy:");
    int bar_len = (player.energy * 20) / 255;
    for (int i = 0; i < 20; i++) {
        addch(i < bar_len ? '=' : ' ');
    }
    attroff(COLOR_PAIR(CP_GREEN));

    // Weapon
    attron(COLOR_PAIR(CP_CYAN));
    const char* weapon_names[] = {"Jetpack", "Pistol", "Icer", "Blaster", "Plasma", "Alien"};
    int wi = player.weapon;
    if (wi > 5) wi = 0;
    mvprintw(hud_y, 30, "Weapon: %s", weapon_names[wi]);
    attroff(COLOR_PAIR(CP_CYAN));

    // Keys
    attron(COLOR_PAIR(CP_YELLOW));
    mvprintw(hud_y + 1, 0, "Keys: %02X", player.keys_collected);
    attroff(COLOR_PAIR(CP_YELLOW));

    // Controls help
    attron(COLOR_PAIR(CP_WHITE));
    mvprintw(hud_y + 1, 30, "Arrows:move Z:jet X:fire Q:quit");
    attroff(COLOR_PAIR(CP_WHITE));
}

int AsciiRenderer::get_key() {
    return getch();
}
