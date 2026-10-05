#pragma once

#include <string>
#include <vector>

#include "dicom/DicomParser.h"
#include "dicom/DicomTypes.h"
#include "imaging/PixelData.h"

namespace vtc {

struct DecodeResult {
    std::vector<DecodedFramePtr> frames;  // one per frame of the instance
    std::string error;                    // pt-BR, empty on success
    [[nodiscard]] bool ok() const { return error.empty() && !frames.empty(); }
};

// Decodes all frames of one instance (any transfer syntax supported by GDCM:
// uncompressed, deflate, JPEG, JPEG Lossless, JPEG-LS, JPEG 2000, RLE).
//
// Guarantees:
//  - stored values are masked to BitsStored/HighBit and sign-extended;
//  - color data is returned as interleaved 8-bit RGB (palette color and
//    YBR are converted);
//  - sizes are validated against the header and `limits` before allocation;
//  - never throws.
DecodeResult decodeInstance(const InstanceInfo& info, const ParseLimits& limits = {});

}  // namespace vtc
