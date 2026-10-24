// 5x7 bitmap font: 7 rows of 5 bits (bit 4 = leftmost pixel). Letters are drawn upper case. Used for the name tags.
#pragma once
#include <cctype>
#include <cstdint>

namespace psx {

inline const uint8_t* glyph5x7(char c) {
    static const uint8_t kSpace[7] = {0, 0, 0, 0, 0, 0, 0};
    static const uint8_t kDigits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},   {14, 17, 1, 2, 4, 8, 31},    {30, 1, 1, 14, 1, 1, 30},   {2, 6, 10, 18, 31, 2, 2},
        {31, 16, 30, 1, 1, 17, 14},   {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8},      {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 2, 12}};
    static const uint8_t kLetters[26][7] = {
        {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14}, {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31},
        {31, 16, 16, 30, 16, 16, 16}, {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},     {7, 2, 2, 2, 2, 18, 12},
        {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31}, {17, 27, 21, 21, 17, 17, 17}, {17, 17, 25, 21, 19, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
        {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17}, {15, 16, 16, 14, 1, 1, 30},  {31, 4, 4, 4, 4, 4, 4},
        {17, 17, 17, 17, 17, 17, 14}, {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},  {17, 17, 10, 4, 4, 4, 4},
        {31, 1, 2, 4, 8, 16, 31}};
    static const uint8_t kDash[7] = {0, 0, 0, 31, 0, 0, 0}, kUnder[7] = {0, 0, 0, 0, 0, 0, 31}, kDot[7] = {0, 0, 0, 0, 0, 0, 4},
                         kColon[7] = {0, 4, 4, 0, 4, 4, 0}, kSlash[7] = {1, 1, 2, 4, 8, 16, 16}, kRight[7] = {8, 4, 2, 1, 2, 4, 8},
                         kLeft[7] = {2, 4, 8, 16, 8, 4, 2}, kBang[7] = {4, 4, 4, 4, 4, 0, 4}, kUnknown[7] = {14, 17, 2, 4, 4, 0, 4};
    if (c >= '0' && c <= '9') return kDigits[c - '0'];
    c = char(std::toupper(static_cast<unsigned char>(c)));
    if (c >= 'A' && c <= 'Z') return kLetters[c - 'A'];
    switch (c) {
    case ' ': return kSpace;
    case '-': return kDash;
    case '_': return kUnder;
    case '.': return kDot;
    case ':': return kColon;
    case '/': return kSlash;
    case '>': return kRight;
    case '<': return kLeft;
    case '!': return kBang;
    default: return kUnknown;
    }
}

}  // namespace psx
