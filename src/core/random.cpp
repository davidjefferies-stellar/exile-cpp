#include "core/random.h"

Random::Random() {
    // Initial state - the original game's state after decryption
    state_[0] = 0x00; // &d9
    state_[1] = 0x00; // &da
    state_[2] = 0x00; // &db
    state_[3] = 0x00; // &dc
    carry_ = 0;
}

void Random::seed(uint8_t s0, uint8_t s1, uint8_t s2, uint8_t s3) {
    state_[0] = s0;
    state_[1] = s1;
    state_[2] = s2;
    state_[3] = s3;
    carry_ = 0;
}

// Port of &2587:
//   ADC &dc (rnd_state+3)    ; A += state[3] + carry
//   ADC &d9 (rnd_state+0)    ; A += state[0] + carry
//   STA &d9                  ; state[0] = A
//   ADC &db (rnd_state+2)    ; A += state[2] + carry
//   STA &db                  ; state[2] = A
//   ADC &da (rnd_state+1)    ; A += state[1] + carry
//   STA &da                  ; state[1] = A
//   ADC &dc (rnd_state+3)    ; A += state[3] + carry
//   STA &dc                  ; state[3] = A
//   RTS                      ; return A
uint8_t Random::next() {
    uint16_t a = carry_; // carry from previous operation

    // ADC &dc
    a += state_[3];
    carry_ = static_cast<uint8_t>(a >> 8);
    a = static_cast<uint8_t>(a);

    // ADC &d9
    a += state_[0] + carry_;
    carry_ = static_cast<uint8_t>(a >> 8);
    a = static_cast<uint8_t>(a);
    state_[0] = static_cast<uint8_t>(a);

    // ADC &db
    a += state_[2] + carry_;
    carry_ = static_cast<uint8_t>(a >> 8);
    a = static_cast<uint8_t>(a);
    state_[2] = static_cast<uint8_t>(a);

    // ADC &da
    a += state_[1] + carry_;
    carry_ = static_cast<uint8_t>(a >> 8);
    a = static_cast<uint8_t>(a);
    state_[1] = static_cast<uint8_t>(a);

    // ADC &dc
    a += state_[3] + carry_;
    carry_ = static_cast<uint8_t>(a >> 8);
    a = static_cast<uint8_t>(a);
    state_[3] = static_cast<uint8_t>(a);

    return static_cast<uint8_t>(a);
}
