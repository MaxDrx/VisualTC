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
    std::atomic<std::size_t> processed{0};
    std::vector<std::filesystem::path> archives;
    const bool openArchives = static_cast<bool>(options_.expander) && !options_.extractRoot.empty();

    // Parses `list` in parallel. `names` (optional) gives the display name of
    // each file for messages (members of archives).
    auto parseAll = [&](const std::vector<std::filesystem::path>& list,
                        const std::vector<std::string>* names) {
        std::atomic<std::size_t> next{0};
        // Header parsing is I/O bound; a few threads hide latency on SSD/NAS.
        unsigned threads = options_.threads != 0 ? options_.threads : std::min(4u, std::max(2u, hardwareThreads()));
        threads = std::max(1u, std::min<unsigned>(threads, static_cast<unsigned>(list.size())));
        auto worker = [&]() {
            while (true) {
                if (cancel != nullptr && cancel->load()) {
                    return;
                }
                const std::size_t i = next.fetch_add(1);
                if (i >= list.size()) {
                    return;
                }
                ParseResult r;
                try {
                    r = options_.parser ? options_.parser(list[i]) : parseDicomHeader(list[i], options_.limits);
                } catch (...) {
                    r.status = ParseStatus::Malformed;
                    r.message = "Este arquivo não pôde ser interpretado como DICOM.";
                }
                const std::string shown = names != nullptr ? (*names)[i] : pathToUtf8(list[i]);
                // Not DICOM: maybe a compressed exam (detected by content).
                const bool isArchive =
                    r.status == ParseStatus::NotDicom && openArchives && names == nullptr &&
                    detectArchive(list[i]) != ArchiveKind::None;
                std::lock_guard lock(mutex);
                switch (r.status) {
                    case ParseStatus::Ok:
                        if (r.instance->pixelDataTruncated) {
                            // Never shown: it could not be decoded, and as a
                            // missing slice it is reported by the geometry
                            // analysis instead of breaking the MPR volume.
                            result.issues.push_back({ParseStatus::Malformed, shown,
                                                     "Arquivo truncado: os dados de pixel estão incompletos."});
                        } else {
                            result.instances.push_back(std::move(r.instance));
                        }
                        break;
                    case ParseStatus::NotDicom:
                        if (isArchive) {
                            archives.push_back(list[i]);
                        } else {
                            ++result.nonDicomFiles;
                        }
                        break;
                    case ParseStatus::NonImage:
                        ++result.nonImageObjects;
                        break;
                    case ParseStatus::Malformed:
                    case ParseStatus::Unsupported:
                        result.issues.push_back({r.status, shown, r.message});
                        break;
                }
                const std::size_t done = processed.fetch_add(1) + 1;
                if (progress && (done % 64 == 0 || done == prog.filesFound)) {
                    prog.filesProcessed = done;
                    prog.dicomImages = result.instances.size();
                    progress(prog);
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
    };

    parseAll(files, nullptr);

    // Compressed exams: expand each one into its own private folder and scan
    // the DICOM members it contained (nested archives are handled inside).
    std::sort(archives.begin(), archives.end());
    for (std::size_t k = 0; k < archives.size(); ++k) {
        if (cancel != nullptr && cancel->load()) {
            break;
        }
        const std::string archiveName = pathToUtf8(archives[k].filename());
        prog.archive = archiveName;
        if (progress) {
            std::lock_guard lock(mutex);
            progress(prog);
        }
        const std::filesystem::path dest =
            options_.extractRoot / ("arquivo-" + std::to_string(result.archivesOpened + 1));
        ExtractResult ex;
        try {
            ex = options_.expander(archives[k], dest, archiveName + " › ");
        } catch (...) {
            ex.status = ExtractStatus::Corrupt;
            ex.message = "Não foi possível abrir o arquivo compactado " + archiveName + ".";
        }
        ++result.archivesOpened;
        if (!ex.ok()) {
            const ParseStatus st = ex.status == ExtractStatus::Unsupported ? ParseStatus::Unsupported
                                                                           : ParseStatus::Malformed;
            if (ex.status != ExtractStatus::Cancelled) {
                result.issues.push_back({st, pathToUtf8(archives[k]), ex.message});
            }
        } else if (!ex.message.empty()) {
            result.issues.push_back({ParseStatus::Unsupported, pathToUtf8(archives[k]), ex.message});
        }
        if (ex.files.empty()) {
            if (ex.ok() && ex.message.empty()) {
                result.issues.push_back({ParseStatus::Unsupported, pathToUtf8(archives[k]),
                                         "O arquivo compactado " + archiveName +
                                             " não contém imagens DICOM."});
            }
            continue;
        }
        std::vector<std::filesystem::path> members;
        std::vector<std::string> names;
        members.reserve(ex.files.size());
        names.reserve(ex.files.size());
        for (auto& f : ex.files) {
            members.push_back(utf8ToPath(f.path));
            names.push_back(f.displayName);
            result.displayNames[f.path] = f.displayName;
        }
        result.filesVisited += members.size();
        {
            std::lock_guard lock(mutex);
            prog.filesFound += members.size();
            prog.archive.clear();
        }
        parseAll(members, &names);
    }
    result.cancelled = cancel != nullptr && cancel->load();

    // Deterministic order independent of thread scheduling.
    std::sort(result.instances.begin(), result.instances.end(),
              [](const InstancePtr& x, const InstancePtr& y) { return x->filePath < y->filePath; });

    logInfo("scan", "Varredura: " + std::to_string(result.filesVisited) + " arquivos, " +
                        std::to_string(result.instances.size()) + " imagens DICOM, " +
                        std::to_string(result.archivesOpened) + " arquivos compactados, " +
                        std::to_string(result.issues.size()) + " com problemas, " +
                        std::to_string(result.nonDicomFiles) + " não-DICOM.");
    return result;
}

}  // namespace vtc
