#pragma once
#include <stdint.h>
#include <math.h>

// ============================================================
//  Joy-Con HD Rumble encoding
//  Source: dekuNukem/Nintendo_Switch_Reverse_Engineering
//          rumble_data_table.md
// ============================================================

// Neutral / silence packet
#define RUMBLE_NEUTRAL { 0x00, 0x01, 0x40, 0x40 }

// Frequency range for LF band
#define RUMBLE_FREQ_MIN  41.0f
#define RUMBLE_FREQ_MAX 626.0f

// Safe amplitude max to avoid damaging the LRA motor
#define RUMBLE_AMP_MAX   0.8f

// ----------------------------------------
// Encode a frequency (Hz) + amplitude (0..1)
// into the 4-byte Joy-Con rumble packet.
//
// Packet layout (same for left and right):
//   Byte 0: HF low byte
//   Byte 1: HF high byte | HF amplitude low byte
//   Byte 2: LF byte      | LF amplitude bit 7
//   Byte 3: LF amplitude >> 1
// ----------------------------------------
static inline void encode_rumble(float freq_hz, float amp, uint8_t out[4]) {
    // Clamp inputs
    if (freq_hz < RUMBLE_FREQ_MIN) freq_hz = RUMBLE_FREQ_MIN;
    if (freq_hz > RUMBLE_FREQ_MAX) freq_hz = RUMBLE_FREQ_MAX;
    if (amp < 0.0f)           amp = 0.0f;
    if (amp > RUMBLE_AMP_MAX) amp = RUMBLE_AMP_MAX;

    // --- Frequency encoding ---
    // Joy-Con uses a log2-based encoding
    uint8_t enc_freq = (uint8_t)roundf(log2f(freq_hz / 10.0f) * 32.0f);

    // HF range: 0x0004..0x01FC, steps +0x0004
    uint16_t hf = (uint16_t)((enc_freq - 0x60) * 4);
    // LF range: 0x01..0x7F
    uint8_t  lf = enc_freq - 0x40;

    // --- Amplitude encoding ---
    uint8_t enc_amp = 0;
    if (amp > 0.23f)
        enc_amp = (uint8_t)roundf(log2f(amp * 8.7f)  * 32.0f);
    else if (amp > 0.12f)
        enc_amp = (uint8_t)roundf(log2f(amp * 17.0f) * 16.0f);
    // else amp ~0: enc_amp stays 0

    uint16_t hf_amp = (uint16_t)(enc_amp * 2);
    uint8_t  lf_amp = (uint8_t)((enc_amp >> 1) + 0x40);

    // Pack into 4 bytes
    out[0] = (uint8_t)(hf & 0xFF);
    out[1] = (uint8_t)(((hf >> 8) & 0xFF) | (hf_amp & 0xFF));
    out[2] = lf | (uint8_t)((lf_amp << 7) & 0x80);
    out[3] = lf_amp >> 1;
}

// Silence (stop rumble)
static inline void encode_rumble_stop(uint8_t out[4]) {
    // Neutral: 320 Hz, 0 amplitude
    out[0] = 0x00;
    out[1] = 0x01;
    out[2] = 0x40;
    out[3] = 0x40;
}

// Convert MIDI note number to frequency in Hz
// Middle C (MIDI 60) = 261.63 Hz
static inline float midi_note_to_freq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}
