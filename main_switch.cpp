// ============================================================
//  JoyCon Singer — Nintendo Switch CFW Homebrew
//  TUI edition: ANSI colors + cursor positioning, no flicker
// ============================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <string>
#include <algorithm>

#include <switch.h>
#include "rumble.h"
#include "minimidi.h"

#define APP_DIR "sdmc:/switch/joycon-singer"

// ============================================================
//  ANSI color / cursor helpers
// ============================================================

#define C_RESET    "\033[0m"
#define C_BOLD     "\033[1m"
#define C_DIM      "\033[2m"
#define C_CYAN     "\033[1;36m"
#define C_GREEN    "\033[1;32m"
#define C_YELLOW   "\033[1;33m"
#define C_RED      "\033[1;31m"
#define C_MAGENTA  "\033[1;35m"
#define C_WHITE    "\033[1;37m"
#define C_GRAY     "\033[0;90m"

#define GOTO(r,c)   printf("\033[%d;%dH", (r), (c))
#define CLEAR_EOL() printf("\033[K")

// Box drawing (ASCII — universally safe on devkitPro console font)
#define BX_H   "-"
#define BX_V   "|"
#define BX_TL  "+"
#define BX_TR  "+"
#define BX_BL  "+"
#define BX_BR  "+"
#define BX_ML  "+"   // left T-junction
#define BX_MR  "+"   // right T-junction
#define BX_MD  "+"   // center cross / top-T

// Total line width (including border characters)
#define UI_W    78

// Draw a horizontal rule of width UI_W at given row
static void ui_hline(int row, const char *left, const char *right,
                     int div_col = 0, const char *div = nullptr) {
    GOTO(row, 1);
    printf("%s", left);
    for (int i = 2; i < UI_W; i++) {
        if (div && i == div_col) printf("%s", div);
        else                     printf(BX_H);
    }
    printf("%s", right);
}

// Print a padded row:  | <content padded to UI_W-4> |
// Пометили как unused, чтобы компилятор не ругался, пока вы её не используете
static __attribute__((unused)) void ui_row(int row, const char *fmt, ...) {
    char buf[512] = {};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    GOTO(row, 1);
    // We can't use %-*s directly with color codes (they add invisible bytes).
    // Just print and let CLEAR_EOL handle trailing space.
    printf(BX_V " %s", buf);
    GOTO(row, UI_W - 1); CLEAR_EOL();
    printf(BX_V);
}

// Draw a velocity / progress bar:   [====------] pct%
// width = number of '=' + '-' chars between the brackets
static void draw_bar(int filled, int width, float pct) {
    printf(C_CYAN "[" C_RESET);
    for (int i = 0; i < width; i++) {
        if (i < filled) printf(C_GREEN "=" C_RESET);
        else            printf(C_GRAY  "-" C_RESET);
    }
    printf(C_CYAN "]" C_RESET " " C_YELLOW "%3d%%" C_RESET, (int)(pct * 100.f));
}

static void fmt_time(char *buf, size_t sz, double s) {
    if (s < 0) s = 0;
    int m = (int)s / 60;
    double sr = s - m * 60.0;
    snprintf(buf, sz, "%d:%05.2f", m, sr);
}

// ============================================================
//  Controller / vibration
// ============================================================

static HidVibrationDeviceHandle g_handles[2];
static int  g_vib_count   = 0;
static u32  g_active_id   = 9999; // Заменено на 9999 вместо несуществующего HidNpadIdType_Unknown
static u32  g_active_style = 0;

