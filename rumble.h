#pragma once
#include <stdint.h>
#include <math.h>

// ============================================================
//  Joy-Con HD Rumble encoding — improved
// ============================================================

// Neutral / silence packet (320 Hz, 0 amplitude)
#define RUMBLE_NEUTRAL { 0x00, 0x01, 0x40, 0x40 }

#define RUMBLE_FREQ_MIN   41.0f
#define RUMBLE_FREQ_MAX  626.0f
#define RUMBLE_AMP_MAX     0.8f
#define RUMBLE_AMP_MIN   0.012f  // FIX: было 0 (dead zone до 0.12)

// ----------------------------------------------------------
// FIX 1: Octave folding вместо hard clamp
// Ноты вне 41-626 Hz сдвигаются на октавы вместо обрезки.
// Сохраняет музыкальное соотношение нот.
// ----------------------------------------------------------
// LF диапазон: 41–626 Hz  (byte 0x01–0x7F)
// HF диапазон: 82–1252 Hz (byte 0x0400–0xFC01)
// encode_rumble использует LF как основу, но send_vibration
// в main_switch.cpp отправляет HF отдельно.
// Здесь fold только для LF — ноты выше 626 складываем вниз.
static inline float fold_frequency(float freq) {
    if (freq <= 0.0f) return RUMBLE_FREQ_MIN;
    while (freq < RUMBLE_FREQ_MIN) freq *= 2.0f;
    while (freq > RUMBLE_FREQ_MAX) freq *= 0.5f;
    return freq;
}

// Для HF канала — другая граница (82–1252 Hz).
// Если 3-я гармоника в диапазоне — используем её (square wave).
// Если нет — октава вниз пока не влезет.
static inline float fold_hf_frequency(float fund_freq) {
    float h3 = fund_freq * 3.0f;
    if (h3 >= 82.0f && h3 <= 1252.0f) return h3;
    // fallback: октава от фундаментала
    float f = fund_freq * 2.0f;
    while (f < 82.0f)   f *= 2.0f;
    while (f > 1252.0f) f *= 0.5f;
    return f;
}

// Амплитуда HF гармоники:
// 3-я гармоника квадратной волны = 1/3 амплитуды.
// Если fallback на октаву — 0.5 (менее резкий тембр, но лучше чем ничего).
static inline float hf_amp_for(float fund_freq, float amp) {
    float h3 = fund_freq * 3.0f;
    return (h3 >= 82.0f && h3 <= 1252.0f) ? amp * 0.333f : amp * 0.5f;
}

// ----------------------------------------------------------
// FIX 2: Логарифмическая кривая velocity → amplitude
// Человек слышит громкость логарифмически (закон Вебера-Фехнера).
// gamma=2.2 даёт хорошее приближение к реальным инструментам.
// ----------------------------------------------------------
static inline float velocity_to_amp(int velocity, float max_amp) {
    if (velocity <= 0)   return 0.0f;
    if (velocity >= 127) return max_amp;
    float v = velocity / 127.0f;
    return powf(v, 1.7f) * max_amp;  // 1.7: баланс между linear и log
}

// ----------------------------------------------------------
// FIX 3: Decay envelope — плавное затухание при note-off
// Вызывать каждый тик после note-off, уменьшает amp * factor.
// Предотвращает резкий клик при обрыве вибрации.
//   decay_ms: время до нуля в миллисекундах (20-60ms хорошо)
//   dt_ms:    прошедшее время с последнего вызова (обычно 5-8ms)
// ----------------------------------------------------------
static inline float rumble_decay(float amp, float decay_ms, float dt_ms) {
    if (amp <= 0.001f) return 0.0f;
    float factor = 1.0f - (dt_ms / decay_ms);
    if (factor < 0.0f) factor = 0.0f;
    return amp * factor;
}

// ----------------------------------------------------------
// FIX 4: Encode — исправлен dead zone (0 → 0.12 теперь рабочий)
// Оригинал: порог if (amp > 0.12f) → тихие ноты (pp, mp)
//           были полностью немые.
// Исправление: порог снижен до 0.012f (минимум из таблицы dekuNukem)
// ----------------------------------------------------------
static inline void encode_rumble(float freq_hz, float amp, uint8_t out[4]) {
    // Используем fold вместо clamp
    freq_hz = fold_frequency(freq_hz);

    if (amp < 0.0f)           amp = 0.0f;
    if (amp > RUMBLE_AMP_MAX) amp = RUMBLE_AMP_MAX;

    // Frequency encoding (log2-based, dekuNukem table)
    uint8_t enc_freq = (uint8_t)roundf(log2f(freq_hz / 10.0f) * 32.0f);
    uint16_t hf = (uint16_t)((enc_freq - 0x60) * 4);
    uint8_t  lf = enc_freq - 0x40;

    // Amplitude encoding — FIX: порог 0.012f вместо 0.12f
    uint8_t enc_amp = 0;
    if (amp > 0.23f)
        enc_amp = (uint8_t)roundf(log2f(amp * 8.7f)  * 32.0f);
    else if (amp > 0.012f)  // <-- было 0.12f, теперь 0.012f
        enc_amp = (uint8_t)roundf(log2f(amp * 17.0f) * 16.0f);

    uint16_t hf_amp = (uint16_t)(enc_amp * 2);
    uint8_t  lf_amp = (uint8_t)((enc_amp >> 1) + 0x40);

    out[0] = (uint8_t)(hf & 0xFF);
    out[1] = (uint8_t)(((hf >> 8) & 0xFF) | (hf_amp & 0xFF));
    out[2] = lf | (uint8_t)((lf_amp << 7) & 0x80);
    out[3] = lf_amp >> 1;
}

static inline void encode_rumble_stop(uint8_t out[4]) {
    out[0] = 0x00; out[1] = 0x01; out[2] = 0x40; out[3] = 0x40;
}

static inline float midi_note_to_freq(int note) {
    return 440.0f * powf(2.0f, (note - 69) / 12.0f);
}
