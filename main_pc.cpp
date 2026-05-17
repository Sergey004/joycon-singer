// ============================================================
//  JoyCon Singer — PC version
//  Plays MIDI files through Joy-Con / Pro Controller HD Rumble
//
//  Build:  make -f Makefile.pc
//  Usage:  ./joycon-singer [-p] [-y] [-l|-r|-b] [-a AMP] MIDI_FILE
//
//  Connections (Bluetooth):
//    Joy-Con L   : VID=0x057E  PID=0x2006
//    Joy-Con R   : VID=0x057E  PID=0x2007
//    Pro Ctrl    : VID=0x057E  PID=0x2009
//
//  MIDI channel mapping:
//    Channel 0 → Right haptic (Joy-Con R / Pro right)
//    Channel 1 → Left  haptic (Joy-Con L / Pro left)
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <hidapi.h>

#include "rumble.h"
#include "minimidi.h"

// Nintendo VIDs / PIDs
#define NINTENDO_VID     0x057E
#define JOYCON_L_PID     0x2006
#define JOYCON_R_PID     0x2007
#define PRO_CTRL_PID     0x2009

// Which controller mode to use
typedef enum {
    MODE_AUTO,       // auto-detect best available
    MODE_JOYCON_LR,  // separate Joy-Con L + R
    MODE_PRO,        // Pro Controller
    MODE_JOYCON_R,   // single Joy-Con R only
    MODE_JOYCON_L,   // single Joy-Con L only
} CtrlMode;

// ---- Global state ----
static hid_device *g_dev_l = NULL;   // Left  (Joy-Con L or Pro for left channel)
static hid_device *g_dev_r = NULL;   // Right (Joy-Con R or Pro for right channel)
static int         g_pro_mode = 0;   // 1 = single Pro Controller device
static volatile int g_running = 1;
static uint8_t g_timer = 0;

// ----  Low-level send  ----

static void joycon_send_rumble(uint8_t *left4, uint8_t *right4) {
    uint8_t neutral[4] = RUMBLE_NEUTRAL;
    if (!left4)  left4  = neutral;
    if (!right4) right4 = neutral;

    if (g_pro_mode && g_dev_l) {
        // Pro Controller: one device, sends both channels in a single packet
        uint8_t pkt[10] = {0};
        pkt[0] = 0x10;
        pkt[1] = g_timer++ & 0x0F;
        memcpy(&pkt[2], left4,  4);
        memcpy(&pkt[6], right4, 4);
        hid_write(g_dev_l, pkt, 10);
    } else {
        // Separate Joy-Cons — each gets its own rumble-only packet
        if (g_dev_l) {
            uint8_t pkt[10] = {0};
            pkt[0] = 0x10;
            pkt[1] = g_timer++ & 0x0F;
            memcpy(&pkt[2], left4,  4);
            memcpy(&pkt[6], neutral, 4);
            hid_write(g_dev_l, pkt, 10);
        }
        if (g_dev_r) {
            uint8_t pkt[10] = {0};
            pkt[0] = 0x10;
            pkt[1] = g_timer++ & 0x0F;
            memcpy(&pkt[2], neutral, 4);
            memcpy(&pkt[6], right4, 4);
            hid_write(g_dev_r, pkt, 10);
        }
    }
}

// ---- Init / cleanup ----

static void joycon_enable_vibration(hid_device *dev) {
    uint8_t pkt[12] = {0};
    pkt[0]  = 0x01;
    pkt[1]  = g_timer++ & 0x0F;
    uint8_t neutral[4] = RUMBLE_NEUTRAL;
    memcpy(&pkt[2], neutral, 4);
    memcpy(&pkt[6], neutral, 4);
    pkt[10] = 0x48;
    pkt[11] = 0x01;
    hid_write(dev, pkt, 12);
    usleep(50000);
}

static void joycon_set_input_mode(hid_device *dev) {
    uint8_t pkt[12] = {0};
    pkt[0]  = 0x01;
    pkt[1]  = g_timer++ & 0x0F;
    uint8_t neutral[4] = RUMBLE_NEUTRAL;
    memcpy(&pkt[2], neutral, 4);
    memcpy(&pkt[6], neutral, 4);
    pkt[10] = 0x03;
    pkt[11] = 0x30;
    hid_write(dev, pkt, 12);
    usleep(50000);
}

static void stop_all_rumble(void) {
    uint8_t neutral[4] = RUMBLE_NEUTRAL;
    joycon_send_rumble(neutral, neutral);
}

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static hid_device *open_controller(uint16_t pid, const char *name) {
    hid_device *dev = hid_open(NINTENDO_VID, pid, NULL);
    if (dev) {
        printf("[joycon-singer] Found %s\n", name);
        hid_set_nonblocking(dev, 1);
        joycon_set_input_mode(dev);
        joycon_enable_vibration(dev);
    }
    return dev;
}

static int init_controllers(CtrlMode mode) {
    if (mode == MODE_AUTO || mode == MODE_PRO) {
        g_dev_l = open_controller(PRO_CTRL_PID, "Pro Controller");
        if (g_dev_l) { g_pro_mode = 1; return 1; }
    }
    if (mode == MODE_AUTO || mode == MODE_JOYCON_LR) {
        g_dev_l = open_controller(JOYCON_L_PID, "Joy-Con (L)");
        g_dev_r = open_controller(JOYCON_R_PID, "Joy-Con (R)");
        if (g_dev_l || g_dev_r) return 1;
    }
    if (mode == MODE_JOYCON_L) {
        g_dev_l = open_controller(JOYCON_L_PID, "Joy-Con (L)");
        return g_dev_l != NULL;
    }
    if (mode == MODE_JOYCON_R) {
        g_dev_r = open_controller(JOYCON_R_PID, "Joy-Con (R)");
        return g_dev_r != NULL;
    }
    return 0;
}

