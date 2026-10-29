#include "render3d/Camera3D.h"

#include <algorithm>
#include <cmath>

#include "render3d/RenderVolume.h"

namespace vtc {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void Camera3D::setView(ViewDirection v) {
    // DICOM patient axes: +x left, +y posterior, +z head.
    switch (v) {
        case ViewDirection::Anterior:  // patient's right on the left of the image
            forward = {0, 1, 0};
            down = {0, 0, -1};
            break;
        case ViewDirection::Posterior:
            forward = {0, -1, 0};
            down = {0, 0, -1};
            break;
        case ViewDirection::Left:  // from the left side: face to the left of the image
            forward = {-1, 0, 0};
            down = {0, 0, -1};
            break;
        case ViewDirection::Right:
            forward = {1, 0, 0};
            down = {0, 0, -1};
            break;
        case ViewDirection::Superior:  // from above, anterior at the top
            forward = {0, 0, -1};
            down = {0, 1, 0};
            break;
        case ViewDirection::Inferior:  // from the feet, as the axial images
            forward = {0, 0, 1};
            down = {0, 1, 0};
            break;
    }
    right = down.cross(forward);
    orthonormalize();
}

void Camera3D::rotate(double horizontal, double vertical, double roll) {
    // Dragging to the right turns the front of the object to the right: the
    // camera orbits the opposite way around the target.
    if (horizontal != 0.0) {
        const Vec3 axis = down;
        right = rotateAroundAxis(right, axis, horizontal);
        forward = rotateAroundAxis(forward, axis, horizontal);
    }
    if (vertical != 0.0) {
        const Vec3 axis = right;
        down = rotateAroundAxis(down, axis, -vertical);
        forward = rotateAroundAxis(forward, axis, -vertical);
    }
    if (roll != 0.0) {
        const Vec3 axis = forward;
        right = rotateAroundAxis(right, axis, roll);
        down = rotateAroundAxis(down, axis, roll);
    }
    orthonormalize();
}

void Camera3D::pan(double dxPixels, double dyPixels) {
    target -= right * (dxPixels * mmPerPixel) + down * (dyPixels * mmPerPixel);
}

void Camera3D::zoom(double factor, double atX, double atY, int width, int height) {
    if (factor <= 0.0) {
        return;
    }
    const double before = mmPerPixel;
    mmPerPixel = std::clamp(mmPerPixel / factor, 1e-3, 1e3);
    if (width > 0 && height > 0) {
        // The point under the cursor stays where it is.
        const double ux = atX - 0.5 * width;
        const double uy = atY - 0.5 * height;
        target += right * (ux * (before - mmPerPixel)) + down * (uy * (before - mmPerPixel));
    }
}

void Camera3D::fit(const RenderVolume& volume, int width, int height) {
    target = volume.center();
    // Extent of the volume box as seen from this side.
    double ex = 0.0;
    double ey = 0.0;
    for (int c = 0; c < 8; ++c) {
        const Vec3 idx{(c & 1) != 0 ? volume.nx() - 1.0 : 0.0, (c & 2) != 0 ? volume.ny() - 1.0 : 0.0,
                       (c & 4) != 0 ? volume.nz() - 1.0 : 0.0};
        const Vec3 rel = volume.toPatient(idx) - target;
        ex = std::max(ex, 2.0 * std::abs(rel.dot(right)));
        ey = std::max(ey, 2.0 * std::abs(rel.dot(down)));
    }
    const double w = std::max(1, width);
    const double h = std::max(1, height);
    mmPerPixel = std::max(1e-3, std::max(ex / (0.9 * w), ey / (0.9 * h)));
}

Vec3 Camera3D::eye(int height) const {
    const double half = std::tan(0.5 * fovDegrees * kPi / 180.0);
    const double dist = 0.5 * height * mmPerPixel / std::max(1e-6, half);
    return target - forward * dist;
}

Ray Camera3D::ray(double px, double py, int width, int height) const {
    const double u = (px - 0.5 * width) * mmPerPixel;
    const double v = (py - 0.5 * height) * mmPerPixel;
    const Vec3 onPlane = target + right * u + down * v;
    if (perspective) {
        const Vec3 e = eye(height);
        return {e, (onPlane - e).normalized()};
    }
    // Far behind the target: the renderers clip the ray to the volume.
    return {onPlane - forward * 1e5, forward};
}

bool Camera3D::project(const Vec3& p, int width, int height, double& px, double& py) const {
    const Vec3 rel = p - target;
    double u = rel.dot(right);
    double v = rel.dot(down);
    if (perspective) {
        const Vec3 e = eye(height);
        const double dist = (target - e).dot(forward);
        const double depth = (p - e).dot(forward);
        if (depth <= 1e-6) {
            return false;
        }
        u *= dist / depth;
        v *= dist / depth;
        // Lateral offsets measured from the eye axis, which passes through the target.
    }
    px = u / mmPerPixel + 0.5 * width;
    py = v / mmPerPixel + 0.5 * height;
    return true;
}

bool Camera3D::operator==(const Camera3D& o) const {
    auto same = [](const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
    return same(right, o.right) && same(down, o.down) && same(forward, o.forward) && same(target, o.target) &&
           mmPerPixel == o.mmPerPixel && perspective == o.perspective && fovDegrees == o.fovDegrees;
}

void Camera3D::orthonormalize() {
    forward = forward.normalized();
    right = (right - forward * right.dot(forward)).normalized();
    down = forward.cross(right);
}

}  // namespace vtc
