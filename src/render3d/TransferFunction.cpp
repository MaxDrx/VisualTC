#include "render3d/TransferFunction.h"

#include <algorithm>
#include <cmath>

#include "render3d/RenderVolume.h"

namespace vtc {

void TransferFunction::sort() {
    std::stable_sort(points.begin(), points.end(),
                     [](const TfPoint& a, const TfPoint& b) { return a.value < b.value; });
}

std::array<float, 4> TransferFunction::evaluate(double value) const {
    if (points.empty()) {
        return {0.0F, 0.0F, 0.0F, 0.0F};
    }
    if (value <= points.front().value) {
        const auto& p = points.front();
        return {p.r, p.g, p.b, p.opacity};
    }
    if (value >= points.back().value) {
        const auto& p = points.back();
        return {p.r, p.g, p.b, p.opacity};
    }
    const auto it =
        std::upper_bound(points.begin(), points.end(), value, [](double v, const TfPoint& p) { return v < p.value; });
    const TfPoint& b = *it;
    const TfPoint& a = *(it - 1);
    const double span = b.value - a.value;
    const auto t = static_cast<float>(span > 0.0 ? (value - a.value) / span : 0.0);
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t,
            a.opacity + (b.opacity - a.opacity) * t};
}

TransferFunction TransferFunction::transformed(double shift, double scale, double pivot) const {
    TransferFunction out = *this;
    for (auto& p : out.points) {
        p.value = pivot + (p.value - pivot) * scale + shift;
    }
    return out;
}

double TransferFunction::center() const {
    double lo = 0.0;
    double hi = 0.0;
    bool any = false;
    for (std::size_t i = 0; i < points.size(); ++i) {
        // A point counts when it or a neighbour has opacity: the ramps.
        const bool on = points[i].opacity > 0.0F || (i > 0 && points[i - 1].opacity > 0.0F) ||
                        (i + 1 < points.size() && points[i + 1].opacity > 0.0F);
        if (!on) {
            continue;
        }
        if (!any) {
            lo = points[i].value;
            any = true;
        }
        hi = points[i].value;
    }
    if (!any) {
        return points.empty() ? 0.0 : 0.5 * (points.front().value + points.back().value);
    }
    // Ramps carry the look: weight the first rise of opacity, not the far end
    // of the scale (3071 HU on CT).
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (points[i].opacity > 0.0F && points[i - 1].opacity <= points[i].opacity) {
            return 0.5 * (points[i - 1].value + points[i].value);
        }
    }
    return 0.5 * (lo + hi);
}

bool TransferFunction::operator==(const TransferFunction& o) const {
    if (points.size() != o.points.size()) {
        return false;
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto& a = points[i];
        const auto& b = o.points[i];
        if (a.value != b.value || a.r != b.r || a.g != b.g || a.b != b.b || a.opacity != b.opacity) {
            return false;
        }
    }
    return true;
}

bool TransferLut::anyOpaque(int qMin, int qMax) const {
    if (opaque.empty()) {
        return false;
    }
    // Interpolation between neighbouring entries: one more on each side.
    const int e0 = std::max(0, static_cast<int>(static_cast<long long>(qMin) * (kSize - 1) / 65535) - 1);
    const int e1 =
        std::min(kSize - 1, static_cast<int>((static_cast<long long>(qMax) * (kSize - 1) + 65534) / 65535) + 1);
    return opaque[static_cast<std::size_t>(e1) + 1] - opaque[static_cast<std::size_t>(e0)] > 0;
}

TransferLut buildTransferLut(const TransferFunction& tf, const RenderVolume& volume, double stepMm,
                             float opacityScale) {
    TransferLut lut;
    lut.stepMm = stepMm;
    lut.rgba.resize(static_cast<std::size_t>(TransferLut::kSize) * 4);
    lut.opaque.assign(static_cast<std::size_t>(TransferLut::kSize) + 1, 0);
    const double exponent = stepMm / kOpacityReferenceMm;
    for (int e = 0; e < TransferLut::kSize; ++e) {
        const double q = e * 65535.0 / (TransferLut::kSize - 1);
        const auto c = tf.evaluate(volume.toValue(q));
        const double a = std::clamp(static_cast<double>(c[3]) * static_cast<double>(opacityScale), 0.0, 0.9999);
        double corrected = 1.0 - std::pow(1.0 - a, exponent);
        if (e == 0) {
            corrected = 0.0;  // outside the scan, gaps and the very minimum: always clear
        }
        const auto idx = static_cast<std::size_t>(e) * 4;
        lut.rgba[idx + 0] = std::clamp(c[0], 0.0F, 1.0F);
        lut.rgba[idx + 1] = std::clamp(c[1], 0.0F, 1.0F);
        lut.rgba[idx + 2] = std::clamp(c[2], 0.0F, 1.0F);
        lut.rgba[idx + 3] = static_cast<float>(corrected);
        lut.opaque[static_cast<std::size_t>(e) + 1] =
            lut.opaque[static_cast<std::size_t>(e)] + (corrected > 1e-5 ? 1 : 0);
    }
    return lut;
}

