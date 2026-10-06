#include "imaging/ColorMap.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <map>
#include <mutex>
#include <vector>

namespace vtc {

namespace {

struct Stop {
    double t;  // 0..1
    double r, g, b;
};

// Piecewise-linear interpolation between colour stops.
std::array<std::uint32_t, 256> build(std::initializer_list<Stop> stops) {
    std::array<std::uint32_t, 256> table{};
    const std::vector<Stop> s(stops);
    for (int i = 0; i < 256; ++i) {
        const double t = i / 255.0;
        std::size_t k = 0;
        while (k + 2 < s.size() && t > s[k + 1].t) {
            ++k;
        }
        const Stop& a = s[k];
        const Stop& b = s[k + 1];
        const double f = b.t > a.t ? std::clamp((t - a.t) / (b.t - a.t), 0.0, 1.0) : 0.0;
        auto channel = [f](double x, double y) {
            return static_cast<std::uint32_t>(std::lround(std::clamp(x + (y - x) * f, 0.0, 255.0)));
        };
        table[static_cast<std::size_t>(i)] =
            0xFF000000u | (channel(a.r, b.r) << 16) | (channel(a.g, b.g) << 8) | channel(a.b, b.b);
    }
    return table;
}

std::array<std::uint32_t, 256> make(ColorMap map) {
    switch (map) {
        case ColorMap::Gray: return build({{0, 0, 0, 0}, {1, 255, 255, 255}});
        // DICOM "Hot Iron" (PS3.6 standard palette): red, then green, then blue rise.
        case ColorMap::HotIron:
            return build({{0, 0, 0, 0}, {0.5, 255, 0, 0}, {0.75, 255, 128, 0}, {1, 255, 255, 255}});
        // Nuclear-medicine style: black, blue, purple, red, yellow, white.
        case ColorMap::Pet:
            return build({{0, 0, 0, 0},
                          {0.2, 0, 0, 160},
                          {0.4, 128, 0, 200},
                          {0.6, 230, 0, 60},
                          {0.8, 255, 180, 0},
                          {1, 255, 255, 255}});
        case ColorMap::Rainbow:
            return build({{0, 0, 0, 131},
                          {0.125, 0, 0, 255},
                          {0.375, 0, 255, 255},
                          {0.625, 255, 255, 0},
                          {0.875, 255, 0, 0},
                          {1, 128, 0, 0}});
        case ColorMap::Bone:
            return build({{0, 0, 0, 0}, {0.375, 84, 84, 116}, {0.75, 167, 199, 199}, {1, 255, 255, 255}});
        case ColorMap::Copper: return build({{0, 0, 0, 0}, {0.8, 255, 160, 102}, {1, 255, 199, 127}});
        case ColorMap::Hot:
            return build({{0, 0, 0, 0}, {0.375, 255, 0, 0}, {0.75, 255, 255, 0}, {1, 255, 255, 255}});
        case ColorMap::Ice:
            return build({{0, 0, 0, 0}, {0.35, 0, 40, 160}, {0.7, 0, 200, 255}, {1, 255, 255, 255}});
    }
    return build({{0, 0, 0, 0}, {1, 255, 255, 255}});
}

}  // namespace

std::string colorMapName(ColorMap map) {
    switch (map) {
        case ColorMap::Gray: return "Tons de cinza";
        case ColorMap::HotIron: return "Ferro quente (Hot Iron)";
        case ColorMap::Pet: return "PET";
        case ColorMap::Rainbow: return "Arco-íris";
        case ColorMap::Bone: return "Osso";
        case ColorMap::Copper: return "Cobre";
        case ColorMap::Hot: return "Fogo";
        case ColorMap::Ice: return "Gelo";
    }
    return {};
}

const std::array<std::uint32_t, 256>& colorMapTable(ColorMap map) {
    static std::mutex mutex;
    static std::map<ColorMap, std::array<std::uint32_t, 256>> tables;
    std::lock_guard lock(mutex);
    auto it = tables.find(map);
    if (it == tables.end()) {
        it = tables.emplace(map, make(map)).first;
    }
    return it->second;
}

}  // namespace vtc
