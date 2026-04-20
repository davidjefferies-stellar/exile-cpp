# Angles from velocities

How Exile converts an `(velocity_x, velocity_y)` pair into an 8-bit angle byte,
and how bullets use that byte to pick a sprite and its flip flags.

The C++ port lives in `src/ai/behaviors/projectile.cpp`
(`calculate_angle_from_velocities`, `orient_bullet_to_angle`). This document
describes the 6502 original and the porting decisions.

## Angle byte convention

A single 8-bit value encodes direction-of-motion with 256 steps around a full
turn:

| Byte   | Direction | `(vx, vy)` |
| ------ | --------- | ---------- |
| `0x00` | right     | `(+, 0)`   |
| `0x40` | down      | `(0, +)`   |
| `0x80` | left      | `(-, 0)`   |
| `0xC0` | up        | `(0, -)`   |

`+y` is "down" because screen/world coordinates grow downwards.

Two useful properties of this encoding that the bullet code relies on:

- **Bit 7 set** — the angle is in `0x80..0xFF`, i.e. the bullet is moving *up*
  (into the upper half of the tile grid). Used directly as the vertical-flip
  flag.
- **Bit 6 set, after a conditional EOR** — distinguishes the half-quadrant that
  maps onto the "flipped" copies of the six base sprites. Used as the
  horizontal-flip flag.

## The 6502 recipe (`&22cc calculate_angle_from_this_object_velocities`)

### Step 1: absolute values + sign tracking

`get_absolute_vector_components` (`&233d`) is called for y first, then x. Each
call takes `|component|` by:

- `CMP #&7f` vs the value. Carry is set iff the value is `0x00..0x7F`, i.e.
  non-negative in the 6502's signed-byte view.
- If negative, `EOR #&ff / ADC #&01` negates.
- `ROL &99 ; vector_signs` rotates the carry (1 = "was positive") into bit 0
  of a bit-accumulator.

After both calls, `vector_signs` has:

| Bit | Meaning       |
| --- | ------------- |
| 0   | `vx >= 0`     |
| 1   | `vy >= 0`     |

Edge case: abs of `-128` (`0x80`) doesn't fit in a signed byte; the 6502
leaves it as unsigned `0x80`. For typical game velocities (diamond-clamped to
`±0x20`) this never matters.

### Step 2: swap so smallest is in A

```
CMP &b7 ; absolute_vector_y       ; carry set iff abs_x >= abs_y
BCC skip_swap
  TAY
  LDA &b7
  STY &b7
skip_swap:
ROL &99 ; vector_signs             ; bit 0 = abs_x >= abs_y
```

After this, `A = min(abs_x, abs_y)`, `&b7 = max(abs_x, abs_y)`, and
`vector_signs` low three bits are:

| Bit | Meaning                |
| --- | ---------------------- |
| 0   | `abs_x >= abs_y`       |
| 1   | `vx >= 0`              |
| 2   | `vy >= 0`              |

This 3-bit value selects one of the 8 octants.

### Step 3: divide min by max

A classic shift-and-subtract division, but with a twist — there's no separate
loop counter. Instead the initial quotient byte is pre-loaded with a `0x08`
sentinel bit; the loop exits when that bit has shifted off the top:

```
LDY #&08
STY &b5 ; angle

division_loop:
  ASL A
  CMP &b7 ; magnitude
  BCC skip_sub
  SBC &b7
skip_sub:
  ROL &b5 ; angle
  BCC division_loop            ; exit once the sentinel rolls out of bit 7
```

The sentinel lives in bit 3 of the initial `0x08`, which takes 5 ROLs to
reach bit 7 and fall off. So the loop runs 5 times and produces a 5-bit
quotient of `min/max` in the low 5 bits of `&b5`. That's `tan(θ)` where
`θ ∈ [0°, 45°]`, discretised into 32 steps.

### Step 4: steer the quotient into the correct octant

