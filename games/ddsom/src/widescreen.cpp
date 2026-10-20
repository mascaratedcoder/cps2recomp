// SPDX-License-Identifier: GPL-3.0-only
//
// Dungeons & Dragons: Shadow over Mystara: an experimental widescreen. The picture is drawn 512 columns wide where
// the board shows 384 (16:9 for 4:3): 64 more columns on each side (video/video.h, Video::wide). Only what is drawn
// changes. The game does not know of the extra width, so what it keeps only while it is in the board's picture
// (enemies, the 8-pixel scroll screen's map, objects it hides beyond the edge) is not there in the added columns
// until the sources of this game say where it is kept: this is a first look, not a finished feature.
#include "game.h"
#include "lift.h"
#include "machine.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace cps2recomp {

namespace {
    constexpr int kSide = Video::kMaxSide;

    // How the picture is made: the board's own, or wide.
    enum class Picture { Board, Wide };

    // The screens that are told apart by what is in their scroll screens' maps (the picture they show is fixed data in
    // the ROM, so the same in any play): the checksums of the 16-pixel and the 32-pixel screens' maps, found by watching
    // each of them over a run. The character choice is wide, the story and the name entry are the board's (they are whole
    // pictures at the board's width, and only repeat when made wider). Any other screen is wide but for the intro and the
    // title, which a word of the game's (work RAM FF0D58) keeps at the board's: it is 0 there and takes other values later
    // in a play, so it is only ever looked at to keep a screen narrow that these two are.
    struct Known {
        uint32_t map16, map32; // (map16 0: any: the screen's 16-pixel map is not always the same)
        bool onlyStatic; // only while the three scroll screens are at the board's left edge (not scrolled)
        Picture picture;
        const char *name;
    };
    constexpr Known kKnown[] = {
        {0xEFFF368E, 0x19787C52, false, Picture::Wide, "character choice"},
        {0x6423027F, 0x1D204EEA, false, Picture::Board, "story"},
        {0, 0x0D136498, true, Picture::Board, "name entry"},
        {0, 0xD912A916, false, Picture::Board, "story text between stages"},
        {0, 0x8E75098C, false, Picture::Wide, "opening title"},
        {0, 0x88A341E4, false, Picture::Wide, "title"},
    };
    constexpr uint32_t kMode = 0xFF0D58;
    // The camera's place in a scroll screen's own plane, a word in work RAM for each (0 at a stage's start)
    constexpr uint32_t kCamera16 = 0xFF850E, kCamera32 = 0xFF85AE;

