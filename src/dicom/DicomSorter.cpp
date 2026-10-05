#include "dicom/DicomSorter.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <sstream>
#include <tuple>

namespace vtc {

namespace {

std::tuple<int, int> instanceKey(const FrameRef& f) {
    return {f.instance->instanceNumber.value_or(0), f.frame};
}

std::string planeName(const Vec3& n) {
    const double ax = std::abs(n.x);
    const double ay = std::abs(n.y);
    const double az = std::abs(n.z);
    const double m = std::max({ax, ay, az});
    if (m < 0.8) {
        return "Oblíqua";
    }
    if (m == az) {
        return "Axial";
    }
    if (m == ay) {
        return "Coronal";
    }
    return "Sagital";
}

std::string formatNumber(double v) {
    std::ostringstream os;
    os.precision(4);
    os << v;
    return os.str();
}

std::string joinLabel(const std::string& a, const std::string& b) {
    if (a.empty()) {
        return b;
    }
    if (b.empty()) {
        return a;
    }
    return a + " · " + b;
}

bool hasDuplicatePositions(const std::vector<FrameRef>& frames) {
    if (frames.size() < 2) {
        return false;
    }
    const Vec3 n = frames.front().geometry().normal();
    std::vector<double> d;
    d.reserve(frames.size());
    for (const auto& f : frames) {
        if (!f.geometry().hasPosition) {
            return false;
        }
        d.push_back(f.geometry().position.dot(n));
    }
    std::sort(d.begin(), d.end());
    for (size_t i = 1; i < d.size(); ++i) {
        if (d[i] - d[i - 1] < 0.01) {
            return true;
        }
    }
    return false;
}

// One attribute that may explain duplicate positions. Returns (sort key, label).
struct SplitKey {
    std::function<std::optional<std::pair<double, std::string>>(const FrameRef&)> value;
};

std::vector<SplitKey> splitKeys() {
    using R = std::optional<std::pair<double, std::string>>;
    return {
        {[](const FrameRef& f) -> R {
            const auto& k = f.info().keys;
            if (k.echoNumber) return std::make_pair(double(*k.echoNumber), "Eco " + std::to_string(*k.echoNumber));
            return std::nullopt;
        }},
        {[](const FrameRef& f) -> R {
            const auto& k = f.info().keys;
            if (k.echoTime) return std::make_pair(*k.echoTime, "TE " + formatNumber(*k.echoTime) + " ms");
            return std::nullopt;
        }},
        {[](const FrameRef& f) -> R {
            const auto& k = f.info().keys;
            if (k.temporalPosition)
                return std::make_pair(double(*k.temporalPosition), "Fase " + std::to_string(*k.temporalPosition));
            return std::nullopt;
        }},
        {[](const FrameRef& f) -> R {
            const auto& k = f.info().keys;
            if (k.diffusionBValue) return std::make_pair(*k.diffusionBValue, "b=" + formatNumber(*k.diffusionBValue));
            return std::nullopt;
        }},
        {[](const FrameRef& f) -> R {
            const auto& k = f.info().keys;
            if (k.triggerTime) return std::make_pair(*k.triggerTime, "Trigger " + formatNumber(*k.triggerTime) + " ms");
            return std::nullopt;
        }},
        {[](const FrameRef& f) -> R {
            const auto& k = f.info().keys;
            if (k.acquisitionNumber)
                return std::make_pair(double(*k.acquisitionNumber), "Aquisição " + std::to_string(*k.acquisitionNumber));
            return std::nullopt;
        }},
        {[](const FrameRef& f) -> R {
            const auto& k = f.info().keys;
            if (!k.stackId.empty()) return std::make_pair(0.0, "Stack " + k.stackId);
            return std::nullopt;
        }},
        {[](const FrameRef& f) -> R {
            const auto& k = f.info().keys;
            if (!k.imageType.empty()) return std::make_pair(0.0, k.imageType);
            return std::nullopt;
        }},
    };
}

std::vector<StackSplit> splitByDuplicates(std::vector<FrameRef> frames) {
    if (!hasDuplicatePositions(frames)) {
        return {{std::string(), std::move(frames)}};
    }
    for (const auto& key : splitKeys()) {
        std::map<std::string, std::pair<double, std::vector<FrameRef>>> groups;
        bool allHave = true;
        for (const auto& f : frames) {
            const auto v = key.value(f);
            if (!v) {
                allHave = false;
                break;
            }
            auto& g = groups[v->second];
            g.first = v->first;
            g.second.push_back(f);
        }
        if (!allHave || groups.size() < 2) {
            continue;
        }
        bool ok = true;
        for (const auto& [label, g] : groups) {
            if (hasDuplicatePositions(g.second)) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            continue;
        }
        std::vector<std::pair<double, StackSplit>> ordered;
        for (auto& [label, g] : groups) {
            ordered.push_back({g.first, StackSplit{label, std::move(g.second)}});
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<StackSplit> out;
        for (auto& o : ordered) {
            out.push_back(std::move(o.second));
        }
        return out;
    }
    // Unexplained duplicates: keep together; analyzeStack() flags them and the
    // series is not used for volumetric reconstruction.
    return {{std::string(), std::move(frames)}};
}

// Reverse the spatial order when the scanner's numbering runs the other way,
// so that "image 1" in VisualTC is the scanner's image 1.
void orientByInstanceNumber(std::vector<FrameRef>& frames) {
    if (frames.size() < 2) {
        return;
    }
    long increasing = 0;
    long decreasing = 0;
    for (size_t i = 1; i < frames.size(); ++i) {
        const auto a = instanceKey(frames[i - 1]);
        const auto b = instanceKey(frames[i]);
        if (a < b) {
            ++increasing;
        } else if (b < a) {
            ++decreasing;
        }
    }
    if (decreasing > increasing) {
        std::reverse(frames.begin(), frames.end());
    }
}

struct GeoClass {
    FrameRef representative;
    std::vector<FrameRef> frames;
};

bool compatible(const FrameRef& a, const FrameRef& b) {
    const auto& ia = *a.instance;
    const auto& ib = *b.instance;
    if (ia.rows != ib.rows || ia.columns != ib.columns || ia.samplesPerPixel != ib.samplesPerPixel) {
        return false;
    }
    if (ia.frameOfReferenceUid != ib.frameOfReferenceUid) {
        return false;
    }
    const auto& ga = a.geometry();
    const auto& gb = b.geometry();
    if (ga.isSpatial() != gb.isSpatial() || ga.hasOrientation != gb.hasOrientation) {
        return false;
    }
    return !ga.hasOrientation || sameOrientation(ga, gb);
}

}  // namespace

void sortByInstance(std::vector<FrameRef>& frames) {
    std::stable_sort(frames.begin(), frames.end(), [](const FrameRef& a, const FrameRef& b) {
        const auto ka = instanceKey(a);
        const auto kb = instanceKey(b);
        if (ka != kb) {
            return ka < kb;
        }
        if (a.instance->contentTime != b.instance->contentTime) {
            return a.instance->contentTime < b.instance->contentTime;
        }
        return a.instance->filePath < b.instance->filePath;
    });
}

void sortSpatially(std::vector<FrameRef>& frames) {
    if (frames.empty()) {
        return;
    }
    const Vec3 n = frames.front().geometry().normal();
    std::stable_sort(frames.begin(), frames.end(), [&n](const FrameRef& a, const FrameRef& b) {
        const double da = a.geometry().position.dot(n);
        const double db = b.geometry().position.dot(n);
        if (std::abs(da - db) >= 1e-6) {
            return da < db;
        }
        return instanceKey(a) < instanceKey(b);
    });
}

std::vector<StackSplit> buildStacks(std::vector<FrameRef> frames) {
    std::vector<StackSplit> out;
    if (frames.empty()) {
        return out;
    }
    sortByInstance(frames);  // deterministic input order

    auto emitNonSpatial = [&out](std::vector<FrameRef> fs, const std::string& label) {
        // Several multi-frame objects (e.g. ultrasound cine clips) in one
        // series: show each clip as its own stack.
        std::map<std::tuple<int, std::string>, std::vector<FrameRef>> byInstance;
        for (auto& f : fs) {
            byInstance[{f.instance->instanceNumber.value_or(0), f.instance->filePath}].push_back(f);
        }
        int clips = 0;
        for (const auto& [k, v] : byInstance) {
            if (v.front().instance->numberOfFrames > 1) {
                ++clips;
            }
        }
        if (clips >= 2) {
            int clipNo = 0;
            std::vector<FrameRef> singles;
            for (auto& [k, v] : byInstance) {
                if (v.front().instance->numberOfFrames > 1) {
                    sortByInstance(v);
                    out.push_back({joinLabel(label, "Clipe " + std::to_string(++clipNo)), std::move(v)});
                } else {
                    singles.insert(singles.end(), v.begin(), v.end());
                }
            }
            if (!singles.empty()) {
                sortByInstance(singles);
                out.push_back({joinLabel(label, "Imagens"), std::move(singles)});
            }
            return;
        }
        sortByInstance(fs);
        out.push_back({label, std::move(fs)});
    };

    auto emitClass = [&](std::vector<FrameRef> fs, const std::string& label) {
        if (!fs.front().geometry().isSpatial()) {
            emitNonSpatial(std::move(fs), label);
            return;
        }
        for (auto& split : splitByDuplicates(std::move(fs))) {
            sortSpatially(split.frames);
            orientByInstanceNumber(split.frames);
            out.push_back({joinLabel(label, split.label), std::move(split.frames)});
        }
    };

    std::vector<GeoClass> classes;
    for (const auto& f : frames) {
        auto it = std::find_if(classes.begin(), classes.end(),
                               [&f](const GeoClass& c) { return compatible(c.representative, f); });
        if (it == classes.end()) {
            classes.push_back({f, {f}});
        } else {
            it->frames.push_back(f);
        }
    }

    if (classes.size() == 1) {
        emitClass(std::move(classes.front().frames), std::string());
        return out;
    }

    const bool allSmall =
        std::all_of(classes.begin(), classes.end(), [](const GeoClass& c) { return c.frames.size() < 3; });
    if (allSmall) {
        // Typical 3-plane localizer: one stack in acquisition order.
        emitNonSpatial(std::move(frames), std::string());
        return out;
    }

    std::vector<FrameRef> misc;
    std::map<std::string, int> nameCount;
    for (const auto& c : classes) {
        if (c.frames.size() >= 3 && c.representative.geometry().hasOrientation) {
            ++nameCount[planeName(c.representative.geometry().normal())];
        }
    }
    std::map<std::string, int> nameSeen;
    for (auto& c : classes) {
        if (c.frames.size() < 3) {
            misc.insert(misc.end(), c.frames.begin(), c.frames.end());
            continue;
        }
        std::string label;
        if (c.representative.geometry().hasOrientation) {
            label = planeName(c.representative.geometry().normal());
            if (nameCount[label] > 1) {
                label += " " + std::to_string(++nameSeen[label]);
            }
        } else {
            label = std::to_string(c.representative.instance->columns) + "×" +
                    std::to_string(c.representative.instance->rows);
        }
        emitClass(std::move(c.frames), label);
    }
    if (!misc.empty()) {
        emitNonSpatial(std::move(misc), "Outras");
    }
    return out;
}

}  // namespace vtc
