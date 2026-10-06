#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace vtc {

// Pseudo-colour tables applied to greyscale images after window/level
// (display only: values, measurements and HU are never changed).
enum class ColorMap { Gray, HotIron, Pet, Rainbow, Bone, Copper, Hot, Ice };

inline constexpr std::array<ColorMap, 8> kColorMaps{ColorMap::Gray,   ColorMap::HotIron, ColorMap::Pet,
                                                   ColorMap::Rainbow, ColorMap::Bone,    ColorMap::Copper,
                                                   ColorMap::Hot,     ColorMap::Ice};

// Name shown to the user (pt-BR).
std::string colorMapName(ColorMap map);

// 256 entries 0xFFRRGGBB, index = displayed grey level (0 black .. 255 white).
const std::array<std::uint32_t, 256>& colorMapTable(ColorMap map);

}  // namespace vtc
