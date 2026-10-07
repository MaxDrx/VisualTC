#pragma once

#include <QString>
#include <filesystem>
#include <string>

#include <atomic>

#include "dicom/DicomDecoder.h"
#include "dicom/DicomParser.h"
#include "dicom/WorkerProtocol.h"

namespace vtc {

// Front door for every read of an untrusted DICOM file.
//
// By default requests go to an isolated `visualtc-worker` process (one per
// calling thread, reused across requests). If a hostile or corrupt file
// crashes a codec, only the worker dies: the request returns an error, the
// file is reported as unreadable, and a fresh worker is started for the next
// request. When the worker executable is not available (or isolation is
// disabled in Preferences) parsing/decoding happens in-process.
class DecoderClient {
public:
    // Locates the worker next to the application executable. Call once from
    // the GUI thread after QApplication is created.
    static void initialize(bool isolationEnabled, const QString& workerExecutable = QString());
    static bool isolationActive();
    static QString workerPath();

    static ParseResult parse(const std::filesystem::path& file);
    static DecodeResult decode(const std::string& utf8Path);
    // Extracts the DICOM members of a compressed exam (see ArchiveExtractor).
    // Cancelling kills the worker doing the extraction.
    static ExtractResult extract(const ExtractRequest& request, const std::atomic<bool>* cancel = nullptr);

    // Releases the calling thread's worker (call before the thread ends).
    static void releaseThreadWorker();

    // Test hook: asks the calling thread's worker to crash.
    static bool crashWorkerForTest();
    // Tests: the worker of this thread ends behind the client's back.
    static void endWorkerSilentlyForTest();
};

}  // namespace vtc
