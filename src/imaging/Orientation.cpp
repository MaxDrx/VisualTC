#include "imaging/Orientation.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace vtc {

std::string orientationLetters(const Vec3& d, double threshold) {
    const Vec3 v = d.normalized();
    struct Axis {
        double magnitude;
        char letter;
    };
    std::array<Axis, 3> axes{{
        {std::abs(v.x), v.x >= 0.0 ? 'L' : 'R'},
        {std::abs(v.y), v.y >= 0.0 ? 'P' : 'A'},
        {std::abs(v.z), v.z >= 0.0 ? 'H' : 'F'},
    }};
    std::stable_sort(axes.begin(), axes.end(), [](const Axis& a, const Axis& b) { return a.magnitude > b.magnitude; });
    std::string out;
    for (const auto& a : axes) {
        if (a.magnitude >= threshold) {
            out.push_back(a.letter);
        }
    }
    return out;
}

std::string planeLabel(const Vec3& normal) {
    const Vec3 n = normal.normalized();
    const double ax = std::abs(n.x);
    const double ay = std::abs(n.y);
    const double az = std::abs(n.z);
    const double m = std::max({ax, ay, az});
    if (m < 0.8) {
        return "Oblíquo";
    }
    if (m == az) {
        return "Axial";
    }
    if (m == ay) {
        return "Coronal";
    }
    return "Sagital";
}

}  // namespace vtc