static void vib_update_handles() {
    HidNpadIdType ids[] = { HidNpadIdType_Handheld, HidNpadIdType_No1 };
    for (int i = 0; i < 2; i++) {
        u32 style = hidGetNpadStyleSet(ids[i]);
        if (!style) continue;

        if ((u32)ids[i] != g_active_id || style != g_active_style) {
            g_active_id    = (u32)ids[i];
            g_active_style = style;
            g_vib_count    = 0;

            if      (style & HidNpadStyleTag_NpadHandheld) { hidInitializeVibrationDevices(g_handles,2,ids[i],HidNpadStyleTag_NpadHandheld); g_vib_count= 2; }
            else if (style & HidNpadStyleTag_NpadJoyDual)  { hidInitializeVibrationDevices(g_handles,2,ids[i],HidNpadStyleTag_NpadJoyDual);  g_vib_count= 2; }
            else if (style & HidNpadStyleTag_NpadFullKey)  { hidInitializeVibrationDevices(g_handles,2,ids[i],HidNpadStyleTag_NpadFullKey);  g_vib_count= 2; }
            else if (style & HidNpadStyleTag_NpadJoyLeft)  { hidInitializeVibrationDevices(g_handles,1,ids[i],HidNpadStyleTag_NpadJoyLeft);  g_vib_count= 1; }
            else if (style & HidNpadStyleTag_NpadJoyRight) { hidInitializeVibrationDevices(g_handles,1,ids[i],HidNpadStyleTag_NpadJoyRight); g_vib_count=-1; }
        }
        return;
    }
    g_active_id    = 9999; // Заменено на 9999
    g_active_style = 0;
    g_vib_count    = 0;
}

static const char *vib_label() {
    if (g_vib_count == 2)  return C_GREEN  "Stereo (L+R)" C_RESET;
    if (g_vib_count == 1)  return C_YELLOW "Mono (L only)" C_RESET;
    if (g_vib_count ==-1)  return C_YELLOW "Mono (R only)" C_RESET;
    return C_RED "No rumble!" C_RESET;
}

static void send_vibration(float fL, float aL, float fR, float aR) {
    if (!g_vib_count) return;
    auto cl = [](float v, float lo, float hi){ return v<lo?lo:v>hi?hi:v; };
    fL=cl(fL,41.f,626.f); aL=cl(aL,0.f,0.8f);
    fR=cl(fR,41.f,626.f); aR=cl(aR,0.f,0.8f);
    HidVibrationValue v[2] = {
        {.amp_low=aL,.freq_low=fL,.amp_high=aL*.5f,.freq_high=cl(fL*2.f,82.f,1252.f)},
        {.amp_low=aR,.freq_low=fR,.amp_high=aR*.5f,.freq_high=cl(fR*2.f,82.f,1252.f)},
    };
    if      (g_vib_count== 2) hidSendVibrationValues(g_handles,    v,    2);
    else if (g_vib_count== 1) hidSendVibrationValues(&g_handles[0],&v[0],1);
    else if (g_vib_count==-1) hidSendVibrationValues(&g_handles[0],&v[1],1);
}

static void stop_vibration() { send_vibration(160.f,0.f,160.f,0.f); }

// ============================================================
//  Utility
// ============================================================

static const char *note_name(int note) {
    static char buf[8];
    static const char *nn[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    snprintf(buf, sizeof(buf), "%s%d", nn[note%12], (note/12)-1);
    return buf;
}

// ============================================================
//  File scanner
// ============================================================

static std::vector<std::string> g_files;

static void scan_files() {
    g_files.clear();
    DIR *dir = opendir(APP_DIR);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        std::string name = ent->d_name;
        auto has_ext = [&](const char *ext) {
            if (name.size() <= strlen(ext)) return false;
            std::string e = name.substr(name.size() - strlen(ext));
            for (char &c : e) if (c>='A'&&c<='Z') c+=32;
            return e == ext;
        };
        if (has_ext(".mid") || has_ext(".midi"))
            g_files.push_back(std::string(APP_DIR "/") + name);
    }
    closedir(dir);
    std::sort(g_files.begin(), g_files.end());
}

// ============================================================
//  Browser TUI
//
//  Row  1   +----...----+
//  Row  2   | title     controller status |
//  Row  3   +----...----+
//  Row  4   | file count / path           |
//  Row  5   |                             |
//  Row  6   |  > [01] name.mid            |  <- LIST_START
//  ...
//  Row 40   |  (last visible row)         |  <- LIST_START + LIST_ROWS - 1
//  Row 41   +----...----+
//  Row 42   | controls                    |
//  Row 43   +----...----+
// ============================================================