namespace {

TfPoint pt(double v, float r, float g, float b, float a) {
    return {v, r, g, b, a};
}

std::vector<RenderPreset> makePresets() {
    std::vector<RenderPreset> list;
    using A = RenderPreset::Applies;
    {
        RenderPreset p;
        p.key = "ct-bone";
        p.applies = A::Ct;
        p.points = {pt(-1024, 0, 0, 0, 0),
                    pt(130, 0.55F, 0.25F, 0.15F, 0),
                    pt(200, 0.80F, 0.55F, 0.38F, 0.18F),
                    pt(400, 0.93F, 0.84F, 0.68F, 0.65F),
                    pt(1000, 1.0F, 0.98F, 0.92F, 0.85F),
                    pt(3071, 1, 1, 1, 0.85F)};
        p.windowCenter = 400;
        p.windowWidth = 1800;
        p.surfaceValue = 250;
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "ct-angio";
        p.applies = A::Ct;
        p.points = {pt(-1024, 0, 0, 0, 0),
                    pt(110, 0.55F, 0.08F, 0.06F, 0),
                    pt(160, 0.78F, 0.12F, 0.08F, 0.30F),
                    pt(280, 0.95F, 0.30F, 0.22F, 0.55F),
                    pt(500, 1.0F, 0.85F, 0.75F, 0.75F),
                    pt(1200, 1, 1, 0.97F, 0.85F),
                    pt(3071, 1, 1, 1, 0.85F)};
        p.windowCenter = 250;
        p.windowWidth = 700;
        p.surfaceValue = 180;
        p.surfaceColor = {0.90F, 0.32F, 0.26F};
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "ct-soft";
        p.applies = A::Ct;
        p.points = {pt(-1024, 0, 0, 0, 0),
                    pt(-120, 0.90F, 0.76F, 0.52F, 0),
                    pt(-70, 0.92F, 0.78F, 0.55F, 0.004F),
                    pt(-25, 0.80F, 0.35F, 0.25F, 0.0F),
                    pt(20, 0.75F, 0.25F, 0.20F, 0.0F),
                    pt(60, 0.82F, 0.32F, 0.26F, 0.025F),
                    pt(160, 0.88F, 0.45F, 0.35F, 0.03F),
                    pt(260, 0.96F, 0.88F, 0.78F, 0.45F),
                    pt(700, 1.0F, 0.98F, 0.94F, 0.85F),
                    pt(3071, 1, 1, 1, 0.85F)};
        p.windowCenter = 40;
        p.windowWidth = 400;
        p.surfaceValue = 20;
        p.surfaceColor = {0.85F, 0.45F, 0.38F};
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "ct-muscle";
        p.applies = A::Ct;
        p.points = {pt(-1024, 0, 0, 0, 0),
                    pt(-60, 0.55F, 0.20F, 0.15F, 0),
                    pt(40, 0.75F, 0.28F, 0.20F, 0.10F),
                    pt(160, 0.88F, 0.55F, 0.40F, 0.40F),
                    pt(450, 1.0F, 0.94F, 0.85F, 0.75F),
                    pt(3071, 1, 1, 1, 0.80F)};
        p.windowCenter = 60;
        p.windowWidth = 500;
        p.surfaceValue = 0;
        p.surfaceColor = {0.82F, 0.40F, 0.32F};
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "ct-skin";
        p.applies = A::Ct;
        p.points = {pt(-1024, 0, 0, 0, 0), pt(-600, 0.90F, 0.70F, 0.58F, 0), pt(-300, 0.95F, 0.76F, 0.64F, 0.85F),
                    pt(3071, 0.97F, 0.82F, 0.72F, 0.95F)};
        p.windowCenter = -400;
        p.windowWidth = 800;
        p.surfaceValue = -400;
        p.surfaceColor = {0.94F, 0.76F, 0.64F};
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "ct-lung";
        p.applies = A::Ct;
        p.points = {pt(-1024, 0, 0, 0, 0),
                    pt(-960, 0.65F, 0.75F, 0.95F, 0),
                    pt(-880, 0.75F, 0.80F, 1.0F, 0.015F),
                    pt(-650, 0.95F, 0.70F, 0.70F, 0.06F),
                    pt(-450, 0.95F, 0.55F, 0.50F, 0.0F),
                    pt(3071, 1, 1, 1, 0.0F)};
        p.windowCenter = -600;
        p.windowWidth = 1200;
        p.surfaceValue = -500;
        p.surfaceColor = {0.90F, 0.65F, 0.65F};
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "ct-airway";
        p.applies = A::Ct;
        p.mode = RenderMode::MinIp;
        p.points = {pt(-1024, 0, 0, 0, 0), pt(3071, 1, 1, 1, 0)};
        p.shading = false;
        p.windowCenter = -800;
        p.windowWidth = 500;
        p.surfaceValue = -500;
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "ct-mip";
        p.applies = A::Ct;
        p.mode = RenderMode::Mip;
        p.points = list.front().points;
        p.shading = false;
        p.windowCenter = 350;
        p.windowWidth = 1100;
        p.surfaceValue = 250;
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "ct-xray";
        p.applies = A::Ct;
        p.mode = RenderMode::Average;
        p.points = list.front().points;
        p.shading = false;
        p.windowCenter = -450;
        p.windowWidth = 700;
        p.surfaceValue = 250;
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "ct-bone-surface";
        p.applies = A::Ct;
        p.mode = RenderMode::Surface;
        p.points = list.front().points;
        p.windowCenter = 400;
        p.windowWidth = 1800;
        p.surfaceValue = 250;
        list.push_back(p);
    }
    // MR: fractions of the intensity range of the series.
    {
        RenderPreset p;
        p.key = "mr-default";
        p.applies = A::Mr;
        p.relative = true;
        p.points = {pt(0.0, 0, 0, 0, 0), pt(0.12, 0.55F, 0.30F, 0.20F, 0), pt(0.35, 0.85F, 0.60F, 0.45F, 0.12F),
                    pt(0.70, 0.98F, 0.90F, 0.80F, 0.45F), pt(1.0, 1, 1, 1, 0.65F)};
        p.windowCenter = 0.5;
        p.windowWidth = 1.0;
        p.surfaceValue = 0.30;
        p.surfaceColor = {0.90F, 0.78F, 0.68F};
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "mr-angio";
        p.applies = A::Mr;
        p.relative = true;
        p.points = {pt(0.0, 0, 0, 0, 0), pt(0.45, 0.70F, 0.10F, 0.08F, 0), pt(0.65, 0.92F, 0.25F, 0.18F, 0.35F),
                    pt(1.0, 1.0F, 0.85F, 0.75F, 0.80F)};
        p.windowCenter = 0.55;
        p.windowWidth = 0.9;
        p.surfaceValue = 0.6;
        p.surfaceColor = {0.90F, 0.32F, 0.26F};
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "mr-mip";
        p.applies = A::Mr;
        p.relative = true;
        p.mode = RenderMode::Mip;
        p.points = list[list.size() - 2].points;
        p.shading = false;
        p.windowCenter = 0.5;
        p.windowWidth = 1.0;
        p.surfaceValue = 0.3;
        list.push_back(p);
    }
    // Any modality (PET, dose, others): same idea as MR.
    {
        RenderPreset p;
        p.key = "generic";
        p.applies = A::Any;
        p.relative = true;
        p.points = {pt(0.0, 0, 0, 0, 0), pt(0.15, 0.30F, 0.30F, 0.35F, 0), pt(0.50, 0.75F, 0.75F, 0.80F, 0.15F),
                    pt(1.0, 1, 1, 1, 0.60F)};
        p.windowCenter = 0.5;
        p.windowWidth = 1.0;
        p.surfaceValue = 0.4;
        list.push_back(p);
    }
    {
        RenderPreset p;
        p.key = "generic-mip";
        p.applies = A::Any;
        p.relative = true;
        p.mode = RenderMode::Mip;
        p.points = list.back().points;
        p.shading = false;
        p.windowCenter = 0.5;
        p.windowWidth = 1.0;
        p.surfaceValue = 0.4;
        list.push_back(p);
    }
    return list;
}

}  // namespace

const std::vector<RenderPreset>& renderPresets() {
    static const std::vector<RenderPreset> presets = makePresets();
    return presets;
}

const RenderPreset* findRenderPreset(const std::string& key) {
    for (const auto& p : renderPresets()) {
        if (p.key == key) {
            return &p;
        }
    }
    return nullptr;
}

ResolvedPreset resolvePreset(const RenderPreset& preset, const RenderVolume& volume) {
    ResolvedPreset r;
    r.tf.points = preset.points;
    r.windowCenter = preset.windowCenter;
    r.windowWidth = preset.windowWidth;
    r.surfaceValue = preset.surfaceValue;
    if (preset.relative) {
        const double lo = volume.percentile(0.01);
        double hi = volume.percentile(0.995);
        if (hi <= lo) {
            hi = lo + 1.0;
        }
        const double span = hi - lo;
        for (auto& p : r.tf.points) {
            p.value = lo + p.value * span;
        }
        r.windowCenter = lo + preset.windowCenter * span;
        r.windowWidth = std::max(1e-6, preset.windowWidth * span);
        r.surfaceValue = lo + preset.surfaceValue * span;
    }
    r.tf.sort();
    return r;
}

}  // namespace vtc
