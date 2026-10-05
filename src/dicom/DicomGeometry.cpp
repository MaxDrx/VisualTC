#include "dicom/DicomGeometry.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace vtc {

std::string describe(GeometryIssue issue) {
    switch (issue) {
        case GeometryIssue::MissingGeometry:
            return "Informação geométrica (posição, orientação ou espaçamento) ausente em parte das imagens.";
        case GeometryIssue::MixedOrientation:
            return "As imagens da série não são paralelas entre si.";
        case GeometryIssue::MixedSize:
            return "As imagens da série possuem dimensões ou espaçamento de pixel diferentes.";
        case GeometryIssue::MixedFrameOfReference:
            return "As imagens pertencem a sistemas de coordenadas (Frame of Reference) diferentes.";
        case GeometryIssue::DuplicatePositions:
            return "Há cortes duplicados na mesma posição espacial.";
        case GeometryIssue::MissingSlices:
            return "A série possui cortes ausentes ou geometria inconsistente. Algumas reconstruções podem não ser "
                   "confiáveis.";
        case GeometryIssue::IrregularSpacing:
            return "O espaçamento entre cortes é irregular; o MPR usa as posições reais de cada corte.";
        case GeometryIssue::GantryTilt:
            return "Aquisição com inclinação do gantry (gantry tilt); a geometria real foi preservada no MPR.";
        case GeometryIssue::TooFewSlices:
            return "Poucos cortes para reconstrução volumétrica.";
        case GeometryIssue::Color:
            return "Imagens coloridas não são reconstruídas volumetricamente.";
    }
    return {};
}

bool StackGeometry::has(GeometryIssue i) const { return std::find(issues.begin(), issues.end(), i) != issues.end(); }

bool sameOrientation(const FrameGeometry& a, const FrameGeometry& b, double cosTolerance) {
    return a.rowDir.dot(b.rowDir) > cosTolerance && a.colDir.dot(b.colDir) > cosTolerance;
}

bool sameSpacing(const FrameGeometry& a, const FrameGeometry& b) {
    if (a.spacingSource != b.spacingSource) {
        return false;
    }
    if (!a.hasSpacing()) {
        return true;
    }
    auto close = [](double x, double y) { return std::abs(x - y) <= 1e-3 * std::max(std::abs(x), std::abs(y)); };
    return close(a.spacingX, b.spacingX) && close(a.spacingY, b.spacingY);
}

bool intersectSegmentWithPlane(const Vec3& a, const Vec3& b, const Vec3& planePoint, const Vec3& planeNormal,
                               Vec3& out) {
    const double da = (a - planePoint).dot(planeNormal);
    const double db = (b - planePoint).dot(planeNormal);
    if ((da > 0.0 && db > 0.0) || (da < 0.0 && db < 0.0)) {
        return false;
    }
    const double denom = da - db;
    if (std::abs(denom) < 1e-12) {
        return false;  // segment lies in (or parallel to) the plane
    }
    const double t = da / denom;
    out = a + (b - a) * t;
    return true;
}

namespace {
void addIssue(StackGeometry& g, GeometryIssue i) {
    if (!g.has(i)) {
        g.issues.push_back(i);
    }
}

double median(std::vector<double> v) {
    if (v.empty()) {
        return 0.0;
    }
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid), v.end());
    double m = v[mid];
    if (v.size() % 2 == 0) {
        const double lower = *std::max_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
        m = 0.5 * (m + lower);
    }
    return m;
}
}  // namespace

