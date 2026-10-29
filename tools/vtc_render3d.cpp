// vtc_render3d: renders a series of a folder in 3D on the processor (no Qt),
// for tuning the presets and measuring the speed of the CPU renderer.
//   vtc_render3d <folder> <series text> <preset> <view> <out.ppm> [size] [yaw°] [pitch°] [maxVoxels]
// view: A P L R S I. The image is written as binary PPM.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>

#include "core/SystemInfo.h"
#include "dicom/DicomDecoder.h"
#include "dicom/DicomScanner.h"
#include "dicom/DicomStudy.h"
#include "mpr/ImageVolume.h"
#include "render3d/Camera3D.h"
#include "render3d/CpuRayCaster.h"
#include "render3d/RenderVolume.h"
#include "render3d/TransferFunction.h"

using namespace vtc;
using Clock = std::chrono::steady_clock;

static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main(int argc, char** argv) {
    if (argc < 6) {
        std::cerr << "uso: vtc_render3d <pasta> <texto da série> <predefinição> <vista A|P|L|R|S|I> <saída.ppm> "
                     "[tamanho] [giro°] [inclinação°] [voxels]\n";
        return 1;
    }
    const std::string want = argv[2];
    const std::string presetKey = argv[3];
    const char view = argv[4][0];
    const int size = argc > 6 ? std::stoi(argv[6]) : 512;
    const double yaw = argc > 7 ? std::stod(argv[7]) : 0.0;
    const double pitch = argc > 8 ? std::stod(argv[8]) : 0.0;
    const std::uint64_t maxVoxels = argc > 9 ? std::stoull(argv[9]) : 64ULL << 20;
    std::shared_ptr<RenderVolume> rawVolume;
    auto t0 = Clock::now();
    auto t1 = t0;
    if (std::string(argv[1]).rfind("raw:", 0) == 0) {
        // raw:<file int16 LE>:nx:ny:nz:sx:sy:sz:offset[:flipz]  (values + offset = HU)
        std::vector<std::string> f;
        std::string cur;
        for (char ch : std::string(argv[1]).substr(4)) {
            if (ch == ':') {
                f.push_back(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        f.push_back(cur);
        const int nx = std::stoi(f[1]);
        const int ny = std::stoi(f[2]);
        const int nz = std::stoi(f[3]);
        const bool flipZ = f.size() > 8 && f[8] == "flipz";
        std::ifstream in(f[0], std::ios::binary);
        std::vector<std::int16_t> raw(static_cast<std::size_t>(nx) * ny * nz);
        in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * 2));
        std::vector<float> values(raw.size());
        const auto slice = static_cast<std::size_t>(nx) * ny;
        for (int z = 0; z < nz; ++z) {
            const int sz = flipZ ? nz - 1 - z : z;
            for (std::size_t i = 0; i < slice; ++i) {
                values[static_cast<std::size_t>(z) * slice + i] =
                    static_cast<float>(raw[static_cast<std::size_t>(sz) * slice + i] + std::stoi(f[7]));
            }
        }
        rawVolume =
            RenderVolume::fromValues(nx, ny, nz, std::stod(f[4]), std::stod(f[5]), std::stod(f[6]), values, true);
    }
    initializeDicomLibrary();
    DicomScanner scanner;
    const ScanResult scan = rawVolume ? ScanResult{} : scanner.scan({argv[1]});
    StudyDatabase db;
    db.addInstances(scan.instances);
    SeriesPtr series;
    for (const auto& s : db.allSeries()) {
        if (s->geometry.volumetric && s->description().find(want) != std::string::npos) {
            series = s;
            break;
        }
    }
    if (!series && !rawVolume) {
        std::cerr << "série volumétrica não encontrada\n";
        return 1;
    }
    std::shared_ptr<const RenderVolume> rendered = rawVolume;
    if (!rendered) {
        std::map<std::string, DecodeResult> decoded;
        auto fetch = [&](const FrameRef& r) -> DecodedFramePtr {
            auto it = decoded.find(r.instance->filePath);
            if (it == decoded.end()) {
                it = decoded.emplace(r.instance->filePath, decodeInstance(*r.instance)).first;
            }
            const auto idx = static_cast<std::size_t>(r.frame);
            return it->second.ok() && idx < it->second.frames.size() ? it->second.frames[idx] : nullptr;
        };
        const auto built = ImageVolume::build(*series, fetch, 4ULL << 30);
        if (!built.volume) {
            std::cerr << built.error << "\n";
            return 1;
        }
        decoded.clear();
        t1 = Clock::now();
        const auto rv = RenderVolume::fromImageVolume(*built.volume, maxVoxels);
        if (!rv.volume) {
            std::cerr << rv.error << "\n";
            return 1;
        }
        rendered = rv.volume;
    }
    auto t2 = Clock::now();
    const RenderVolume& vol = *rendered;
    std::printf("volume %dx%dx%d (%.2f %.2f %.2f mm), valores %.0f..%.0f; montagem %.0f ms, reamostragem %.0f ms\n",
                vol.nx(), vol.ny(), vol.nz(), vol.spacingX(), vol.spacingY(), vol.spacingZ(), vol.minValue(),
                vol.maxValue(), ms(t0, t1), ms(t1, t2));
    const RenderPreset* preset = findRenderPreset(presetKey);
    if (preset == nullptr) {
        std::cerr << "predefinição desconhecida\n";
        return 1;
    }
    const ResolvedPreset rp = resolvePreset(*preset, vol);
    RenderSettings settings;
    settings.mode = preset->mode;
    settings.shading = preset->shading;
    settings.windowCenter = rp.windowCenter;
    settings.windowWidth = rp.windowWidth;
    settings.surfaceValue = rp.surfaceValue;
    settings.surfaceColor = preset->surfaceColor;
    if (const char* step = std::getenv("VTC_STEP")) {
        settings.stepVoxels = std::stod(step);
    }
    if (std::getenv("VTC_FAST") != nullptr) {
        settings.fastGradient = true;
    }
    if (std::getenv("VTC_NOJITTER") != nullptr) {
        settings.jitter = false;
    }
    const TransferLut lut = buildTransferLut(rp.tf, vol, settings.stepMm(vol.minSpacing()));
    Camera3D cam;
    switch (view) {
        case 'P':
            cam.setView(ViewDirection::Posterior);
            break;
        case 'L':
            cam.setView(ViewDirection::Left);
            break;
        case 'R':
            cam.setView(ViewDirection::Right);
            break;
        case 'S':
            cam.setView(ViewDirection::Superior);
            break;
        case 'I':
            cam.setView(ViewDirection::Inferior);
            break;
        default:
            cam.setView(ViewDirection::Anterior);
            break;
    }
    cam.fit(vol, size, size);
    cam.rotate(yaw * 3.14159265358979 / 180.0, pitch * 3.14159265358979 / 180.0);
    std::vector<std::uint32_t> img(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
    // VTC_REPEAT=N: best of N (timings on a busy machine vary).
    const int repeat = std::getenv("VTC_REPEAT") != nullptr ? std::max(1, std::atoi(std::getenv("VTC_REPEAT"))) : 1;
    double best = 1e30;
    for (int i = 0; i < repeat; ++i) {
        auto r0 = Clock::now();
        CpuRayCaster::render(vol, lut, cam, settings, size, size, img.data());
        best = std::min(best, ms(r0, Clock::now()));
    }
    std::printf("renderização %dx%d: %.0f ms em %u núcleos\n", size, size, best, hardwareThreads());
    std::ofstream out(argv[5], std::ios::binary);
    out << "P6\n" << size << " " << size << "\n255\n";
    for (std::uint32_t p : img) {
        const char rgb[3] = {static_cast<char>((p >> 16) & 0xFF), static_cast<char>((p >> 8) & 0xFF),
                             static_cast<char>(p & 0xFF)};
        out.write(rgb, 3);
    }
    return 0;
}
