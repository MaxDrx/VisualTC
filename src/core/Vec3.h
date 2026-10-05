#pragma once

#include <cmath>

namespace vtc {

// 3D vector in patient coordinates (DICOM LPS: +x = Left, +y = Posterior,
// +z = Head). All geometry math in VisualTC is done in double precision.
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;

    constexpr Vec3() = default;
    constexpr Vec3(double xx, double yy, double zz) : x(xx), y(yy), z(zz) {}

    constexpr Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    constexpr Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    constexpr Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    constexpr Vec3 operator/(double s) const { return {x / s, y / s, z / s}; }
    constexpr Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }

    [[nodiscard]] constexpr double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    [[nodiscard]] constexpr Vec3 cross(const Vec3& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    [[nodiscard]] double norm() const { return std::sqrt(dot(*this)); }
    [[nodiscard]] Vec3 normalized() const {
        const double n = norm();
        return n > 0.0 ? *this / n : Vec3{};
    }
    [[nodiscard]] double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};

inline constexpr Vec3 operator*(double s, const Vec3& v) { return v * s; }

inline double distance(const Vec3& a, const Vec3& b) { return (a - b).norm(); }

// Rotates v around unit axis k by angle (radians), Rodrigues' formula.
inline Vec3 rotateAroundAxis(const Vec3& v, const Vec3& k, double angle) {
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    return v * c + k.cross(v) * s + k * (k.dot(v) * (1.0 - c));
}

}  // namespace vtc
