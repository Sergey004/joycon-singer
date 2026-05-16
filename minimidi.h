#pragma once
// ============================================================
//  miniMidi — improved: tempo map, sustain pedal, bug fixes
// ============================================================

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// ---- Data structures ----

typedef struct {
    int     channel;
    int     note;
    int     velocity;   // 0 = note off
    double  time_sec;
} MidiNote;

typedef struct {
    MidiNote *notes;
    int       count;
    int       capacity;
    double    total_duration;
} MidiSong;

// FIX 4: Tempo map для корректного Type 1 MIDI
// В Type 1 все треки играют одновременно с одной картой темпа.
// Оригинал передавал tempo_us_out между треками — для Type 1
// это неверно: темп трека 0 применялся ко всему треку 1 целиком,
// игнорируя момент изменения темпа.
typedef struct {
    uint32_t tick;
    double   tempo_us;
} TempoPoint;

typedef struct {
    TempoPoint *pts;
    int         count;
    int         capacity;
    int         ticks_per_qn;
} TempoMap;

static void _tmap_push(TempoMap *m, uint32_t tick, double tempo_us) {
    if (m->count >= m->capacity) {
        m->capacity = m->capacity ? m->capacity * 2 : 16;
        m->pts = (TempoPoint *)realloc(m->pts, m->capacity * sizeof(TempoPoint));
    }
    m->pts[m->count++] = (TempoPoint){ tick, tempo_us };
}

static double _tmap_tick_to_sec(const TempoMap *m, uint32_t tick) {
    double   time     = 0.0;
    uint32_t last_t   = 0;
    double   last_bpm = 500000.0;
    for (int i = 0; i < m->count; i++) {
        if (m->pts[i].tick >= tick) break;
        uint32_t dt = m->pts[i].tick - last_t;
        time   += (dt * last_bpm) / ((double)m->ticks_per_qn * 1e6);
        last_t  = m->pts[i].tick;
        last_bpm = m->pts[i].tempo_us;
    }
    time += ((tick - last_t) * last_bpm) / ((double)m->ticks_per_qn * 1e6);
    return time;
}

// ---- Internal helpers ----

