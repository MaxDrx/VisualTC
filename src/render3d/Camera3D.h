#pragma once

#include "core/Vec3.h"

namespace vtc {

class RenderVolume;

// Standard views of the 3D reconstruction, named by where the observer
// stands (an anterior view looks at the face of the patient).
enum class ViewDirection { Anterior, Posterior, Left, Right, Superior, Inferior };

struct Ray {
    Vec3 origin;
    Vec3 dir;  // unit length
};

// Camera of the 3D view in patient coordinates. Screen axes: `right` to the
// right of the image, `down` to its bottom, `forward` into the screen (the
// direction the rays travel), a right-handed set.
class Camera3D {
public:
    Vec3 right{1, 0, 0};
    Vec3 down{0, 0, -1};
    Vec3 forward{0, 1, 0};
    Vec3 target;              // patient point at the centre of the image
    double mmPerPixel = 1.0;  // image scale at the target
    bool perspective = false;
    double fovDegrees = 30.0;  // vertical field of view in perspective

    void setView(ViewDirection v);
    // Turns the scene: horizontal and vertical angles in radians around the
    // screen axes through the target, then a roll around the view axis.
    void rotate(double horizontal, double vertical, double roll = 0.0);
    void pan(double dxPixels, double dyPixels);
    void zoom(double factor, double atX = 0.0, double atY = 0.0, int width = 0, int height = 0);
    // Volume centred, filling about 85% of the smaller side of the image.
    void fit(const RenderVolume& volume, int width, int height);

    [[nodiscard]] Ray ray(double px, double py, int width, int height) const;
    // Patient point -> pixel coordinates (false behind the eye in perspective).
    [[nodiscard]] bool project(const Vec3& p, int width, int height, double& px, double& py) const;
    [[nodiscard]] Vec3 eye(int height) const;  // perspective only
    [[nodiscard]] bool operator==(const Camera3D& o) const;

private:
    void orthonormalize();
};

}  // namespace vtc