    uint32_t MapChecksum(const Machine &m, int which) {
        const uint8_t *map = &m.videoRam[(static_cast<uint32_t>(m.cpsA[1 + which]) * 256) & ~0x3FFFu & 0x3FFFF];
        return static_cast<uint32_t>(crc32(0, map, 0x4000));
    }
    Picture PictureNow(const Machine &m, const char *&name) {
        const uint32_t map16 = MapChecksum(m, 1), map32 = MapChecksum(m, 2);
        // (the dragon rising and the flame that ends in the title: the cave's own screen, scrolled up, and then the
        // maps that follow it; it is the board's, up to where the game's mode word leaves 0 (the title) or the screen is
        // another)
        if (map32 == 0xA6A5FFF8 && Ram16(kMode) != 0) {
            name = "opening title";
            return Picture::Wide;
        }
        static bool dragon = false;
        if (map32 != 0xA6A5FFF8 || Ram16(kMode) != 0) {
            dragon = false;
        } else if (map16 != 0x563BC752 && map16 != 0) {
            dragon = true;
        }
        // (the flame fills the board: from the map the logo's flash is drawn in, the picture grows to the wide one)
        if (dragon && map16 == 0x4EA29F7B) {
            name = "opening title";
            return Picture::Wide;
        }
        if (dragon) {
            name = "opening dragon";
            return Picture::Board;
        }
        const bool still = m.cpsA[6] == 0xFFC0 && m.cpsA[8] == 0xFFC0 && m.cpsA[10] == 0xFFC0;
        // (the name entry: its maps are not always the same ones, but the game's mode word is 4 on it and the scroll screens
        // are at the board's left edge; in the scene before it, which asks for the name, the word is 4 and they are scrolled)
        // (the scenes of story between the stages, and the screen of text there: the word is 3, or 0xA4A in the middle
        // of them, and the scroll screens are at the board's left edge; their maps change from one to the next)
        if (still && (Ram16(kMode) == 3 || Ram16(kMode) == 0x0A4A)) {
            name = "story between stages";
            return Picture::Board;
        }
        if (still && Ram16(kMode) == 4) {
            name = "name entry";
            return Picture::Board;
        }
        for (const Known &known : kKnown) {
            if ((!known.map16 || known.map16 == map16) && known.map32 == map32 && (still || !known.onlyStatic)) {
                name = known.name;
                return known.picture;
            }
        }
        name = "";
        return Ram16(kMode) == 0 ? Picture::Board : Picture::Wide;
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
    constexpr int kRestartColumns = 24;
    Machine *g_machine = nullptr;

    // The title's flame is drawn in objects (the scroll screens hold the dragon behind it): for the added columns the
    // objects within a margin's width of the board's edge are drawn again mirrored at that edge, behind the others.
    // (and the burning village's flames along the horizon, which the game puts only inside the board's picture: their
    // tiles are one run of the graphics)
    bool g_titleFlame = false;
    void MirrorObjects(uint16_t *list, int &last) {
        if (!g_machine) {
            return;
        }
        const int regX = g_machine->objectRegs[4];
        int end = last + 4;
        for (int i = 0; i <= last && end + 8 <= 0x1000; i += 4) {
            const uint16_t attr = list[i + 3];
            if (list[i + 1] >= 0x8000 || attr >= 0xFF00) {
                continue;
            }
            // (the title's flame is in colour sets 16 and up; the words, the logo and the rest at the sides are not repeated)
            const uint32_t code = list[i + 2];
            if (g_titleFlame ? (attr & 0x1F) < 0x10 : code < 0xBD00 || code > 0xBEFF || (attr & 0x1F) != 0x1D) {
                continue;
            }
            const int offset = attr & 0x80 ? 64 : 64 - regX;
            const int across = ((attr >> 8) & 0x0F) + 1, width = across * 16;
            const int at = (list[i] + offset) & 0x3FF;
            const int raster = at >= 0x200 ? at - 0x400 : at;
            // (the board's picture is raster columns 64 to 447)
            for (int edge : {64, 448}) {
                const int mirrored = 2 * edge - raster - width;
                const bool inMargin = edge == 64 ? mirrored < edge && mirrored + width > edge - kSide
                                                 : mirrored + width > edge && mirrored < edge + kSide;
                const bool near = edge == 64 ? raster < edge + kSide && raster + width > edge
                                             : raster + width > edge - kSide && raster < edge;
                if (!near || !inMargin) {
                    continue;
                }
                list[end] = static_cast<uint16_t>((list[i] & ~0x3FF) | ((mirrored - offset) & 0x3FF));
                list[end + 1] = list[i + 1];
                list[end + 2] = list[i + 2];
                list[end + 3] = attr ^ 0x20;
                end += 4;
            }
        }
        last = end - 4;
    }

    const uint8_t *MapOf(const Machine &m, int which) {
        return &m.videoRam[(static_cast<uint32_t>(m.cpsA[1 + which]) * 256) & ~0x3FFFu & 0x3FFFF];
    }
    // The map's columns that are the board's picture (the first and the last, which may be partly in it)
    void BoardColumns(const Machine &m, int which, int &first, int &last) {
        const int shift = g_screens[which - 1].shift, scrollX = m.cpsA[6 + which * 2];
        first = ((scrollX + kSide) >> shift) & 63;
        last = ((scrollX + kSide + Video::kWidth - 1) >> shift) & 63;
    }

    // Whether a column of a map is one tile all down (what clearing the map writes)
    bool ColumnIsOneTile(const Screen &screen, const uint8_t *map, int column) {
        const uint8_t *first = &map[static_cast<size_t>(column) * screen.entries * 4];
        for (int g = 0; g < screen.groups; g++) {
            const uint8_t *at = &map[(static_cast<size_t>(column) * screen.entries + static_cast<size_t>(g) * screen.stride) * 4];
            for (int e = 0; e < screen.entries; e++) {
                if (memcmp(at + e * 4, first, 4)) {
                    return false;
                }
            }
        }
        return true;
    }

    void WatchMaps(Machine &machine) {
        for (int which = 1; which <= 2; which++) {
            Screen &screen = g_screens[which - 1];
            const uint8_t *map = MapOf(machine, which);
            const size_t columnBytes = static_cast<size_t>(screen.entries) * 4;
            bool changed[64] = {};
            int changes = 0;
            if (screen.before.size() == 0x10000) {
                for (int c = 0; c < 64; c++) {
                    for (int g = 0; g < screen.groups; g++) {
                        const size_t at = (static_cast<size_t>(c) * screen.entries + static_cast<size_t>(g) * screen.stride) * 4;
                        if (memcmp(&screen.before[at], &map[at], columnBytes)) {
                            changed[c] = true;
                        }
                    }
                    changes += changed[c];
                }
            }
            // (a scene begins with the screen's map written over, nearly all of it at once: what was learned of the
            // one before is of no use. A frame that changes this much starts again; the columns that change from the
            // frames after it are the ones the game writes for this scene)
            const bool restart = screen.before.size() != 0x10000 || changes >= kRestartColumns;
            if (restart) {
                if (screen.before.size() == 0x10000 && getenv("DDSOM_WIDE_LOG")) {
                    fprintf(stderr, "widescreen: frame %llu map of screen %d written over (%d columns changed)\n",
                            static_cast<unsigned long long>(machine.frame), which + 1, changes);
                }
                // (a column that was rewritten and has art in it was written for this scene, and is trusted; one that is
                // one tile all down, the blank the game clears the map to, was not. A column the rewrite did not touch is
                // what it was: the game wrote it, or did not, as far as this knew before, and a rewrite that leaves a
                // column as it is does not make it any the less the game's)
                const bool had = screen.before.size() == 0x10000;
                for (int c = 0; c < 64; c++) {
                    if (!had) {
                        screen.written[c] = false;
                    } else if (changed[c]) {
                        screen.written[c] = !ColumnIsOneTile(screen, map, c);
                    }
                }
            } else {
                for (int c = 0; c < 64; c++) {
                    screen.written[c] = screen.written[c] || changed[c];
                }
            }
            screen.before.assign(map, map + 0x10000);
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


    // The 32-pixel scroll screen (the floor and what stands on it: fences, mountains) keeps its map up to one column
    // beyond the board's right edge, where the 16-pixel one keeps three: the picture's added columns would have to copy
    // a column or two, and a fence or a ridge there would come and go as the camera moves. So, with the widescreen on,
    // the game's own routine that draws the next column of that screen's map is asked for two more, between frames:
    // its dispatcher (0x328C0) draws a column on the right while the camera is 32 pixels or more ahead of the place it
    // has drawn to (the plane's word at +0x1C), so told the camera is 64 pixels further on it draws one or two. Then
    // all it keeps (the plane's record, the registers, the time) is put back as it was: the game does not know, and
    // draws the same columns again when it comes to them. Only while the plane is in step with the picture's
    // scroll register (its place is the register's and 64 more, to a few pixels) and has no work of its own to do.
    // (frames in a row the picture has been wide and none of the screens told apart by their maps: a stage's scene)
    int g_sceneFrames = 0;
    constexpr int kSceneFramesBefore = 60;
    constexpr uint32_t kPlane32 = 0xFF854E, kPlaneSize = 0x80, kDispatch32 = 0x0328C0;
    constexpr int kAhead = 0x40;
    void DrawAhead(Machine &machine) {
        // (not in the first moments of a scene, nor on a screen of another kind: the plane's record is of the last stage
        // until the game sets it up again, and drawing from it would write that stage's columns into the other screen's map)
        if (!FeatureOn("widescreen") || !g_machine || g_sceneFrames < kSceneFramesBefore) {
            return;
        }
        const uint16_t camera = Ram16(kPlane32 + 8), drawnTo = Ram16(kPlane32 + 0x10);
        const int behind = static_cast<int16_t>(camera - drawnTo), down = static_cast<int16_t>(Ram16(kPlane32 + 0x1E));
        // (the plane's first word is 0x200 for the plane of the floor of the stages the routine was seen to keep right
        // in; the third stage's has other bits set and the routine draws over the board's own columns there)
        if (Ram16(kPlane32) != 0x0200) {
            return;
        }
        if (std::abs(static_cast<int16_t>(camera - machine.cpsA[10] - 64)) > 8 || behind <= -0x20 || behind >= 0x20 || down <= -0x20 || down >= 0x20) {
            return;
        }
        M68K &cpu = machine.cpu;
        const M68K::Kept kept = cpu.Keep();
        const uint64_t cycles = cpu.cycles;
        const unsigned busWait = cpu.busWait;
        const uint32_t pc = cpu.pc;
        uint8_t record[kPlaneSize];
        memcpy(record, Ram(kPlane32), kPlaneSize);
        cpu.busWait = 0;
        cpu.a[0] = kPlane32;
        cpu.a[5] = 0xFF8000;
        SetRam16(kPlane32 + 0x1C, static_cast<uint16_t>(behind + kAhead));
        Call(cpu, kDispatch32);
        memcpy(Ram(kPlane32), record, kPlaneSize);
        cpu.PutBack(kept);
        cpu.cycles = cycles;
        cpu.busWait = busWait;
        cpu.pc = pc;
    }

    // What the picture asks (Video::wide.tile) for a tile outside the board's own columns
    bool SideTile(int which, int column, int row, int at, uint16_t &code, uint16_t &attributes) {
        if (which < 1 || which > 2 || !g_machine) {
            return false;
        }
        const Machine &m = *g_machine;
        Screen &screen = g_screens[which - 1];
        int first, last;
        BoardColumns(m, which, first, last);
        const bool leftSide = at < kSide;
        // (the columns left of the board are of the stage only as far as the camera is from the stage's left end: at its
        // very start the game has written there what its plan has before the stage, which is other pictures. So only so
        // many of them are trusted: the camera's place, the word the game keeps for the screen's plane, over the tile's width)
        const int tileWidth = 1 << screen.shift;
        const int trusted = std::min<int>(Ram16(which == 1 ? kCamera16 : kCamera32) / tileWidth, 8);
        auto good = [&](int c) {
            const int away = (first - c) & 63; // (columns to the left of the board's first: 1, 2...)
            return screen.written[c] && !(leftSide && away >= 1 && away <= 8 && away > trusted);
        };
        if (good(column)) {
            return false;
        }
        // (how far from the board's picture the column is, on its side, and the column the good ones end at there)
        int edge, step;
        if (leftSide) {
            edge = first;
            step = -1;
        } else {
            edge = last;
            step = 1;
        }
        for (int n = 0; n < 8 && good((edge + step) & 63); n++) {
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
        g_machine = &machine;
        // (which screen it is is worked out from the maps as the game left them, before any column is drawn ahead)
        const char *screen = "";
        const Picture picture = PictureNow(machine, screen);
        g_sceneFrames = picture == Picture::Wide && !screen[0] ? g_sceneFrames + 1 : 0;
        DrawAhead(machine);
        WatchMaps(machine);
        // (the title's flame is the whole picture: what the maps hold outside the board's is the dragon before it, not
        // a thing to show at the sides, which take the flame's edge, mirrored)
        if (!strcmp(screen, "opening title")) {
            for (int which = 1; which <= 2; which++) {
                int first, last;
                BoardColumns(machine, which, first, last);
                for (int c = 0; c < 64; c++) {
                    const bool inBoard = first <= last ? c >= first && c <= last : c >= first || c <= last;
                    g_screens[which - 1].written[c] = g_screens[which - 1].written[c] && inBoard;
                }
            }
        }
        // (a log of what the picture was made of, beside settings.ini: for finding out why a screen is not told apart. It
        // has every change of how the picture is made, and the checksums of the maps and the scroll positions about every
        // two seconds; for the work on this port, to be left out when it is done)
        {
            static FILE *log = [] {
                std::string path = SettingsPath();
                const size_t slash = path.find_last_of('/');
                path = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + "ddsom-widescreen.log";
                return fopen(path.c_str(), "w");
            }();
            static int last = -1;
            const int now = static_cast<int>(picture) | (screen[0] << 8);
            if (log && (now != last || machine.frame % 20 == 0)) {
                // (how many of the added columns each side are not the game's own, for the two scroll screens)
                int mirrored[2][2] = {};
                for (int which = 1; which <= 2; which++) {
                    int first, last;
                    BoardColumns(machine, which, first, last);
                    const int across = kSide >> g_screens[which - 1].shift;
                    for (int d = 1; d <= across; d++) {
                        mirrored[which - 1][0] += !g_screens[which - 1].written[(first - d) & 63];
                        mirrored[which - 1][1] += !g_screens[which - 1].written[(last + d) & 63];
                    }
                }
                if (machine.frame % 600 == 0) {
                    for (int which = 1; which <= 2; which++) {
                        int first, last;
                        BoardColumns(machine, which, first, last);
                        std::string flags;
                        for (int c = 0; c < 64; c++) {
                            flags += c == first ? '[' : c == last ? ']' : g_screens[which - 1].written[c] ? 'w' : '.';
                        }
                        fprintf(log, "  map %d first %d last %d scroll %04X plane x %d drawn-to %d: %s\n", which + 1, first, last,
                                machine.cpsA[6 + which * 2], static_cast<int16_t>(Ram16(which == 1 ? kCamera16 : kCamera32)),
                                static_cast<int16_t>(Ram16((which == 1 ? kCamera16 : kCamera32) + 8)), flags.c_str());
                    }
                }
                fprintf(log, "mirrored left/right 16px %d/%d 32px %d/%d | ", mirrored[0][0], mirrored[0][1], mirrored[1][0], mirrored[1][1]);
                fprintf(log, "frame %llu %s: %s%s map16 %08X map32 %08X scroll x %04X %04X %04X y %04X %04X %04X mode %04X\n",
                        static_cast<unsigned long long>(machine.frame),
                        picture == Picture::Wide ? "wide" : "board's",
                        screen[0] ? screen : "not told apart", now != last ? " [CHANGE]" : "", MapChecksum(machine, 1), MapChecksum(machine, 2),
                        machine.cpsA[6], machine.cpsA[8], machine.cpsA[10], machine.cpsA[7], machine.cpsA[9], machine.cpsA[11], Ram16(kMode));
                fflush(log);
                last = now;
            }
        }
        // (the title's flame widens the picture a little each frame; anything else is at its width at once)
        // (it begins when the flame has filled the board, which takes the first frames of the title's map)
        static int flameFrames = 0;
        flameFrames = !strcmp(screen, "opening title") ? flameFrames + 1 : 0;
        constexpr int kFlameFills = 58, kGrowth = 2;
        const int grown = flameFrames ? std::clamp((flameFrames - kFlameFills) * kGrowth, 0, +kSide) : picture == Picture::Board ? 0 : kSide;
        machine.video.wide.side = FeatureOn("widescreen") && picture != Picture::Board ? grown : 0;
        // (where the stage ends short of the wide picture nothing is drawn: the line's own picture is mirrored there)
        machine.video.wide.fillHoles = true;
        // (the panels at the top and bottom are the 8-pixel scroll screen: they are not mirrored into the margins)
        machine.video.wide.fillLayers = 6;
        g_machine = &machine;
        machine.video.wide.tile = machine.video.wide.side ? SideTile : nullptr;
        g_titleFlame = !strcmp(screen, "opening title");
        machine.video.objectsTaken = machine.video.wide.side == kSide || g_titleFlame ? MirrorObjects : nullptr;
    };
}

} // namespace cps2recomp