StackGeometry analyzeStack(const std::vector<FrameRef>& frames) {
    StackGeometry g;
    if (frames.empty()) {
        return g;
    }
    const InstanceInfo& first = *frames.front().instance;
    const FrameGeometry& f0 = frames.front().geometry();
    g.rows = first.rows;
    g.columns = first.columns;
    g.rowDir = f0.rowDir;
    g.colDir = f0.colDir;
    g.normal = f0.normal();
    g.spacingX = f0.spacingX;
    g.spacingY = f0.spacingY;
    g.frameOfReferenceUid = first.frameOfReferenceUid;

    g.spatial = true;
    g.parallel = true;
    bool sameFor = true;
    bool color = false;
    for (const auto& fr : frames) {
        const FrameGeometry& fg = fr.geometry();
        if (!fg.isSpatial()) {
            g.spatial = false;
        }
        if (fr.instance->rows != g.rows || fr.instance->columns != g.columns || !sameSpacing(fg, f0)) {
            g.parallel = false;
            addIssue(g, GeometryIssue::MixedSize);
        }
        if (fg.hasOrientation && !sameOrientation(fg, f0)) {
            g.parallel = false;
            addIssue(g, GeometryIssue::MixedOrientation);
        }
        if (fr.instance->frameOfReferenceUid != g.frameOfReferenceUid) {
            sameFor = false;
        }
        if (fr.instance->samplesPerPixel != 1) {
            color = true;
        }
    }
    if (!g.spatial) {
        addIssue(g, GeometryIssue::MissingGeometry);
    }
    if (!sameFor) {
        addIssue(g, GeometryIssue::MixedFrameOfReference);
    }
    if (color) {
        addIssue(g, GeometryIssue::Color);
    }

    if (!g.spatial || !g.parallel) {
        g.volumetric = false;
        return g;
    }

    g.sliceOffsets.reserve(frames.size());
    for (const auto& fr : frames) {
        g.sliceOffsets.push_back(fr.geometry().position.dot(g.normal));
    }

    if (frames.size() < 2) {
        addIssue(g, GeometryIssue::TooFewSlices);
        return g;
    }

    std::vector<double> diffs;
    diffs.reserve(frames.size() - 1);
    bool duplicates = false;
    for (size_t i = 1; i < g.sliceOffsets.size(); ++i) {
        const double d = std::abs(g.sliceOffsets[i] - g.sliceOffsets[i - 1]);
        if (d < 0.01) {
            duplicates = true;
        } else {
            diffs.push_back(d);
        }
    }
    if (duplicates) {
        addIssue(g, GeometryIssue::DuplicatePositions);
    }
    if (!diffs.empty()) {
        g.sliceSpacing = median(diffs);
        g.minSpacing = *std::min_element(diffs.begin(), diffs.end());
        g.maxSpacing = *std::max_element(diffs.begin(), diffs.end());
        const double tol = std::max(0.01, 0.02 * g.sliceSpacing);
        g.uniformSpacing = true;
        for (double d : diffs) {
            if (d > 1.5 * g.sliceSpacing) {
                ++g.gapCount;
                g.uniformSpacing = false;
            } else if (std::abs(d - g.sliceSpacing) > tol) {
                g.uniformSpacing = false;
            }
        }
        if (g.gapCount > 0) {
            addIssue(g, GeometryIssue::MissingSlices);
        } else if (!g.uniformSpacing) {
            addIssue(g, GeometryIssue::IrregularSpacing);
        }
    }

    // In-plane shear of the slice origins (gantry tilt).
    const Vec3 p0 = frames.front().geometry().position;
    double maxShift = 0.0;
    double tiltAtMax = 0.0;
    for (size_t i = 1; i < frames.size(); ++i) {
        const Vec3 delta = frames[i].geometry().position - p0;
        const double along = delta.dot(g.normal);
        const Vec3 inPlane = delta - g.normal * along;
        const double shift = inPlane.norm();
        if (shift > maxShift) {
            maxShift = shift;
            tiltAtMax = std::abs(along) > 1e-6 ? std::atan2(shift, std::abs(along)) : 0.0;
        }
    }
    g.maxInPlaneShift = maxShift;
    if (maxShift > 0.1) {
        g.tiltDegrees = tiltAtMax * 180.0 / std::numbers::pi;
        addIssue(g, GeometryIssue::GantryTilt);
    }

    if (frames.size() < 3) {
        addIssue(g, GeometryIssue::TooFewSlices);
    }
    g.volumetric = !duplicates && sameFor && !color && frames.size() >= 3 && g.sliceSpacing > 0.0;
    return g;
}

}  // namespace vtc
