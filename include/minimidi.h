#pragma once
// ============================================================
//  miniMidi — minimal single-header MIDI file parser
//  No external dependencies. Works on Switch (libnx) and PC.
//  Supports SMF Type 0 and Type 1.
// ============================================================

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

// ---- Data structures ----

typedef struct {
    int     channel;   // MIDI channel (0-15)
    int     note;      // MIDI note number (0-127)
    int     velocity;  // velocity (0-127; 0 = note off)
    double  time_sec;  // absolute time in seconds
} MidiNote;

typedef struct {
    MidiNote *notes;
    int       count;
    int       capacity;
    double    total_duration;  // seconds
} MidiSong;

// ---- Internal helpers ----

static uint32_t _midi_read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] <<  8) |  (uint32_t)p[3];
}
static uint16_t _midi_read_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

// Variable-length quantity
static uint32_t _midi_read_vlq(const uint8_t *data, int *pos, int len) {
    uint32_t val = 0;
    for (int i = 0; i < 4; i++) {
        if (*pos >= len) break;
        uint8_t b = data[(*pos)++];
        val = (val << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return val;
}

static void _midi_note_push(MidiSong *song, MidiNote note) {
    if (song->count >= song->capacity) {
        song->capacity = song->capacity ? song->capacity * 2 : 256;
        song->notes = (MidiNote *)realloc(song->notes,
                                          song->capacity * sizeof(MidiNote));
    }
    song->notes[song->count++] = note;
}

// ---- Compare function for qsort ----
static int _midi_note_cmp(const void *a, const void *b) {
    double diff = ((const MidiNote *)a)->time_sec -
                  ((const MidiNote *)b)->time_sec;
    return diff < 0 ? -1 : diff > 0 ? 1 : 0;
}

// ---- Parse a single track chunk ----
// Returns number of notes appended to song.
// tempo_bpm is updated if a Set Tempo event is found.
static void _midi_parse_track(const uint8_t *data, int data_len,
                               int ticks_per_qn,
                               MidiSong *song,
                               double *tempo_us_out)
{
    double tempo_us = *tempo_us_out;  // microseconds per quarter note
    double abs_time_sec = 0.0;
    uint32_t abs_ticks = 0;
    int pos = 0;
    uint8_t running_status = 0;

    while (pos < data_len) {
        // Delta time
        uint32_t delta = _midi_read_vlq(data, &pos, data_len);
        abs_ticks += delta;
        abs_time_sec += (delta * tempo_us) / (ticks_per_qn * 1e6);

        if (pos >= data_len) break;

        uint8_t status = data[pos];

        // Meta event
        if (status == 0xFF) {
            pos++;
            if (pos >= data_len) break;
            uint8_t meta_type = data[pos++];
            uint32_t meta_len = _midi_read_vlq(data, &pos, data_len);

            if (meta_type == 0x51 && meta_len == 3) {
                // Set Tempo: 3 bytes, microseconds per quarter note
                tempo_us = (double)(((uint32_t)data[pos] << 16) |
                                    ((uint32_t)data[pos+1] << 8) |
                                     (uint32_t)data[pos+2]);
                *tempo_us_out = tempo_us;
            }
            // 0x2F = End of Track
            pos += (int)meta_len;
            continue;
        }

        // SysEx
        if (status == 0xF0 || status == 0xF7) {
            pos++;
            uint32_t sysex_len = _midi_read_vlq(data, &pos, data_len);
            pos += (int)sysex_len;
            running_status = 0;
            continue;
        }

        // MIDI event
        uint8_t ev;
        if (status & 0x80) {
            ev = status;
            pos++;
            running_status = status;
        } else {
            // Running status
            ev = running_status;
        }

        uint8_t type    = ev & 0xF0;
        uint8_t channel = ev & 0x0F;

        if (type == 0x90 || type == 0x80) {
            // Note On / Note Off
            if (pos + 1 >= data_len) break;
            uint8_t note = data[pos++];
            uint8_t vel  = data[pos++];

            MidiNote mn;
            mn.channel  = channel;
            mn.note     = note;
            mn.velocity = (type == 0x80) ? 0 : vel;  // note off = vel 0
            mn.time_sec = abs_time_sec;
            _midi_note_push(song, mn);

        } else if (type == 0xA0) {
            pos += 2;  // Aftertouch: skip
        } else if (type == 0xB0) {
            pos += 2;  // Control Change: skip
        } else if (type == 0xC0) {
            pos += 1;  // Program Change: skip
        } else if (type == 0xD0) {
            pos += 1;  // Channel Pressure: skip
        } else if (type == 0xE0) {
            pos += 2;  // Pitch Bend: skip
        } else {
            // Unknown — try to skip 1 byte
            pos++;
        }
    }
}

// ---- Public API ----

// Load a MIDI file from a memory buffer.
// Returns 0 on success, -1 on error.
static int midi_load_mem(MidiSong *song, const uint8_t *buf, int buf_len) {
    memset(song, 0, sizeof(MidiSong));

    if (buf_len < 14) return -1;

    // MThd
    if (memcmp(buf, "MThd", 4) != 0) return -1;
    uint32_t header_len = _midi_read_be32(buf + 4);
    if (header_len < 6) return -1;

    uint16_t format        = _midi_read_be16(buf + 8);
    uint16_t num_tracks    = _midi_read_be16(buf + 10);
    uint16_t ticks_per_qn  = _midi_read_be16(buf + 12);

    // SMPTE timecode not supported
    if (ticks_per_qn & 0x8000) return -1;

    int offset = 8 + (int)header_len;
    double tempo_us = 500000.0;  // default: 120 BPM

    for (int t = 0; t < num_tracks; t++) {
        if (offset + 8 > buf_len) break;
        if (memcmp(buf + offset, "MTrk", 4) != 0) break;
        uint32_t track_len = _midi_read_be32(buf + offset + 4);
        offset += 8;

        if (offset + (int)track_len > buf_len) break;

        _midi_parse_track(buf + offset, (int)track_len,
                          ticks_per_qn, song, &tempo_us);
        offset += (int)track_len;
    }

    // Sort all events by time (important for multi-track Type 1)
    qsort(song->notes, song->count, sizeof(MidiNote), _midi_note_cmp);

    // Record total duration
    if (song->count > 0)
        song->total_duration = song->notes[song->count - 1].time_sec + 0.5;

    return 0;
}

// Load a MIDI file from disk (uses fopen/fread — available on Switch via libnx)
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

// Free memory allocated by midi_load_*
static void midi_free(MidiSong *song) {
    if (song->notes) free(song->notes);
    memset(song, 0, sizeof(MidiSong));
}
