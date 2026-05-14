// ============================================================
//  JoyCon Singer — Nintendo Switch CFW Homebrew
//  Multi-controller edition: до 8 пар Joy-Con = 16 моторов
//
//  Маппинг MIDI каналов:
//    ch  0 → контроллер #1 Right
//    ch  1 → контроллер #1 Left
//    ch  2 → контроллер #2 Right
//    ch  3 → контроллер #2 Left
//    ...
//    ch 14 → контроллер #8 Right
//    ch 15 → контроллер #8 Left
//
//  Build:   make -f Makefile.switch
//  Install: sdmc:/switch/joycon-singer/joycon-singer.nro
//  MIDI:    sdmc:/switch/joycon-singer/*.mid
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <string>
#include <algorithm>

#include <switch.h>
#include "rumble.h"
#include "minimidi.h"

#define APP_DIR    "sdmc:/switch/joycon-singer"
#define MAX_PADS   8    
#define MAX_MOTORS (MAX_PADS * 2)  

// ---- Vibration state ----

typedef struct {
    HidVibrationDeviceHandle handle[2];
    int   motors;     
    bool  active;
    float freq[2];
    float amp[2];
} PadVib;

static PadVib g_pads[MAX_PADS];
static int    g_pad_count = 0;

static const HidNpadIdType PAD_IDS[MAX_PADS] = {
    HidNpadIdType_No1, HidNpadIdType_No2,
    HidNpadIdType_No3, HidNpadIdType_No4,
    HidNpadIdType_No5, HidNpadIdType_No6,
    HidNpadIdType_No7, HidNpadIdType_No8,
};

static void vib_init_all() {
    g_pad_count = 0;

    PadVib *hh = &g_pads[g_pad_count];
    if (R_SUCCEEDED(hidInitializeVibrationDevices(
            hh->handle, 2,
            HidNpadIdType_Handheld,
            HidNpadStyleTag_NpadHandheld))) {
        hh->motors = 2;
        hh->active = true;
        hh->freq[0] = hh->freq[1] = 160.0f;
        hh->amp[0]  = hh->amp[1]  = 0.0f;
        g_pad_count++;
    }

    for (int i = 0; i < MAX_PADS && g_pad_count < MAX_PADS; i++) {
        PadVib *pv = &g_pads[g_pad_count];

        if (R_SUCCEEDED(hidInitializeVibrationDevices(
                pv->handle, 2,
                PAD_IDS[i],
                HidNpadStyleTag_NpadJoyDual))) {
            pv->motors = 2;
            pv->active = true;
            pv->freq[0] = pv->freq[1] = 160.0f;
            pv->amp[0]  = pv->amp[1]  = 0.0f;
            g_pad_count++;
            continue;
        }

        if (R_SUCCEEDED(hidInitializeVibrationDevices(
                pv->handle, 1,
                PAD_IDS[i],
                HidNpadStyleTag_NpadFullKey))) {
            pv->motors = 1;
            pv->active = true;
            pv->freq[0] = pv->freq[1] = 160.0f;
            pv->amp[0]  = pv->amp[1]  = 0.0f;
            g_pad_count++;
        }
    }
}

static void vib_set_motor(int motor_idx, float freq, float amp) {
    int pad_idx  = motor_idx / 2;
    int side     = motor_idx % 2;   
    int hid_side = (side == 0) ? 1 : 0; 

    if (pad_idx >= g_pad_count) return;
    PadVib *pv = &g_pads[pad_idx];
    if (!pv->active) return;

    int actual_side = (pv->motors == 1) ? 0 : hid_side;

    pv->freq[hid_side] = freq;
    pv->amp[hid_side]  = amp;

    float f = (freq  < 41.0f) ? 41.0f   : (freq  > 626.0f) ? 626.0f : freq;
    float a = (amp   < 0.0f)  ? 0.0f    : (amp   > 0.8f)   ? 0.8f   : amp;

    HidVibrationValue val = {
        .amp_low   = a,
        .freq_low  = f,
        .amp_high  = a * 0.5f,
        .freq_high = (f * 2.0f > 1252.0f) ? 1252.0f : f * 2.0f,
    };
    hidSendVibrationValues(&pv->handle[actual_side], &val, 1);
}

static void vib_stop_all() {
    for (int m = 0; m < MAX_MOTORS; m++)
        vib_set_motor(m, 160.0f, 0.0f);
}

// ---- File scanner ----

static std::vector<std::string> g_files;

static void scan_files() {
    g_files.clear();
    DIR *dir = opendir(APP_DIR);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        std::string name = ent->d_name;
        if (name.size() < 5) continue;
        std::string ext = name.substr(name.size() - 4);
        for (char &c : ext) if (c >= 'A' && c <= 'Z') c += 32;
        if (ext == ".mid")
            g_files.push_back(std::string(APP_DIR "/") + name);
    }
    closedir(dir);
    std::sort(g_files.begin(), g_files.end());
}

static void draw_controller_bar() {
    printf(" Controllers: %d  |  Motors: ", g_pad_count);
    int total_motors = 0;
    for (int i = 0; i < g_pad_count; i++)
        total_motors += g_pads[i].motors;
    printf("%d / 16  |  MIDI ch: 0-%d\n", total_motors, total_motors - 1);

    for (int i = 0; i < g_pad_count; i++) {
        printf("  #%d [%s]", i + 1,
               g_pads[i].motors == 2 ? "JC-L JC-R" : "Pro  ----");
        printf("  ch %2d,%2d\n", i * 2, i * 2 + 1);
    }
}

// ---- Playback ----

