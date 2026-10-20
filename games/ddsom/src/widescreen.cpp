// SPDX-License-Identifier: GPL-3.0-only
//
// Dungeons & Dragons: Shadow over Mystara: an experimental widescreen. The picture is drawn 512 columns wide where
// the board shows 384 (16:9 for 4:3): 64 more columns on each side (video/video.h, Video::wide). Only what is drawn
// changes. The game does not know of the extra width, so what it keeps only while it is in the board's picture
// (enemies, the 8-pixel scroll screen's map, objects it hides beyond the edge) is not there in the added columns
// until the sources of this game say where it is kept: this is a first look, not a finished feature.
#include "game.h"
#include "machine.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cps2recomp {

namespace {
    constexpr int kSide = Video::kMaxSide;

    // The game's mode, a word in work RAM (found by watching it over a long run of made-up play): 0 in the intro, the
    // title and the character choice, 0x10 while a stage is played, and 4, 3 and others in the high score entry and the
    // cutscenes between stages. A second word is 0xFFFF on the character choice and the name entry, the screens laid
    // out for the board's 384 columns with text and a banner: those are the board's. Every other screen is a scene of
    // a stage and wide. (That word is also 0xFFFF for a few frames now and then in a stage, counts down from 7 and is 1
    // on the continue screen: so it is not looked at in a stage, and elsewhere it must stay 0xFFFF for a few frames, so
    // that a blip does not change the picture's shape.)
    constexpr uint32_t kMode = 0xFF0D58, kPlainScreen = 0xFF0B38;
    constexpr uint16_t kModeStage = 0x10, kPlainValue = 0xFFFF;
    constexpr int kPlainFrames = 6;
    int g_plainFor = 0;
    bool WideScreen() {
        const uint16_t mode = Ram16(kMode);
        if (mode == kModeStage) {
            g_plainFor = 0;
            return true;
        }
        g_plainFor = Ram16(kPlainScreen) == kPlainValue ? g_plainFor + 1 : 0;
        return mode != 0 && g_plainFor < kPlainFrames;
    }

    // The scroll screens of 16 and 32 pixels' tiles have maps of 64 columns, and the game writes into them only what its
    // camera is about to show: the rest is what was there before (the end of the stage before, say). The picture's added
    // columns would show that. So the sources watch which columns the game writes in a scene (a column read as changed
    // since the scene began, or in the board's own picture), and a column outside the board's that it has not written
    // is taken from the nearest one it has, mirrored.
    struct Screen {
        int shift;           // tile size as a power of two
        int groups, entries; // a column's tiles: this many runs of this many tiles ...
        int stride;          // ... that far apart (in tiles)
        std::vector<uint8_t> before;
        bool written[64] = {};
    };
    Screen g_screens[2] = {{4, 4, 16, 1024, {}, {}}, {5, 8, 8, 512, {}, {}}};
    int g_modeBefore = -1;
    Machine *g_machine = nullptr;

    const uint8_t *MapOf(const Machine &m, int which) {
        return &m.videoRam[(static_cast<uint32_t>(m.cpsA[1 + which]) * 256) & ~0x3FFFu & 0x3FFFF];
    }
    // The map's columns that are the board's picture (the first and the last, which may be partly in it)
    void BoardColumns(const Machine &m, int which, int &first, int &last) {
        const int shift = g_screens[which - 1].shift, scrollX = m.cpsA[6 + which * 2];
        first = ((scrollX + kSide) >> shift) & 63;
        last = ((scrollX + kSide + Video::kWidth - 1) >> shift) & 63;
    }

