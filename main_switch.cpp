// ============================================================
//  JoyCon Singer — Nintendo Switch CFW Homebrew (Dynamic)
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

#define APP_DIR "sdmc:/switch/joycon-singer"

// ---- Dynamic Vibration System ----

static HidVibrationDeviceHandle g_handles[2];
static int g_vib_count = 0;
// Используем 9999 как маркер "контроллер отключен", т.к. Invalid нет в libnx
static u32 g_active_id = 9999; 
static u32 g_active_style = 0;

// Вызывается каждый кадр для моментального подхвата нового контроллера
static void vib_update_handles() {
    // Приоритет: сначала Handheld (консоль в руках), затем Player 1
    HidNpadIdType ids[] = { HidNpadIdType_Handheld, HidNpadIdType_No1 };
    
    for (int i = 0; i < 2; i++) {
        u32 style = hidGetNpadStyleSet(ids[i]);
        if (style == 0) continue; 
        
        // Если контроллер поменялся, переинициализируем моторы
        if ((u32)ids[i] != g_active_id || style != g_active_style) {
            g_active_id = (u32)ids[i];
            g_active_style = style;
            g_vib_count = 0;
            
            if (style & HidNpadStyleTag_NpadHandheld) {
                hidInitializeVibrationDevices(g_handles, 2, ids[i], HidNpadStyleTag_NpadHandheld);
                g_vib_count = 2;
            } else if (style & HidNpadStyleTag_NpadJoyDual) {
                hidInitializeVibrationDevices(g_handles, 2, ids[i], HidNpadStyleTag_NpadJoyDual);
                g_vib_count = 2;
            } else if (style & HidNpadStyleTag_NpadFullKey) {
                hidInitializeVibrationDevices(g_handles, 2, ids[i], HidNpadStyleTag_NpadFullKey);
                g_vib_count = 2;
            } else if (style & HidNpadStyleTag_NpadJoyLeft) {
                hidInitializeVibrationDevices(g_handles, 1, ids[i], HidNpadStyleTag_NpadJoyLeft);
                g_vib_count = 1; // Только левый
            } else if (style & HidNpadStyleTag_NpadJoyRight) {
                hidInitializeVibrationDevices(g_handles, 1, ids[i], HidNpadStyleTag_NpadJoyRight);
                g_vib_count = -1; // Только правый
            }
        }
        return; 
    }
    
    g_active_id = 9999;
    g_active_style = 0;
    g_vib_count = 0;
}

static void send_vibration(float freq_L, float amp_L, float freq_R, float amp_R) {
    if (g_vib_count == 0) return;

    HidVibrationValue values[2];
    
    // Left (MIDI Channel 1)
    float fL = (freq_L < 41.0f) ? 41.0f : (freq_L > 626.0f) ? 626.0f : freq_L;
    float aL = (amp_L < 0.0f) ? 0.0f : (amp_L > 0.8f) ? 0.8f : amp_L;
    values[0].freq_low  = fL;
    values[0].amp_low   = aL;
    values[0].freq_high = (fL * 2.0f > 1252.0f) ? 1252.0f : fL * 2.0f;
    values[0].amp_high  = aL * 0.5f;

    // Right (MIDI Channel 0)
    float fR = (freq_R < 41.0f) ? 41.0f : (freq_R > 626.0f) ? 626.0f : freq_R;
    float aR = (amp_R < 0.0f) ? 0.0f : (amp_R > 0.8f) ? 0.8f : amp_R;
    values[1].freq_low  = fR;
    values[1].amp_low   = aR;
    values[1].freq_high = (fR * 2.0f > 1252.0f) ? 1252.0f : fR * 2.0f;
    values[1].amp_high  = aR * 0.5f;

    if (g_vib_count == 2) {
        hidSendVibrationValues(g_handles, values, 2);
    } else if (g_vib_count == 1) {
        hidSendVibrationValues(&g_handles[0], &values[0], 1); // Только левый
    } else if (g_vib_count == -1) {
        hidSendVibrationValues(&g_handles[0], &values[1], 1); // Только правый
    }
}