```
LDA &99 ; vector_signs
AND #&07
TAY
LDA &b5 ; angle
EOR &14bf,Y ; angle_calculation_half_quadrants_table
STA &b5 ; angle
```

The table at `&14bf` is 8 bytes long (one per octant):

```
bf 80 c0 ff 40 7f 3f 00
```

The EOR both *rotates* the quotient into the right region of the 0..255 angle
space and *mirrors* it where the quotient runs "backwards" within its
octant. It's the reason a +x/+y vector (down-right) comes out near `0x20`
while a -x/-y vector (up-left) comes out near `0xa0`, even though both use
the same raw min/max ratio.

### Porting note: ASL + CMP across 8 bits

`ASL A` followed by `CMP B` in the 6502 loses the shifted-out bit, but the
subsequent `SBC B` uses carry from `CMP` as borrow and *implicitly* extends
`A` to 9 bits. For small velocities it's fine to drop the extension, but to
keep the port byte-exact I do the compare-and-subtract in 16 bits:

```cpp
uint16_t A16 = uint16_t(A) << 1;
bool cmp_carry = (A16 >= B);
A = cmp_carry ? uint8_t(A16 - B) : uint8_t(A16 & 0xff);
```

## Bullet sprite selection (`&4447-&4460`)

Once the angle byte is in `A`, `move_bullet` derives three things from it:
the vertical flip, the horizontal flip, and one of six sprite offsets.

### Vertical flip

```
&444a STA &39 ; this_object_y_flip (bullet angle)
```

The angle byte *is* the y-flip byte. The plot code reads bit 7, so:

- bit 7 = 1 → angle in `0x80..0xFF` → moving up → flip vertically
- bit 7 = 0 → angle in `0x00..0x7F` → moving down → no v-flip

### Horizontal flip (and prep for sprite index)

```
&444c BIT &39
&444e BVC &4452            ; V flag = bit 6 of &39
&4450 EOR #&ff             ; invert if bit 6 was set
&4452 STA &37 ; this_object_x_flip
```

The angle gets inverted when bit 6 is set (i.e. when the angle is in the
second half of either the down-going or up-going hemisphere —
`0x40..0x7F` or `0xC0..0xFF`). That conditional inversion mirrors the 16
half-quadrants back onto 8 sprite-sized buckets, with the x-flip handed out
for free in the process.

After the conditional EOR, `x_flip`'s bit 7 gives the horizontal flip flag.

### Sprite index

```
&4454 AND #&7f
&4456 LSR A / LSR A / LSR A   ; A /= 8  →  0..15
&4459 CMP #&04
&445b BCC to_change_object_sprite_to_base_plus_A
&445d LSR A
&445e EOR #&06
&4460 JMP change_object_sprite_to_base_plus_A
```

16 buckets of 22.5°, folded down to 6:

| Bucket | Sprite offset | Sprite name                |
| ------ | ------------- | -------------------------- |
| 0      | 0             | `SPRITE_BULLET_HORIZONTAL` |
| 1      | 1             | `SPRITE_BULLET_TWENTY_TWO` |
| 2      | 2             | `SPRITE_BULLET_FORTY_FIVE` |
| 3      | 3             | `SPRITE_BULLET_SIXTY`      |
| 4      | 4             | `SPRITE_BULLET_SEVENTY_FIVE` |
| 5      | 4             | `SPRITE_BULLET_SEVENTY_FIVE` |
| 6      | 5             | `SPRITE_BULLET_VERTICAL`   |
| 7      | 5             | `SPRITE_BULLET_VERTICAL`   |
| 8, 9   | 2             | `SPRITE_BULLET_FORTY_FIVE` |
| 10, 11 | 3             | `SPRITE_BULLET_SIXTY`      |
| 12, 13 | 0             | `SPRITE_BULLET_HORIZONTAL` |
| 14, 15 | 1             | `SPRITE_BULLET_TWENTY_TWO` |

