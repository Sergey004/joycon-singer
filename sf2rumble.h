#pragma once
// ============================================================
//  sf2rumble.h — TinySoundFont → Joy-Con HD Rumble bridge
//
//  Идея: рендерим MIDI через SF2 в стерео PCM на низкой частоте
//  (8000 Hz), потом анализируем L/R каналы — извлекаем частоту
//  (zero-crossing) и амплитуду (RMS) — и отдаём в моторы.
//
//  Все 16 MIDI-каналов обрабатываются автоматически.
//  Панорама SF2 инструментов → распределение L/R по моторам.
//
//  Зависимости: tsf.h (TinySoundFont, single-header)
//    https://github.com/schellingb/TinySoundFont
// ============================================================

#define TSF_IMPLEMENTATION
#include "tsf.h"

// Рабочая частота дискретизации.
// 8000 Hz: достаточно для 626 Hz (Найквист = 4000 Hz),
//          в 5.5x меньше нагрузки чем 44100 Hz.
#define SF2R_RATE      8000

// Размер одного рендер-фрейма (= 1 тик главного цикла = 8ms).
// 8000 * 0.008 = 64 сэмпла
#define SF2R_FRAME     64

// Размер окна анализа = 2 фрейма (128 сэмплов = 16ms).
// Даёт минимальную детектируемую частоту: 1/0.016 = 62.5 Hz
// — покрывает нижний предел Joy-Con (41 Hz).
#define SF2R_WINDOW    (SF2R_FRAME * 2)

// Минимальная RMS при которой считаем что "звук есть"
// (фильтр тишины / шума SF2-синтеза)
#define SF2R_NOISE_FLOOR  0.002f

struct Sf2Rumble {

    tsf   *sf      = nullptr;
    bool   ready   = false;

    // Накопительный буфер (последние 2 фрейма) для анализа
    float  win_L[SF2R_WINDOW] = {};
    float  win_R[SF2R_WINDOW] = {};

    // Последние стабильные значения (держим пока нет звука)
    float  last_freq_L = 160.f,  last_freq_R = 160.f;
    float  last_amp_L  = 0.f,    last_amp_R  = 0.f;

    // ---- Загрузка SF2 ----

    bool load(const char *path) {
        if (sf) { tsf_close(sf); sf = nullptr; }
        ready = false;

        sf = tsf_load_filename(path);
        if (!sf) return false;

        // Стерео, 8000 Hz, небольшой гейн (-6 dB чтобы не клипать)
        tsf_set_output(sf, TSF_STEREO_INTERLEAVED, SF2R_RATE, -6.0f);

        // Percussion на канале 9 (GM стандарт)
        tsf_channel_set_bank_preset(sf, 9, 128, 0);

        ready = true;
        return true;
    }

    void unload() {
        if (sf) { tsf_close(sf); sf = nullptr; }
        ready = false;
    }

    // ---- MIDI события ----

    void note_on(int channel, int note, int velocity) {
        if (!ready) return;
        tsf_channel_note_on(sf, channel, note, velocity / 127.f);
    }

    void note_off(int channel, int note) {
        if (!ready) return;
        tsf_channel_note_off(sf, channel, note);
    }

    void program_change(int channel, int program) {
        if (!ready) return;
        tsf_channel_set_presetnumber(sf, channel, program,
                                     channel == 9 ? 1 : 0);
    }

    void control_change(int channel, int cc, int val) {
        if (!ready) return;
        tsf_channel_midi_control(sf, channel, cc, val);
    }

    void pitch_bend(int channel, int bend) {
        if (!ready) return;
        tsf_channel_set_pitchwheel(sf, channel, bend);
    }

    void all_notes_off() {
        if (!ready) return;
        for (int ch = 0; ch < 16; ch++)
            tsf_channel_note_off_all(sf, ch);
    }

    // ---- Рендер + анализ ----
    // Вызывать каждые 8ms.
    // Заполняет out_freq_L/R и out_amp_L/R.