static void close_controllers(void) {
    stop_all_rumble();
    usleep(20000);
    if (g_dev_l) { hid_close(g_dev_l); g_dev_l = NULL; }
    if (g_dev_r && !g_pro_mode) { hid_close(g_dev_r); g_dev_r = NULL; }
}

// ---- MIDI playback ----

static void play_midi(const char *midi_path, float amplitude, int repeat, int interval_us) {
    MidiSong song;

    do {
        if (midi_load_file(&song, midi_path) != 0) {
            fprintf(stderr, "Error: cannot read MIDI file: %s\n", midi_path);
            return;
        }

        printf("[joycon-singer] MIDI: %d notes, %.1f sec\n", song.count, song.total_duration);

        float ch_freq[2] = {160.0f, 160.0f};
        float ch_amp[2]  = {0.0f, 0.0f};
        double time_sec = 0.0;
        int ei = 0;

        while (ei < song.count && g_running) {
            MidiNote *ev = &song.notes[ei];
            double ev_time = ev->time_sec;
            double sleep_s = ev_time - time_sec;
            
            if (sleep_s > 0.0) {
                long sleep_us = (long)(sleep_s * 1e6);
                while (sleep_us > 0 && g_running) {
                    long chunk = sleep_us > interval_us ? interval_us : sleep_us;
                    usleep((useconds_t)chunk);
                    sleep_us -= chunk;
                }
            }
            time_sec = ev_time;
            if (!g_running) break;

            bool changed = false;
            while (ei < song.count && song.notes[ei].time_sec <= time_sec) {
                MidiNote *n = &song.notes[ei++];
                int ch = n->channel;
                if (ch > 1) continue;

                if (n->velocity > 0) {
                    float freq = midi_note_to_freq(n->note);
                    float vel_amp = (n->velocity / 127.0f) * amplitude;
                    ch_freq[ch] = freq;
                    ch_amp[ch]  = vel_amp;
                    
                    const char *nn[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                    printf("  Ch%d: %s%d  %.1f Hz  amp %.2f\n",
                           ch, nn[n->note % 12], (n->note / 12) - 1, freq, vel_amp);
                } else {
                    ch_amp[ch]  = 0.0f;
                }
                changed = true;
            }

            if (changed) {
                uint8_t left4[4], right4[4];
                if (ch_amp[1] > 0.0f) encode_rumble(ch_freq[1], ch_amp[1], left4);
                else encode_rumble_stop(left4);

                if (ch_amp[0] > 0.0f) encode_rumble(ch_freq[0], ch_amp[0], right4);
                else encode_rumble_stop(right4);

                joycon_send_rumble(left4, right4);
            }
        }

        stop_all_rumble();
        midi_free(&song);

        if (repeat && g_running) printf("[joycon-singer] Repeating...\n");

    } while (repeat && g_running);
}

// ---- Main ----

static void usage(const char *prog) {
    printf("Usage: %s [-p] [-a AMP] [-i INTERVAL] [-l|-r|-b|-P] MIDI_FILE\n\n"
           "  -p            Repeat song\n"
           "  -a AMP        Amplitude 0.0-1.0 (default 0.7)\n"
           "  -i INTERVAL   Sleep interval in microseconds (default 5000)\n"
           "  -l            Force Joy-Con L only\n"
           "  -r            Force Joy-Con R only\n"
           "  -b            Force Joy-Con L+R pair\n"
           "  -P            Force Pro Controller\n\n"
           "MIDI channel 0 → right haptic, channel 1 → left haptic\n"
           "Pair controllers via Bluetooth before running.\n", prog);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }

    int     repeat      = 0;
    float   amplitude   = 0.7f;
    int     interval_us = 5000;
    CtrlMode mode       = MODE_AUTO;
    const char *midi_path = NULL;

    for (int i = 1; i < argc; i++) {
        if      (strcmp(argv[i], "-p") == 0) repeat = 1;
        else if (strcmp(argv[i], "-l") == 0) mode = MODE_JOYCON_L;
        else if (strcmp(argv[i], "-r") == 0) mode = MODE_JOYCON_R;
        else if (strcmp(argv[i], "-b") == 0) mode = MODE_JOYCON_LR;
        else if (strcmp(argv[i], "-P") == 0) mode = MODE_PRO;
        else if (strcmp(argv[i], "-a") == 0 && i+1 < argc) amplitude   = atof(argv[++i]);
        else if (strcmp(argv[i], "-i") == 0 && i+1 < argc) interval_us = atoi(argv[++i]);
        else if (argv[i][0] != '-') midi_path = argv[i];
    }

    if (!midi_path) { usage(argv[0]); return 1; }

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    if (hid_init() != 0) {
        fprintf(stderr, "Error: hid_init() failed\n");
        return 1;
    }

    printf("[joycon-singer] Connecting to controllers...\n");
    if (!init_controllers(mode)) {
        fprintf(stderr,
            "Error: no Nintendo Switch controller found.\n"
            "  Make sure your Joy-Con or Pro Controller is paired via Bluetooth\n"
            "  and that you have permission to access HID devices.\n"
            "  On Linux: add a udev rule or run as root.\n");
        hid_exit();
        return 1;
    }

    printf("[joycon-singer] Playing: %s\n", midi_path);
    play_midi(midi_path, amplitude, repeat, interval_us);

    close_controllers();
    hid_exit();
    printf("\n[joycon-singer] Done.\n");
    return 0;
}