    void WatchMaps(Machine &machine) {
        const int mode = Ram16(kMode);
        const bool restart = mode != g_modeBefore;
        g_modeBefore = mode;
        for (int which = 1; which <= 2; which++) {
            Screen &screen = g_screens[which - 1];
            const uint8_t *map = MapOf(machine, which);
            const size_t columnBytes = static_cast<size_t>(screen.entries) * 4;
            if (restart || screen.before.size() != 0x10000) {
                screen.before.assign(map, map + 0x10000);
                std::fill(std::begin(screen.written), std::end(screen.written), false);
            } else {
                for (int c = 0; c < 64; c++) {
                    for (int g = 0; g < screen.groups; g++) {
                        const size_t at = (static_cast<size_t>(c) * screen.entries + static_cast<size_t>(g) * screen.stride) * 4;
                        if (memcmp(&screen.before[at], &map[at], columnBytes)) {
                            screen.written[c] = true;
                            memcpy(&screen.before[at], &map[at], columnBytes);
                        }
                    }
                }
            }
            int first, last;
            BoardColumns(machine, which, first, last);
            for (int c = first;; c = (c + 1) & 63) {
                screen.written[c] = true;
                if (c == last) {
                    break;
                }
            }
        }
    }

    // What the picture asks (Video::wide.tile) for a tile outside the board's own columns
    bool SideTile(int which, int column, int row, int at, uint16_t &code, uint16_t &attributes) {
        if (which < 1 || which > 2 || !g_machine) {
            return false;
        }
        const Machine &m = *g_machine;
        Screen &screen = g_screens[which - 1];
        if (screen.written[column]) {
            return false;
        }
        int first, last;
        BoardColumns(m, which, first, last);
        // (how far from the board's picture the column is, on its side, and the column the written ones end at there)
        int edge, step;
        if (at < kSide) {
            edge = first;
            step = -1;
        } else {
            edge = last;
            step = 1;
        }
        for (int n = 0; n < 8 && screen.written[(edge + step) & 63]; n++) {
            edge = (edge + step) & 63;
        }
        const int distance = ((column - edge) * step) & 63; // 1 at the first column that was not written
        if (distance < 1 || distance > 8) {
            return false;
        }
        const int mirror = (edge - step * (distance - 1)) & 63;
        const uint8_t *map = MapOf(m, which);
        const uint32_t index = which == 1 ? static_cast<uint32_t>((row & 0xF) + (mirror << 4) + ((row & 0x30) << 6))
                                          : static_cast<uint32_t>((row & 7) + (mirror << 3) + ((row & 0x38) << 6));
        code = static_cast<uint16_t>((map[index * 4] << 8) | map[index * 4 + 1]);
        attributes = static_cast<uint16_t>(((map[index * 4 + 2] << 8) | map[index * 4 + 3]) ^ 0x20); // (turned over)
        return true;
    }
}

void RegisterGame() {
    TheGame().widestPicture = [] { return Video::kWidth + (FeatureOn("widescreen") ? 2 * kSide : 0); };
    TheGame().features.push_back({"widescreen", "Display", "Widescreen",
                                  "The picture is shown in 16:9, with more of the scene on each side than the board shows. "
                                  "Experimental: the added columns show only what the game already keeps there.",
                                  kFeatureFirstPage});
    TheGame().beforeFrame = [](Machine &machine) {
        WatchMaps(machine);
        const bool wideNow = WideScreen(); // (once a frame: it counts)
        if (static const bool log = getenv("DDSOM_WIDE_LOG") != nullptr; log) {
            // (for finding out why the picture changes shape: every change of the two words that decide it)
            static unsigned last = ~0u;
            const unsigned now = (Ram16(kMode) << 16) | Ram16(kPlainScreen);
            if (now != last) {
                fprintf(stderr, "widescreen: frame %llu mode %04X word %04X: %s\n", static_cast<unsigned long long>(machine.frame),
                        Ram16(kMode), Ram16(kPlainScreen), wideNow ? "wide" : "board's");
                last = now;
            }
        }
        machine.video.wide.side = FeatureOn("widescreen") && wideNow ? kSide : 0;
        // (where the stage ends short of the wide picture nothing is drawn: the line's own picture is mirrored there)
        machine.video.wide.fillHoles = true;
        // (the panels at the top and bottom are the 8-pixel scroll screen: they are not mirrored into the margins)
        machine.video.wide.fillLayers = 6;
        g_machine = &machine;
        machine.video.wide.tile = machine.video.wide.side ? SideTile : nullptr;
    };
}

} // namespace cps2recomp
