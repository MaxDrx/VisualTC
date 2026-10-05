// vtc_bench: rough performance figures of the VisualTC core on a folder.
//   vtc_bench <folder>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>

#include "core/SystemInfo.h"
#include "dicom/DicomDecoder.h"
#include "dicom/DicomScanner.h"
#include "dicom/DicomStudy.h"
#include "imaging/WindowLevel.h"
#include "mpr/ImageVolume.h"
#include "mpr/MprGeometry.h"
#include "mpr/Reslicer.h"

using namespace vtc;
using Clock = std::chrono::steady_clock;

static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "uso: vtc_bench <pasta>\n";
        return 1;
    }
    initializeDicomLibrary();
    auto t0 = Clock::now();
    DicomScanner scanner;
    const ScanResult scan = scanner.scan({argv[1]});
    auto t1 = Clock::now();
    std::printf("Varredura: %zu arquivos, %zu imagens em %.0f ms (%.2f ms/arquivo)\n", scan.filesVisited,
                scan.instances.size(), ms(t0, t1), ms(t0, t1) / std::max<std::size_t>(1, scan.filesVisited));
    StudyDatabase db;
    db.addInstances(scan.instances);
    for (const auto& s : db.allSeries()) {
        if (s->frames.empty()) {
            continue;
        }
        const auto& inst = s->firstInstance();
        const int n = std::min(20, s->frameCount());
        auto d0 = Clock::now();
        DecodedFramePtr last;
        std::size_t decodes = 0;
        std::string lastPath;
        for (int i = 0; i < n; ++i) {
            const auto& fr = s->frames[static_cast<size_t>(i)];
            if (fr.instance->filePath == lastPath) {
                continue;
            }
            lastPath = fr.instance->filePath;
            const auto r = decodeInstance(*fr.instance);
            if (r.ok()) {
                last = r.frames[static_cast<size_t>(fr.frame)];
                ++decodes;
            }
        }
        auto d1 = Clock::now();
        double renderMs = 0.0;
        if (last) {
            DisplayRenderer renderer;
            std::vector<std::uint8_t> out(last->pixelCount() * 4);
            DisplayParams p;
            p.center = 40;
            p.width = 400;
            auto r0 = Clock::now();
            for (int k = 0; k < 50; ++k) {
                p.center = 40 + k;  // force LUT rebuild every time (worst case: W/L drag)
                if (last->isColor()) {
                    renderer.renderRgb32(*last, p, reinterpret_cast<std::uint32_t*>(out.data()), last->width * 4);
                } else {
                    renderer.renderGray(*last, p, out.data(), last->width);
                }
            }
            renderMs = ms(r0, Clock::now()) / 50.0;
        }
        std::printf("Série %-28s %-4s %4d×%-4d %-36s decodificação %.2f ms/arquivo, W/L %.2f ms/imagem\n",
                    s->description().substr(0, 28).c_str(), inst.modality.c_str(), inst.columns, inst.rows,
                    transferSyntaxName(inst.transferSyntaxUid).substr(0, 36).c_str(),
                    decodes ? ms(d0, d1) / static_cast<double>(decodes) : 0.0, renderMs);
    }
    // MPR on the largest volumetric series
    SeriesPtr best;
    for (const auto& s : db.allSeries()) {
        if (s->geometry.volumetric && (!best || s->frameCount() > best->frameCount())) {
            best = s;
        }
    }
    if (best) {
        std::map<std::string, std::vector<DecodedFramePtr>> cache;
        auto fetch = [&cache](const FrameRef& r) -> DecodedFramePtr {
            auto& v = cache[r.instance->filePath];
            if (v.empty()) {
                auto d = decodeInstance(*r.instance);
                v = d.frames;
            }
            return static_cast<size_t>(r.frame) < v.size() ? v[static_cast<size_t>(r.frame)] : nullptr;
        };
        auto b0 = Clock::now();
        auto built = ImageVolume::build(*best, fetch, 4ull << 30);
        auto b1 = Clock::now();
        if (built.volume) {
            const auto& vol = *built.volume;
            std::printf("Volume %d×%d×%d (%.0f MB) montado em %.0f ms\n", vol.nx(), vol.ny(), vol.nz(),
                        static_cast<double>(vol.byteSize()) / 1048576.0, ms(b0, b1));
            for (auto o : {MprOrientation::Axial, MprOrientation::Coronal, MprOrientation::Sagittal}) {
                const auto view = makeOrthogonalView(vol, o);
                const auto plane = view.planeThrough(vol.center());
                auto r0 = Clock::now();
                for (int k = 0; k < 10; ++k) {
                    (void)reslice(vol, plane, {}, Interpolation::Linear);
                }
                const double thin = ms(r0, Clock::now()) / 10.0;
                r0 = Clock::now();
                (void)reslice(vol, plane, {20.0, SlabMode::MIP}, Interpolation::Linear);
                const double mip = ms(r0, Clock::now());
                std::printf("  MPR %-8s %4d×%-4d  plano fino %.1f ms   MIP 20 mm %.1f ms\n", toLabel(o).c_str(),
                            plane.width, plane.height, thin, mip);
            }
        }
    }
    std::printf("Threads de hardware: %u, RAM: %.1f GB\n", hardwareThreads(),
                static_cast<double>(physicalMemoryBytes()) / 1073741824.0);
    return 0;
}