#define B_TITLE     2
#define B_SEP1      3
#define B_INFO      4
#define B_LIST_Y    6
#define B_LIST_N    34    // visible rows for files
#define B_SEP2      41
#define B_CTRL      42
#define B_BOT       43

static void browser_draw(const std::vector<std::string> &files, int sel,
                          int view_top) {
    consoleClear();
    int n = (int)files.size();

    // Top border
    GOTO(1,1); printf(BX_TL); for(int i=2;i<UI_W;i++) printf(BX_H); printf(BX_TR);

    // Title row
    GOTO(B_TITLE, 1);
    printf(BX_V C_CYAN " \xE2\x99\xAB JoyCon Singer" C_RESET
           "  Motor: %s", vib_label());
    GOTO(B_TITLE, UI_W); printf(BX_V);

    // Separator
    ui_hline(B_SEP1, BX_ML, BX_MR);

    // Info row
    GOTO(B_INFO, 1);
    printf(BX_V C_GRAY "  Files: " C_RESET C_YELLOW "%d" C_RESET
           C_GRAY "   %s" C_RESET "  ", n, APP_DIR);
    GOTO(B_INFO, UI_W); printf(BX_V);

    // Empty row
    GOTO(5, 1); printf(BX_V); GOTO(5, UI_W); printf(BX_V);

    // File list
    for (int i = 0; i < B_LIST_N; i++) {
        int idx = view_top + i;
        int row = B_LIST_Y + i;
        GOTO(row, 1); printf(BX_V "  ");
        if (idx < n) {
            const char *fn = strrchr(files[idx].c_str(), '/');
            fn = fn ? fn + 1 : files[idx].c_str();
            if (idx == sel)
                printf(C_YELLOW ">" C_RESET
                       " [" C_CYAN "%02d" C_RESET "] "
                       C_WHITE "%-62.62s" C_RESET, idx+1, fn);
            else
                printf(C_GRAY  " " C_RESET
                       "  " C_DIM "%02d" C_RESET "  "
                       C_GRAY "%-62.62s" C_RESET, idx+1, fn);
        } else {
            printf("%*s", UI_W-4, "");
        }
        GOTO(row, UI_W); printf(BX_V);
    }

    if (n == 0) {
        GOTO(B_LIST_Y, 1);
        printf(BX_V C_GRAY "  No .mid/.midi files found.  "
               "Press " C_CYAN "[+]" C_GRAY " to rescan." C_RESET);
        GOTO(B_LIST_Y, UI_W); printf(BX_V);
    }

    // Scroll indicator
    if (n > B_LIST_N) {
        GOTO(B_LIST_Y + B_LIST_N/2, UI_W-3);
        printf(C_GRAY "%2d%%" C_RESET,
               (int)((float)view_top / (n - B_LIST_N) * 100));
    }

    // Bottom separators + controls
    ui_hline(B_SEP2, BX_ML, BX_MR);
    GOTO(B_CTRL, 1);
    printf(BX_V
           " " C_CYAN "\xe2\x86\x91\xe2\x86\x93" C_RESET "/Stick: Select"   // ↑↓
           "   " C_CYAN "A" C_RESET ": Play"
           "   " C_CYAN "+" C_RESET ": Rescan"
           "   " C_CYAN "B" C_RESET ": Quit");
    GOTO(B_CTRL, UI_W); printf(BX_V);
    GOTO(B_BOT, 1);
    printf(BX_BL); for(int i=2;i<UI_W;i++) printf(BX_H); printf(BX_BR);

    consoleUpdate(NULL);
}

// ============================================================
//  Player TUI
//
//  Row  1   +----...----+
//  Row  2   | ♫ filename            [>> PLAYING] |
//  Row  3   +----...----+
//  Row  4   |  Time: 0:12.3 / 2:34.5             |
//  Row  5   |  [============================] 50% |
//  Row  6   +------------------+------------------+   (with divider at col P_DIV)
//  Row  7   |  CH0 [Right]     |  CH1 [Left]      |
//  Row  8   +------------------+------------------+
//  Row  9   |  Note: A4 440Hz  |  Note: C#5 554Hz |
//  Row 10   |  Vol: [====]80%  |  Vol: [===]60%   |
//  Row 11   +------------------+------------------+
//  Row 12   +----...----+
//  Row 13   | controls                             |
//  Row 14   +----...----+
// ============================================================

