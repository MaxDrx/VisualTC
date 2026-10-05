#pragma once

#include <string>

#include "core/Vec3.h"

namespace vtc {

// Patient orientation letters for a direction in LPS space:
// +x = L, -x = R, +y = P, -y = A, +z = H, -z = F.
// Components are listed by decreasing magnitude; components smaller than
// `threshold` are omitted (an axial image shows "L", an oblique one "LP").
std::string orientationLetters(const Vec3& direction, double threshold = 0.25);

// "Axial", "Coronal", "Sagital" or "Oblíquo" from a plane normal.
std::string planeLabel(const Vec3& normal);

}  // namespace vtc
