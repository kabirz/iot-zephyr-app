// ============================================================
// Label Icons for SH1106 Display (8x16, content 8x10 centered)
// Format: page0 cols[0..7] then page1 cols[0..7]
// 2 pages x 8 columns = 16 bytes per icon
// ============================================================

#ifndef LABEL_ICONS_H
#define LABEL_ICONS_H

#include <stdint.h>

#define LABEL_ICON_W  8
#define LABEL_ICON_H  16
#define LABEL_ICON_PAGES  2
#define LABEL_ICON_SIZE  (16)

// label_can
//     ........
//     ........
//     ........
//     ########
//     ##...###
//     #.###.##
//     #.######
//     #.######
//     #.######
//     #.###.##
//     ##...###
//     ########
//     ########
//     ........
//     ........
//     ........
const uint8_t label_can[16] = {
        0xF8, 0x18, 0xE8, 0xE8, 0xE8, 0xD8, 0xF8, 0xF8, 0x1F, 0x1C, 0x1B, 0x1B, 0x1B, 0x1D, 0x1F, 0x1F
};

// label_rf24 (2.4G 无线图标)
//     ........
//     ........
//     ........
//     ########
//     #.######
//     #.######
//     #.######
//     #.######
//     #.######
//     #.######
//     #.....##
//     ########
//     ########
//     ........
//     ........
//     ........
const uint8_t label_rf24[16] = {
        0xF8, 0x08, 0xF8, 0xF8, 0xF8, 0xF8, 0xF8, 0xF8, 0x1F, 0x18, 0x1B, 0x1B, 0x1B, 0x1B, 0x1F, 0x1F
};

#endif // LABEL_ICONS_H