#define P_TITLE     2
#define P_SEP1      3
#define P_TIME      4
#define P_PROG      5
#define P_CHSEP1    6
#define P_CHLABEL   7
#define P_CHSEP2    8
#define P_NOTE      9
#define P_VEL       10
#define P_CHSEP3    11
#define P_SEP2      12
#define P_CTRL      13
#define P_BOT       14

#define P_DIV       40   // column of the center divider in channel rows

static void player_draw_static(const char *fname, int note_count,
                                double total_sec) {
    consoleClear();
    char ttot[16]; fmt_time(ttot, sizeof(ttot), total_sec);

    // Top border
    GOTO(1,1); printf(BX_TL); for(int i=2;i<UI_W;i++) printf(BX_H); printf(BX_TR);

    // Title + status (updated dynamically — just leave placeholders)
    GOTO(P_TITLE,1);
    printf(BX_V C_CYAN " \xE2\x99\xAB " C_WHITE "%-50.50s" C_RESET, fname);
    GOTO(P_TITLE, UI_W); printf(BX_V);

    // Sep1
    ui_hline(P_SEP1, BX_ML, BX_MR);

    // Time label (static)
    GOTO(P_TIME, 1);
    printf(BX_V C_GRAY "  Time:  " C_RESET);
    GOTO(P_TIME, UI_W); printf(BX_V);

    // Progress row (static label)
    GOTO(P_PROG, 1);
    printf(BX_V "  ");
    GOTO(P_PROG, UI_W); printf(BX_V);

    // Channel rows with divider
    ui_hline(P_CHSEP1, BX_ML, BX_MR, P_DIV, BX_MD);

    GOTO(P_CHLABEL, 1);
    printf(BX_V C_CYAN "  CH0" C_RESET C_GRAY " [Right Joy-Con]" C_RESET
           "           ");
    GOTO(P_CHLABEL, P_DIV); printf(BX_V);
    printf(C_CYAN " CH1" C_RESET C_GRAY " [Left Joy-Con]" C_RESET);
    GOTO(P_CHLABEL, UI_W); printf(BX_V);

    ui_hline(P_CHSEP2, BX_ML, BX_MR, P_DIV, BX_MD);

    // Note row placeholders
    GOTO(P_NOTE, 1);    printf(BX_V); GOTO(P_NOTE, P_DIV);    printf(BX_V); GOTO(P_NOTE, UI_W);    printf(BX_V);
    GOTO(P_VEL, 1);     printf(BX_V); GOTO(P_VEL, P_DIV);     printf(BX_V); GOTO(P_VEL, UI_W);     printf(BX_V);

    ui_hline(P_CHSEP3, BX_ML, BX_MR, P_DIV, BX_MD);

    // Sep2 + controls
    ui_hline(P_SEP2, BX_ML, BX_MR);
    GOTO(P_CTRL, 1);
    printf(BX_V
           " " C_CYAN "A" C_RESET ": Pause"
           "  " C_CYAN "B" C_RESET ": Back"
           "  " C_CYAN "+" C_RESET ": Next"
           "  " C_CYAN "-" C_RESET ": Prev"
           C_GRAY "   Notes: " C_YELLOW "%d"
           C_GRAY "  Dur: " C_YELLOW "%s" C_RESET,
           note_count, ttot);
    GOTO(P_CTRL, UI_W); printf(BX_V);
    GOTO(P_BOT, 1);
    printf(BX_BL); for(int i=2;i<UI_W;i++) printf(BX_H); printf(BX_BR);
}

static void player_update_status(const char *fname, bool paused) {
    GOTO(P_TITLE, 1);
    printf(BX_V C_CYAN " \xE2\x99\xAB " C_WHITE "%-50.50s" C_RESET, fname);
    GOTO(P_TITLE, 57);
    if (paused) printf(C_YELLOW "[|| PAUSED ]" C_RESET);
    else        printf(C_GREEN  "[>> PLAYING]" C_RESET);
    GOTO(P_TITLE, UI_W); printf(BX_V);
}