static int play_file(const char *path, float amplitude, PadState *pad) {
    consoleClear();

    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;

    printf("\n \xE2\x99\xAB  JoyCon Singer\n");
    printf(" Loading: %s\n\n", fname);
    draw_controller_bar();
    printf("\n A=Pause  B=Back  +=Next  -=Prev\n");
    printf(" ─────────────────────────────────\n");

    MidiSong song;
    if (midi_load_file(&song, path) != 0) {
        printf(" ERROR: Cannot read MIDI file!\n");
        svcSleepThread(2000000000LL);
        return 0;
    }

    printf(" Notes: %d  Duration: %.1fs\n\n", song.count, song.total_duration);

    float motor_freq[MAX_MOTORS];
    float motor_amp[MAX_MOTORS];
    for (int m = 0; m < MAX_MOTORS; m++) {
        motor_freq[m] = 160.0f;
        motor_amp[m]  = 0.0f;
    }

    double playhead = 0.0;
    bool   paused   = false;
    int    ei       = 0;
    const long long TICK_NS = 5000000LL;  // 5ms

    while (appletMainLoop()) {
        padUpdate(pad);
        uint64_t down = padGetButtonsDown(pad);

        if (down & HidNpadButton_Plus)  { vib_stop_all(); midi_free(&song); return  1; }
        if (down & HidNpadButton_Minus) { vib_stop_all(); midi_free(&song); return -1; }
        if (down & HidNpadButton_B)     { vib_stop_all(); midi_free(&song); return  0; }
        if (down & HidNpadButton_A) {
            paused = !paused;
            if (paused) vib_stop_all();
        }

        if (paused) { svcSleepThread(TICK_NS); continue; }

        playhead += TICK_NS / 1e9;

        bool changed = false;
        while (ei < song.count && song.notes[ei].time_sec <= playhead) {
            MidiNote *n = &song.notes[ei++];
            int ch = n->channel;
            if (ch >= MAX_MOTORS) continue;

            if (n->velocity > 0) {
                motor_freq[ch] = midi_note_to_freq(n->note);
                motor_amp[ch]  = (n->velocity / 127.0f) * amplitude;
                const char *nn[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                printf("  ch%2d: %-3s%d %.0fHz\n",
                       ch, nn[n->note % 12], (n->note / 12) - 1, motor_freq[ch]);
            } else {
                motor_freq[ch] = 160.0f;
                motor_amp[ch]  = 0.0f;
            }
            changed = true;
        }

        if (changed) {
            for (int m = 0; m < MAX_MOTORS; m++)
                vib_set_motor(m, motor_freq[m], motor_amp[m]);
        }

        if (ei >= song.count && playhead > song.total_duration) {
            vib_stop_all();
            svcSleepThread(1500000000LL);
            break;
        }

        svcSleepThread(TICK_NS);
    }

    midi_free(&song);
    return 1;
}

// ---- File browser ----

static void browser(PadState *pad) {
    int sel = 0;

    while (appletMainLoop()) {
        int n = (int)g_files.size();
        consoleClear();

        printf("\n \xE2\x99\xAB  JoyCon Singer\n");
        printf(" ─────────────────────────────────\n");
        draw_controller_bar();
        printf(" ─────────────────────────────────\n\n");

        if (n == 0) {
            printf(" No .mid files found in:\n  %s\n\n", APP_DIR);
            printf(" + to rescan\n");
        } else {
            for (int i = 0; i < n; i++) {
                const char *fname = strrchr(g_files[i].c_str(), '/');
                fname = fname ? fname + 1 : g_files[i].c_str();
                printf(" %s %.42s\n", i == sel ? ">" : " ", fname);
            }
            printf("\n A=Play  +/-=Select  HOME=Exit\n");
        }

        padUpdate(pad);
        uint64_t down = padGetButtonsDown(pad);

        if (n == 0) {
            if (down & HidNpadButton_Plus) scan_files();
        } else {
            if (down & HidNpadButton_Plus)  sel = (sel + 1) % n;
            if (down & HidNpadButton_Minus) sel = (sel - 1 + n) % n;
            if (down & HidNpadButton_A) {
                int cur = sel;
                while (appletMainLoop()) {
                    int res = play_file(g_files[cur].c_str(), 0.7f, pad);
                    if (!appletMainLoop()) break;
                    n = (int)g_files.size();
                    if (n == 0) break;
                    if (res ==  1) { cur = (cur + 1) % n; continue; }
                    if (res == -1) { cur = (cur - 1 + n) % n; continue; }
                    break;
                }
                sel = (n > 0) ? cur % n : 0;
            }
        }

        svcSleepThread(16000000LL);
    }
}

// ---- Entry point ----

int main(int argc, char *argv[]) {
    consoleInit(NULL);

    padConfigureInput(MAX_PADS, HidNpadStyleSet_NpadStandard);

    PadState pad;
    padInitializeDefault(&pad);

    printf("\n JoyCon Singer — Multi-Controller\n");
    printf(" Scanning controllers...\n\n");

    vib_init_all();

    int total_motors = 0;
    for (int i = 0; i < g_pad_count; i++)
        total_motors += g_pads[i].motors;

    if (g_pad_count == 0) {
        printf(" WARNING: No controllers found!\n");
        printf(" Connect Joy-Con or Pro Controller.\n\n");
    } else {
        printf(" Found %d controller(s), %d motor(s)\n",
               g_pad_count, total_motors);
        printf(" MIDI channels available: 0-%d\n\n", total_motors - 1);
    }

    mkdir(APP_DIR, 0777);
    scan_files();
    printf(" MIDI files found: %zu\n\n", g_files.size());
    printf(" Connect more controllers now if needed.\n");
    printf(" Press any button to start...\n");

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad)) break;
        svcSleepThread(16000000LL);
    }

    browser(&pad);

    vib_stop_all();
    consoleExit(NULL);
    return 0;
}