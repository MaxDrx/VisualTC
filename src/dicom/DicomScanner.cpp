#include "dicom/DicomScanner.h"

#include <algorithm>
#include <mutex>
#include <thread>

#include "core/Logger.h"
#include "core/PathUtil.h"
#include "core/SystemInfo.h"

namespace vtc {

std::vector<std::filesystem::path> DicomScanner::collectFiles(const std::vector<std::filesystem::path>& inputs,
                                                              bool* truncated) const {
    namespace fs = std::filesystem;
    std::vector<fs::path> files;
    bool hitLimit = false;
    for (const auto& input : inputs) {
        std::error_code ec;
        const auto st = fs::status(input, ec);
        if (ec) {
            continue;
        }
        if (fs::is_regular_file(st)) {
            files.push_back(input);
        } else if (fs::is_directory(st)) {
            // Directory symlinks are not followed: avoids cycles and escaping
            // the selected folder through malicious links.
            fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec);
            const fs::recursive_directory_iterator end;
            while (!ec && it != end) {
                std::error_code fec;
                if (it->is_regular_file(fec) && !fec) {
                    const auto name = it->path().filename().native();
                    // Hidden/system files (".DS_Store", "._x" AppleDouble) are never DICOM.
                    if (name.empty() || name[0] != '.') {
                        files.push_back(it->path());
                    }
                }
                if (files.size() >= options_.maxFiles) {
                    hitLimit = true;
                    break;
                }
                it.increment(ec);
            }
        }
        if (hitLimit) {
            break;
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (truncated != nullptr) {
        *truncated = hitLimit;
    }
    return files;
}

ScanResult DicomScanner::scan(const std::vector<std::filesystem::path>& inputs, const ScanProgressCallback& progress,
                              const std::atomic<bool>* cancel) const {
    ScanResult result;
    const auto files = collectFiles(inputs, &result.truncated);
    result.filesVisited = files.size();

    ScanProgress prog;
    prog.filesFound = files.size();
    if (progress) {
        progress(prog);
    }

    std::mutex mutex;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> processed{0};
    // Header parsing is I/O bound; a few threads hide latency on SSD/NAS.
    unsigned threads = options_.threads != 0 ? options_.threads : std::min(4u, std::max(2u, hardwareThreads()));
    threads = std::max(1u, std::min<unsigned>(threads, static_cast<unsigned>(files.size())));

    auto worker = [&]() {
        while (true) {
            if (cancel != nullptr && cancel->load()) {
                return;
            }
            const std::size_t i = next.fetch_add(1);
            if (i >= files.size()) {
                return;
            }
            ParseResult r;
            try {
                r = options_.parser ? options_.parser(files[i]) : parseDicomHeader(files[i], options_.limits);
            } catch (const std::exception&) {
                r.status = ParseStatus::Malformed;
                r.message = "Este arquivo não pôde ser interpretado como DICOM.";
            } catch (...) {
                r.status = ParseStatus::Malformed;
                r.message = "Este arquivo não pôde ser interpretado como DICOM.";
            }
            {
                std::lock_guard lock(mutex);
                switch (r.status) {
                    case ParseStatus::Ok:
                        if (r.instance->pixelDataTruncated) {
                            result.issues.push_back({ParseStatus::Malformed, pathToUtf8(files[i]),
                                                     "Arquivo truncado: os dados de pixel estão incompletos."});
                        }
                        result.instances.push_back(std::move(r.instance));
                        break;
                    case ParseStatus::NotDicom:
                        ++result.nonDicomFiles;
                        break;
                    case ParseStatus::NonImage:
                        ++result.nonImageObjects;
                        break;
                    case ParseStatus::Malformed:
                    case ParseStatus::Unsupported:
                        result.issues.push_back({r.status, pathToUtf8(files[i]), r.message});
                        break;
                }
                const std::size_t done = processed.fetch_add(1) + 1;
                if (progress && (done % 64 == 0 || done == files.size())) {
                    prog.filesProcessed = done;
                    prog.dicomImages = result.instances.size();
                    progress(prog);
                }
            }
        }
    };

    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) {
        pool.emplace_back(worker);
    }
    worker();
    for (auto& th : pool) {
        th.join();
    }
    result.cancelled = cancel != nullptr && cancel->load();

    // Deterministic order independent of thread scheduling.
    std::sort(result.instances.begin(), result.instances.end(),
              [](const InstancePtr& x, const InstancePtr& y) { return x->filePath < y->filePath; });

    logInfo("scan", "Varredura: " + std::to_string(result.filesVisited) + " arquivos, " +
                        std::to_string(result.instances.size()) + " imagens DICOM, " +
                        std::to_string(result.issues.size()) + " com problemas, " +
                        std::to_string(result.nonDicomFiles) + " não-DICOM.");
    return result;
}

}  // namespace vtc