static void player_update_time(double now, double total) {
    char tnow[16], ttot[16];
    fmt_time(tnow, sizeof(tnow), now);
    fmt_time(ttot, sizeof(ttot), total);

    // Time row
    GOTO(P_TIME, 10);
    printf(C_WHITE "%s" C_RESET C_GRAY " / " C_RESET C_WHITE "%s" C_RESET,
           tnow, ttot);
    CLEAR_EOL();
    GOTO(P_TIME, UI_W); printf(BX_V);

    // Progress bar (44 chars)
    float pct = (total > 0.0) ? (float)(now / total) : 0.f;
    if (pct > 1.f) pct = 1.f;
    GOTO(P_PROG, 3);
    draw_bar((int)(pct * 44), 44, pct);
    CLEAR_EOL();
    GOTO(P_PROG, UI_W); printf(BX_V);
}

static void player_update_channels(float fL, float aL, int noteL,
                                   float fR, float aR, int noteR) {
    // ---- Note row ----
    GOTO(P_NOTE, 2);
    if (aR > 0.f)
        printf(C_GRAY "Note:" C_RESET " " C_MAGENTA "%-3s" C_RESET
               C_GRAY " Freq:" C_RESET " " C_WHITE "%6.1f" C_RESET " Hz  ",
               note_name(noteR), fR);
    else
        printf(C_GRAY "Note: " C_DIM "---" C_RESET
               C_GRAY "  Freq:    " C_DIM "---" C_RESET "      ");

    GOTO(P_NOTE, P_DIV + 1);
    if (aL > 0.f)
        printf(C_GRAY " Note:" C_RESET " " C_MAGENTA "%-3s" C_RESET
               C_GRAY " Freq:" C_RESET " " C_WHITE "%6.1f" C_RESET " Hz  ",
               note_name(noteL), fL);
    else
        printf(C_GRAY " Note: " C_DIM "---" C_RESET
               C_GRAY "  Freq:    " C_DIM "---" C_RESET "      ");
    GOTO(P_NOTE, UI_W); printf(BX_V);

    // ---- Vel row ----
    GOTO(P_VEL, 2);
    printf(C_GRAY "Vol: " C_RESET);
    draw_bar((int)(aR / 0.8f * 16), 16, aR / 0.8f);

    GOTO(P_VEL, P_DIV + 1);
    printf(C_GRAY " Vol: " C_RESET);
    draw_bar((int)(aL / 0.8f * 16), 16, aL / 0.8f);
    GOTO(P_VEL, UI_W); printf(BX_V);
}

// ============================================================
//  Playback
// ============================================================