static uint32_t _midi_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|(uint32_t)p[3];
}
static uint16_t _midi_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0]<<8)|p[1]);
}
static uint32_t _midi_vlq(const uint8_t *d, int *pos, int len) {
    uint32_t v = 0;
    for (int i = 0; i < 4 && *pos < len; i++) {
        uint8_t b = d[(*pos)++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return v;
}

static void _midi_push(MidiSong *s, MidiNote n) {
    if (s->count >= s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 256;
        s->notes = (MidiNote *)realloc(s->notes, s->capacity * sizeof(MidiNote));
    }
    s->notes[s->count++] = n;
}

static int _midi_cmp(const void *a, const void *b) {
    double d = ((const MidiNote*)a)->time_sec - ((const MidiNote*)b)->time_sec;
    return d < 0 ? -1 : d > 0 ? 1 : 0;
}

// ---- Pass 1: build tempo map from ALL tracks ----

static void _build_tempo_map(const uint8_t *buf, int buf_len,
                              int num_tracks, int offset0,
                              TempoMap *tmap) {
    int offset = offset0;
    for (int t = 0; t < num_tracks; t++) {
        if (offset + 8 > buf_len) break;
        if (memcmp(buf + offset, "MTrk", 4) != 0) break;
        uint32_t track_len = _midi_be32(buf + offset + 4);
        offset += 8;
        if (offset + (int)track_len > buf_len) break;

        const uint8_t *data = buf + offset;
        int   data_len = (int)track_len;
        int   pos = 0;
        uint32_t abs_tick = 0;
        uint8_t  running = 0;

        while (pos < data_len) {
            uint32_t delta = _midi_vlq(data, &pos, data_len);
            abs_tick += delta;
            if (pos >= data_len) break;

            uint8_t status = data[pos];
            if (status == 0xFF) {
                pos++;
                if (pos >= data_len) break;
                uint8_t mtype = data[pos++];
                uint32_t mlen = _midi_vlq(data, &pos, data_len);
                if (mtype == 0x51 && mlen == 3 && pos + 3 <= data_len) {
                    double new_tempo = (double)(
                        ((uint32_t)data[pos]<<16)|
                        ((uint32_t)data[pos+1]<<8)|
                        (uint32_t)data[pos+2]);
                    _tmap_push(tmap, abs_tick, new_tempo);
                }
                pos += (int)mlen;
                running = 0;  // FIX: мета-события сбрасывают running status
                continue;
            }
            if (status == 0xF0 || status == 0xF7) {
                pos++;
                uint32_t slen = _midi_vlq(data, &pos, data_len);
                pos += (int)slen;
                running = 0;
                continue;
            }
            uint8_t ev = (status & 0x80) ? (running = status, pos++, status) : running;
            uint8_t type = ev & 0xF0;
            if (type == 0x90 || type == 0x80 || type == 0xA0 || type == 0xB0 || type == 0xE0) pos += 2;
            else if (type == 0xC0 || type == 0xD0) pos += 1;
            else pos++;
        }
        offset += (int)track_len;
    }
}

// ---- Pass 2: parse notes using tempo map ----

// FIX 5: Sustain pedal (CC 64)
// Когда педаль нажата — note-off откладывается до её отпускания.
// Критично для фортепианных MIDI-файлов.
typedef struct { int note; int channel; } _PendingOff;

static void _parse_notes(const uint8_t *data, int data_len,
                         const TempoMap *tmap, MidiSong *song,
                         // sustain state per channel (16 каналов)
                         uint8_t sustain[16], _PendingOff *pending,
                         int *pending_count, int pending_cap)
{
    int      pos     = 0;
    uint32_t abs_tick = 0;
    uint8_t  running = 0;

    while (pos < data_len) {
        uint32_t delta = _midi_vlq(data, &pos, data_len);
        abs_tick += delta;
        if (pos >= data_len) break;

        double time_sec = _tmap_tick_to_sec(tmap, abs_tick);
        uint8_t status = data[pos];

        if (status == 0xFF) {
            pos++;
            if (pos >= data_len) break;
            pos++;  // meta type
            uint32_t mlen = _midi_vlq(data, &pos, data_len);
            pos += (int)mlen;
            running = 0;
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {
            pos++;
            uint32_t slen = _midi_vlq(data, &pos, data_len);
            pos += (int)slen;
            running = 0;
            continue;
        }

        uint8_t ev;
        if (status & 0x80) { ev = status; pos++; running = status; }
        else                { ev = running; }

        uint8_t type    = ev & 0xF0;
        uint8_t channel = ev & 0x0F;

        if (type == 0x90 || type == 0x80) {
            if (pos + 1 >= data_len) break;
            uint8_t note = data[pos++];
            uint8_t vel  = data[pos++];
            int is_off   = (type == 0x80) || (vel == 0);

            if (!is_off) {
                MidiNote mn = { channel, note, vel, time_sec };
                _midi_push(song, mn);
            } else {
                // FIX 5: если педаль нажата — откладываем note-off
                if (sustain[channel] && *pending_count < pending_cap) {
                    pending[(*pending_count)++] = (_PendingOff){ note, channel };
                } else {
                    MidiNote mn = { channel, note, 0, time_sec };
                    _midi_push(song, mn);
                }
            }

        } else if (type == 0xB0) {
            if (pos + 1 >= data_len) break;
            uint8_t cc  = data[pos++];
            uint8_t val = data[pos++];

            if (cc == 64) {  // Sustain pedal
                if (val >= 64) {
                    sustain[channel] = 1;
                } else {
                    sustain[channel] = 0;
                    // Сбрасываем отложенные note-off для этого канала
                    int i = 0;
                    while (i < *pending_count) {
                        if (pending[i].channel == (int)channel) {
                            MidiNote mn = { channel, pending[i].note, 0, time_sec };
                            _midi_push(song, mn);
                            // Сдвигаем массив
                            memmove(&pending[i], &pending[i+1],
                                    (*pending_count - i - 1) * sizeof(_PendingOff));
                            (*pending_count)--;
                        } else { i++; }
                    }
                }
            }
        } else if (type == 0xA0 || type == 0xE0) { pos += 2; }
        else if (type == 0xC0 || type == 0xD0)   { pos += 1; }
        else { pos++; }
    }
}

// ---- Public API ----

static int midi_load_mem(MidiSong *song, const uint8_t *buf, int buf_len) {
    memset(song, 0, sizeof(MidiSong));
    if (buf_len < 14) return -1;
    if (memcmp(buf, "MThd", 4) != 0) return -1;

    uint32_t header_len  = _midi_be32(buf + 4);
    if (header_len < 6) return -1;

    uint16_t format      = _midi_be16(buf + 8);
    uint16_t num_tracks  = _midi_be16(buf + 10);
    uint16_t ticks_per_qn = _midi_be16(buf + 12);
    (void)format;

    if (ticks_per_qn & 0x8000) return -1;

    int first_track_offset = 8 + (int)header_len;

    // FIX 4: строим карту темпа из ВСЕХ треков за один проход
    TempoMap tmap = { NULL, 0, 0, ticks_per_qn };
    _tmap_push(&tmap, 0, 500000.0);  // дефолтный темп в начале
    _build_tempo_map(buf, buf_len, num_tracks, first_track_offset, &tmap);

    // FIX 5: sustain state + pending note-offs (per channel)
    uint8_t sustain[16] = {};
    _PendingOff pending[256];
    int pending_count = 0;

    // Парсим ноты с использованием tempo map
    int offset = first_track_offset;
    for (int t = 0; t < num_tracks; t++) {
        if (offset + 8 > buf_len) break;
        if (memcmp(buf + offset, "MTrk", 4) != 0) break;
        uint32_t track_len = _midi_be32(buf + offset + 4);
        offset += 8;
        if (offset + (int)track_len > buf_len) break;

        _parse_notes(buf + offset, (int)track_len,
                     &tmap, song,
                     sustain, pending, &pending_count, 256);
        offset += (int)track_len;
    }

    // Сбрасываем оставшиеся pending note-offs (если педаль так и не отпустили)
    for (int i = 0; i < pending_count; i++) {
        MidiNote mn = { pending[i].channel, pending[i].note, 0,
                        song->count > 0 ? song->notes[song->count-1].time_sec : 0.0 };
        _midi_push(song, mn);
    }

    free(tmap.pts);

    qsort(song->notes, song->count, sizeof(MidiNote), _midi_cmp);

    if (song->count > 0)
        song->total_duration = song->notes[song->count - 1].time_sec + 0.5;

    return 0;
}

static int midi_load_file(MidiSong *song, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) { fclose(f); return -1; }
    fread(buf, 1, (size_t)size, f);
    fclose(f);
    int result = midi_load_mem(song, buf, (int)size);
    free(buf);
    return result;
}

static void midi_free(MidiSong *song) {
    if (song->notes) free(song->notes);
    memset(song, 0, sizeof(MidiSong));
}