    void tick(float *out_freq_L, float *out_amp_L,
              float *out_freq_R, float *out_amp_R)
    {
        if (!ready) {
            *out_freq_L = 160.f; *out_amp_L = 0.f;
            *out_freq_R = 160.f; *out_amp_R = 0.f;
            return;
        }

        // Временный interleaved буфер [L0,R0, L1,R1, ...]
        float interleaved[SF2R_FRAME * 2];
        tsf_render_float(sf, interleaved, SF2R_FRAME, 0);

        // Сдвигаем окно влево (старый фрейм уходит)
        for (int i = 0; i < SF2R_FRAME; i++) {
            win_L[i] = win_L[i + SF2R_FRAME];
            win_R[i] = win_R[i + SF2R_FRAME];
        }
        // Заполняем правую половину новыми сэмплами
        for (int i = 0; i < SF2R_FRAME; i++) {
            win_L[SF2R_FRAME + i] = interleaved[i * 2];
            win_R[SF2R_FRAME + i] = interleaved[i * 2 + 1];
        }

        // Анализируем
        float rms_L = _rms(win_L, SF2R_WINDOW);
        float rms_R = _rms(win_R, SF2R_WINDOW);

        if (rms_L > SF2R_NOISE_FLOOR) {
            last_freq_L = _zero_cross_freq(win_L, SF2R_WINDOW);
            last_amp_L  = _rms_to_amp(rms_L);
        } else {
            // Плавное затухание вместо резкого обрыва
            last_amp_L *= 0.7f;
            if (last_amp_L < 0.005f) last_amp_L = 0.f;
        }

        if (rms_R > SF2R_NOISE_FLOOR) {
            last_freq_R = _zero_cross_freq(win_R, SF2R_WINDOW);
            last_amp_R  = _rms_to_amp(rms_R);
        } else {
            last_amp_R *= 0.7f;
            if (last_amp_R < 0.005f) last_amp_R = 0.f;
        }

        *out_freq_L = last_freq_L;
        *out_amp_L  = last_amp_L;
        *out_freq_R = last_freq_R;
        *out_amp_R  = last_amp_R;
    }

    // ---- Анализ PCM ----

    // Zero-crossing rate → частота.
    // Для Joy-Con достаточно точно на чистых тонах.
    // На аккордах даёт "среднюю" частоту — нормально для моторов.
    static float _zero_cross_freq(const float *buf, int n) {
        // Убираем DC offset
        float dc = 0.f;
        for (int i = 0; i < n; i++) dc += buf[i];
        dc /= n;

        int crossings = 0;
        bool prev_pos = (buf[0] - dc) >= 0.f;
        for (int i = 1; i < n; i++) {
            bool cur_pos = (buf[i] - dc) >= 0.f;
            if (cur_pos != prev_pos) crossings++;
            prev_pos = cur_pos;
        }

        // crossings / 2 = количество полных периодов
        float dur = (float)n / SF2R_RATE;
        float freq = (crossings * 0.5f) / dur;

        // Clamp + octave fold в диапазон Joy-Con
        if (freq < 1.f) return 160.f;  // DC / тишина → нейтраль
        while (freq < 41.f)  freq *= 2.f;
        while (freq > 626.f) freq *= 0.5f;
        return freq;
    }

    // RMS амплитуды буфера
    static float _rms(const float *buf, int n) {
        float s = 0.f;
        for (int i = 0; i < n; i++) s += buf[i] * buf[i];
        return sqrtf(s / n);
    }

    // RMS → амплитуда мотора.
    // TSF выдаёт значения ~0..1. Масштабируем в 0..0.8 (safe max).
    // Логарифмическая кривая — человек слышит громкость логарифмически.
    static float _rms_to_amp(float rms) {
        if (rms < SF2R_NOISE_FLOOR) return 0.f;
        // log-scale: amp = (log(rms) - log(noise)) / (log(1) - log(noise))
        float log_noise = logf(SF2R_NOISE_FLOOR);
        float log_rms   = logf(rms < SF2R_NOISE_FLOOR ?
                               SF2R_NOISE_FLOOR : rms);
        float t = (log_rms - log_noise) / (0.f - log_noise);
        if (t < 0.f) t = 0.f;
        if (t > 1.f) t = 1.f;
        return t * 0.8f;
    }
};