static int play_file(const char *path, float amplitude, PadState *pad) {
    const char *fname = strrchr(path, '/');
    fname = fname ? fname + 1 : path;

    MidiSong song;
    if (midi_load_file(&song, path) != 0) {
        consoleClear();
        GOTO(10, 5);
        printf(C_RED "ERROR: Cannot read: %s" C_RESET, fname);
        consoleUpdate(NULL);
        svcSleepThread(2000000000LL);
        return 0;
    }

    player_draw_static(fname, song.count, song.total_duration);
    player_update_status(fname, false);
    player_update_channels(160.f, 0.f, 0, 160.f, 0.f, 0);
    player_update_time(0.0, song.total_duration);
    consoleUpdate(NULL);

    float fL = 160.f, aL = 0.f, fR = 160.f, aR = 0.f;
    int   noteL = 0,  noteR = 0;
    bool  paused    = false;
    int   ei        = 0;

    double   tick_freq    = (double)armGetSystemTickFreq();
    uint64_t start_tick   = armGetSystemTick();
    uint64_t pause_tick   = 0;
    uint64_t last_display = 0;

    // Display refresh at ~30fps
    const uint64_t DISP_INT = (uint64_t)(tick_freq * 0.033);

    while (appletMainLoop()) {
        padUpdate(pad);
        vib_update_handles();
        uint64_t down = padGetButtonsDown(pad);

        if (down & HidNpadButton_Plus)  { stop_vibration(); midi_free(&song); return  1; }
        if (down & HidNpadButton_Minus) { stop_vibration(); midi_free(&song); return -1; }
        if (down & HidNpadButton_B)     { stop_vibration(); midi_free(&song); return  0; }

        if (down & HidNpadButton_A) {
            paused = !paused;
            if (paused) {
                stop_vibration();
                pause_tick = armGetSystemTick();
            } else {
                start_tick += armGetSystemTick() - pause_tick;
            }
            player_update_status(fname, paused);
            consoleUpdate(NULL);
        }

        if (!paused) {
            double playhead = (double)(armGetSystemTick() - start_tick) / tick_freq;

            bool ch_changed = false;
            while (ei < song.count && song.notes[ei].time_sec <= playhead) {
                MidiNote *n = &song.notes[ei++];
                if (n->channel == 0) {
                    if (n->velocity > 0) { fR = midi_note_to_freq(n->note); aR = (n->velocity/127.f)*amplitude; noteR = n->note; }
                    else { aR = 0.f; }
                    ch_changed = true;
                } else if (n->channel == 1) {
                    if (n->velocity > 0) { fL = midi_note_to_freq(n->note); aL = (n->velocity/127.f)*amplitude; noteL = n->note; }
                    else { aL = 0.f; }
                    ch_changed = true;
                }
            }

            send_vibration(fL, aL, fR, aR);

            uint64_t now_tick = armGetSystemTick();
            if (ch_changed || (now_tick - last_display) >= DISP_INT) {
                last_display = now_tick;
                player_update_time(playhead, song.total_duration);
                if (ch_changed)
                    player_update_channels(fL, aL, noteL, fR, aR, noteR);
                consoleUpdate(NULL);
            }

            if (ei >= song.count && playhead > song.total_duration) {
                stop_vibration();
                svcSleepThread(1500000000LL);
                break;
            }
        }

        svcSleepThread(8000000LL);  // 8ms
    }

    midi_free(&song);
    return 1;
}

// ============================================================
//  Browser
// ============================================================

static void browser(PadState *pad) {
    int  sel       = 0;
    int  view_top  = 0;
    bool dirty     = true;

    while (appletMainLoop()) {
        padUpdate(pad);
        vib_update_handles();

        int n = (int)g_files.size();
        uint64_t down = padGetButtonsDown(pad);

        if (down & HidNpadButton_B) break;

        if (n == 0) {
            if (down & HidNpadButton_Plus) {
                scan_files(); sel=0; view_top=0; dirty=true;
            }
        } else {
            if (down & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
                sel = (sel + 1) % n;
                dirty = true;
            }
            if (down & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
                sel = (sel - 1 + n) % n;
                dirty = true;
            }
            if (down & HidNpadButton_Plus) {
                scan_files(); n=(int)g_files.size(); sel=0; view_top=0; dirty=true;
            }
            if (down & HidNpadButton_A) {
                int cur = sel;
                while (appletMainLoop()) {
                    int res = play_file(g_files[cur].c_str(), 0.7f, pad);
                    if (!appletMainLoop()) break;
                    n = (int)g_files.size();
                    if (!n) break;
                    if (res ==  1) { cur=(cur+1)%n; continue; }
                    if (res == -1) { cur=(cur-1+n)%n; continue; }
                    break;
                }
                sel = n ? cur%n : 0;
                dirty = true;
            }
        }

        // Keep viewport in sync with selection
        if (sel < view_top)               { view_top = sel; dirty = true; }
        if (sel >= view_top + B_LIST_N)   { view_top = sel - B_LIST_N + 1; dirty = true; }

        if (dirty) {
            dirty = false;
            browser_draw(g_files, sel, view_top);
        }

        svcSleepThread(16000000LL);  // 16ms (~60fps polling)
    }
}

// ============================================================
//  Entry point
// ============================================================

int main(int argc, char *argv[]) {
    consoleInit(NULL);

    padConfigureInput(8, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    mkdir(APP_DIR, 0777);
    scan_files();

    browser(&pad);

    stop_vibration();
    consoleExit(NULL);
    return 0;
}