#ifndef FONT_CHARMAPS_H
#define FONT_CHARMAPS_H
#include <string>

/** @brief Defines common frame to character mappings for Fonts. */
namespace FontCharmaps {
extern const unsigned M_SKIP;
const unsigned*
get_char_map(std::string name);
};

#endif