`change_object_sprite_to_base_plus_A` (`&3292`) adds the offset to the type's
base sprite (from `object_types_sprite`). For bullets the base is `0x08` =
`SPRITE_BULLET_HORIZONTAL`, so offsets `0..5` land on sprite ids `0x08..0x0d`.

Some non-bullet projectiles (cannonball, blue death ball — base `0x21`
`SPRITE_BALL`) also go through `move_bullet` in the 6502, but the resulting
`base + offset` would pick non-sense sprites for them. The C++ port guards the
sprite write with `if (base == 0x08)` so only the bullet-strip types get their
sprite rotated.

### Quirk: 45° maps to the 60° sprite

A vector like `(vx=+10, vy=-10)` comes out with angle byte `0xe0`. The bucket
math gives:

- EOR via bit 6 → `0x1f`
- AND `0x7f` → `0x1f`
- `>> 3` → `3`
- `3 < 4`, so offset `3` = `SPRITE_BULLET_SIXTY`

So the sprite shown for "exactly 45°" is the 60° artwork, not the 45° artwork.
That's the 6502's behaviour; the port reproduces it.

## Worked examples

### Straight right: `(vx=+10, vy=0)`

1. `abs_x = 10`, `abs_y = 0`, `vector_signs bits = { vx_pos=1, vy_pos=1 }`
2. `abs_x >= abs_y` → swap → `A = 0`, `B = 10`; `vector_signs bit 0 = 1`
3. Index = `0b111 = 7`; table entry = `0x00`
4. Divide: `0 / 10 → 0`
5. Angle = `0x00 ^ 0x00 = 0x00` ✓

### Straight up: `(vx=0, vy=-10)`

1. `abs_x = 0`, `abs_y = 10`, `vector_signs bits = { vx_pos=1, vy_pos=0 }`
2. `abs_x < abs_y` → no swap; `vector_signs bit 0 = 0`
3. Index = `0b010 = 2`; table entry = `0xc0`
4. Divide: `0 / 10 → 0`
5. Angle = `0x00 ^ 0xc0 = 0xc0` ✓

### 45° up-right: `(vx=+10, vy=-10)`

1. `abs_x = abs_y = 10`, `vector_signs = { vx_pos=1, vy_pos=0 }`
2. `abs_x >= abs_y` → swap → `A = 10`, `B = 10`; `vector_signs bit 0 = 1`
3. Index = `0b011 = 3`; table entry = `0xff`
4. Divide: `10 / 10 → 0x1f` (all five bits set)
5. Angle = `0x1f ^ 0xff = 0xe0`

Sprite recipe:

- `v-flip = 1` (bit 7 of `0xe0`)
- `BIT` bit 6 set → `EOR #&ff` → `x_flip = 0x1f`
- `h-flip = 0` (bit 7 of `0x1f`)
- `(0x1f & 0x7f) >> 3 = 3` → offset `3` → `SPRITE_BULLET_SIXTY`

Final: sprite `0x08 + 3 = 0x0b` (BULLET_SIXTY), flipped vertically.

### Shallow up-right: `(vx=+10, vy=-5)`

1. `abs_x = 10`, `abs_y = 5`, `vx_pos = 1`, `vy_pos = 0`
2. `abs_x >= abs_y` → swap → `A = 5`, `B = 10`; bit 0 = 1
3. Index = `0b011 = 3`; table entry = `0xff`
4. Divide: `5 / 10 → 0x10`
5. Angle = `0x10 ^ 0xff = 0xef`

Sprite recipe:

- `v-flip = 1`
- `BIT` bit 6 of `0xef` is 1 → EOR → `x_flip = 0x10`
- `h-flip = 0`
- `(0x10 & 0x7f) >> 3 = 2` → offset `2` → `SPRITE_BULLET_FORTY_FIVE`

So the shallower the up-right angle, the "more horizontal" the sprite gets —
which is the intuitively correct behaviour for a bullet arcing low.