static void stop_all_vibration() {
    send_vibration(160.0f, 0.0f, 160.0f, 0.0f);
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

static void draw_top_bar() {
    printf("\n \xE2\x99\xAB  JoyCon Singer (Dynamic Mode)\n");
    printf(" ─────────────────────────────────\n");
    if (g_vib_count == 2) {
        printf(" Active: Stereo (L & R motors)\n");
    } else if (g_vib_count == 1) {
        printf(" Active: Mono (Left motor only)\n");
    } else if (g_vib_count == -1) {
        printf(" Active: Mono (Right motor only)\n");
    } else {
        printf(" Active: NO RUMBLE CONNECTED!\n");
    }
    printf(" ─────────────────────────────────\n\n");
}

// ---- Playback ----

static int play_file(const char *path, float amplitude, PadState *pad) {
    consoleClear();

    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;

    draw_top_bar();
    printf(" Loading: %s\n\n", fname);
    consoleUpdate(NULL); 

    MidiSong song;
    if (midi_load_file(&song, path) != 0) {
        printf(" ERROR: Cannot read MIDI file!\n");
        consoleUpdate(NULL);
        svcSleepThread(2000000000LL);
        return 0;
    }

    printf(" A=Pause  B=Back  +=Next  -=Prev\n\n");

    float freq_L = 160.0f, amp_L = 0.0f;
    float freq_R = 160.0f, amp_R = 0.0f;

    double playhead   = 0.0;
    bool   paused     = false;
    int    ei         = 0;
    
    double tick_freq    = (double)armGetSystemTickFreq();
    uint64_t start_tick = armGetSystemTick();
    uint64_t pause_tick = 0;

    while (appletMainLoop()) {
        padUpdate(pad);
        vib_update_handles(); 

        uint64_t down = padGetButtonsDown(pad);

        if (down & HidNpadButton_Plus)  { stop_all_vibration(); midi_free(&song); return  1; }
        if (down & HidNpadButton_Minus) { stop_all_vibration(); midi_free(&song); return -1; }
        if (down & HidNpadButton_B)     { stop_all_vibration(); midi_free(&song); return  0; }
        if (down & HidNpadButton_A) {
            paused = !paused;
            if (paused) {
                stop_all_vibration();
                pause_tick = armGetSystemTick();
            } else {
                start_tick += (armGetSystemTick() - pause_tick);
            }
        }

        if (!paused) { 
            playhead = (double)(armGetSystemTick() - start_tick) / tick_freq;

            while (ei < song.count && song.notes[ei].time_sec <= playhead) {
                MidiNote *n = &song.notes[ei++];
                int ch = n->channel;
                if (ch > 1) continue; 

                if (n->velocity > 0) {
                    float f = midi_note_to_freq(n->note);
                    float a = (n->velocity / 127.0f) * amplitude;
                    if (ch == 0) { freq_R = f; amp_R = a; }
                    if (ch == 1) { freq_L = f; amp_L = a; }
                    
                    const char *nn[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
                    printf("  [%.1fs] ch%d: %s%d  %.0fHz\n", 
                           playhead, ch, nn[n->note % 12], (n->note / 12) - 1, f);
                } else {
                    if (ch == 0) { amp_R = 0.0f; }
                    if (ch == 1) { amp_L = 0.0f; }
                }
            }
        } else {
            amp_L = 0.0f;
            amp_R = 0.0f;
        }

        // Значения подаются каждый кадр, не давая системе "заглушить" мотор!
        send_vibration(freq_L, amp_L, freq_R, amp_R);

        if (ei >= song.count && playhead > song.total_duration) {
            stop_all_vibration();
            svcSleepThread(1500000000LL);
            break;
        }

        consoleUpdate(NULL);
    }

    midi_free(&song);
    return 1;
}

// ---- File browser ----

static void browser(PadState *pad) {
    int sel = 0;

    while (appletMainLoop()) {
        padUpdate(pad);
        vib_update_handles();

        int n = (int)g_files.size();
        consoleClear();

        draw_top_bar();

        if (n == 0) {
            printf(" No .mid files found in:\n  %s\n\n", APP_DIR);
            printf(" Press [+] to rescan\n");
        } else {
            for (int i = 0; i < n; i++) {
                const char *fname = strrchr(g_files[i].c_str(), '/');
                fname = fname ? fname + 1 : g_files[i].c_str();
                printf(" %s %.42s\n", i == sel ? ">" : " ", fname);
            }
            printf("\n A=Play  +/-=Select  B=Exit\n");
        }

        uint64_t down = padGetButtonsDown(pad);
        
        if (down & HidNpadButton_B) break;

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

        consoleUpdate(NULL);
    }
}

int main(int argc, char *argv[]) {
    consoleInit(NULL);

    // Поддержка до 8 слотов, чтобы захватывать любые контроллеры
    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    
    PadState pad;
    padInitializeDefault(&pad);

    mkdir(APP_DIR, 0777);
    scan_files();

    browser(&pad);

    stop_all_vibration();
    consoleExit(NULL);
    return 0;
}