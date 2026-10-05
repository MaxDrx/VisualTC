#include "fixtures/DicomFixtures.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

#include <gdcmDataElement.h>
#include <gdcmDataSet.h>
#include <gdcmFileMetaInformation.h>
#include <gdcmImageChangeTransferSyntax.h>
#include <gdcmImageReader.h>
#include <gdcmImageWriter.h>
#include <gdcmItem.h>
#include <gdcmReader.h>
#include <gdcmSequenceOfItems.h>
#include <gdcmTag.h>
#include <gdcmTransferSyntax.h>
#include <gdcmVR.h>
#include <gdcmWriter.h>

namespace vtc::test {

namespace {

std::string ds(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

void putString(gdcm::DataSet& d, std::uint16_t g, std::uint16_t e, gdcm::VR vr, std::string value) {
    if (value.size() % 2 != 0) {
        value.push_back(vr == gdcm::VR::UI ? '\0' : ' ');
    }
    gdcm::DataElement de(gdcm::Tag(g, e));
    de.SetVR(vr);
    de.SetByteValue(value.data(), static_cast<std::uint32_t>(value.size()));
    d.Replace(de);
}

void putUS(gdcm::DataSet& d, std::uint16_t g, std::uint16_t e, std::uint16_t v) {
    gdcm::DataElement de(gdcm::Tag(g, e));
    de.SetVR(gdcm::VR::US);
    de.SetByteValue(reinterpret_cast<const char*>(&v), 2);
    d.Replace(de);
}

gdcm::TransferSyntax::TSType tsOf(Encoding e) {
    switch (e) {
        case Encoding::ExplicitLittle: return gdcm::TransferSyntax::ExplicitVRLittleEndian;
        case Encoding::ImplicitLittle: return gdcm::TransferSyntax::ImplicitVRLittleEndian;
        case Encoding::JpegLossless: return gdcm::TransferSyntax::JPEGLosslessProcess14_1;
        case Encoding::JpegLsLossless: return gdcm::TransferSyntax::JPEGLSLossless;
        case Encoding::Jpeg2000Lossless: return gdcm::TransferSyntax::JPEG2000Lossless;
        case Encoding::RleLossless: return gdcm::TransferSyntax::RLELossless;
        case Encoding::Deflated: return gdcm::TransferSyntax::DeflatedExplicitVRLittleEndian;
        case Encoding::ExplicitBig: return gdcm::TransferSyntax::ExplicitVRBigEndian;
        case Encoding::JpegBaseline: return gdcm::TransferSyntax::JPEGBaselineProcess1;
    }
    return gdcm::TransferSyntax::ExplicitVRLittleEndian;
}

bool isCompressed(Encoding e) {
    return e == Encoding::JpegLossless || e == Encoding::JpegLsLossless || e == Encoding::Jpeg2000Lossless ||
           e == Encoding::RleLossless || e == Encoding::JpegBaseline;
}

}  // namespace

std::string makeUid(const std::string& suffix) {
    static std::atomic<unsigned> counter{1};
    return "1.2.826.0.1.3680043.10.999." + std::to_string(counter.fetch_add(1)) + (suffix.empty() ? "" : "." + suffix);
}

bool writeDicom(const std::filesystem::path& file, const SyntheticImage& img, Encoding encoding) {
    if (isCompressed(encoding)) {
        const auto tmp = file.string() + ".raw.tmp";
        if (!writeDicom(tmp, img, Encoding::ExplicitLittle)) {
            return false;
        }
        const bool ok = transcode(tmp, file, encoding);
        std::filesystem::remove(tmp);
        return ok;
    }

    gdcm::Writer w;
    gdcm::File& f = w.GetFile();
    gdcm::DataSet& d = f.GetDataSet();
    f.GetHeader().SetDataSetTransferSyntax(tsOf(encoding));

    const std::string sop = img.sopInstanceUid.empty() ? makeUid("") : img.sopInstanceUid;
    if (!img.specificCharacterSet.empty()) {
        putString(d, 0x0008, 0x0005, gdcm::VR::CS, img.specificCharacterSet);
    }
    putString(d, 0x0008, 0x0008, gdcm::VR::CS, img.imageType);
    putString(d, 0x0008, 0x0016, gdcm::VR::UI, img.sopClassUid);
    putString(d, 0x0008, 0x0018, gdcm::VR::UI, sop);
    putString(d, 0x0008, 0x0020, gdcm::VR::DA, img.studyDate);
    putString(d, 0x0008, 0x0030, gdcm::VR::TM, "101500");
    putString(d, 0x0008, 0x0060, gdcm::VR::CS, img.modality);
    putString(d, 0x0008, 0x0080, gdcm::VR::LO, "HOSPITAL TESTE");
    putString(d, 0x0008, 0x1030, gdcm::VR::LO, img.studyDescription);
    if (!img.bodyPart.empty()) {
        putString(d, 0x0018, 0x0015, gdcm::VR::CS, img.bodyPart);
    }
    if (!img.patientPosition.empty()) {
        putString(d, 0x0018, 0x5100, gdcm::VR::CS, img.patientPosition);
    }
    if (img.spacingBetweenSlices) {
        putString(d, 0x0018, 0x0088, gdcm::VR::DS, ds(*img.spacingBetweenSlices));
    }
    if (img.numberOfFrames > 1) {
        putString(d, 0x0028, 0x0008, gdcm::VR::IS, std::to_string(img.numberOfFrames));
    }
    if (img.frameTimeMs) {
        putString(d, 0x0018, 0x1063, gdcm::VR::DS, ds(*img.frameTimeMs));
    }
    putString(d, 0x0008, 0x103E, gdcm::VR::LO, img.seriesDescription);
    putString(d, 0x0010, 0x0010, gdcm::VR::PN, img.patientNameRaw);
    putString(d, 0x0010, 0x0020, gdcm::VR::LO, img.patientId);
    putString(d, 0x0020, 0x000D, gdcm::VR::UI, img.studyInstanceUid);
    putString(d, 0x0020, 0x000E, gdcm::VR::UI, img.seriesInstanceUid);
    putString(d, 0x0020, 0x0011, gdcm::VR::IS, std::to_string(img.seriesNumber));
    if (img.instanceNumber) {
        putString(d, 0x0020, 0x0013, gdcm::VR::IS, std::to_string(*img.instanceNumber));
    }
    if (img.echoNumber) {
        putString(d, 0x0018, 0x0086, gdcm::VR::IS, std::to_string(*img.echoNumber));
    }
    if (!img.frameOfReferenceUid.empty()) {
        putString(d, 0x0020, 0x0052, gdcm::VR::UI, img.frameOfReferenceUid);
    }
    if (img.writePosition) {
        putString(d, 0x0020, 0x0032, gdcm::VR::DS,
                  ds(img.position.x) + "\\" + ds(img.position.y) + "\\" + ds(img.position.z));
        putString(d, 0x0020, 0x0037, gdcm::VR::DS,
                  ds(img.rowDir.x) + "\\" + ds(img.rowDir.y) + "\\" + ds(img.rowDir.z) + "\\" + ds(img.colDir.x) +
                      "\\" + ds(img.colDir.y) + "\\" + ds(img.colDir.z));
    }
    if (img.writeSpacing) {
        putString(d, img.spacingIsImager ? 0x0018 : 0x0028, img.spacingIsImager ? 0x1164 : 0x0030, gdcm::VR::DS,
                  ds(img.spacingRow) + "\\" + ds(img.spacingColumn));
    }
    if (img.usPhysicalDeltaCm) {
        auto sq = gdcm::SequenceOfItems::New();
        gdcm::Item item;
        item.SetVLToUndefined();
        gdcm::DataSet& nested = item.GetNestedDataSet();
        auto putUSn = [&nested](std::uint16_t e, std::uint16_t v) {
            gdcm::DataElement de(gdcm::Tag(0x0018, e));
            de.SetVR(gdcm::VR::US);
            de.SetByteValue(reinterpret_cast<const char*>(&v), 2);
            nested.Replace(de);
        };
        auto putFD = [&nested](std::uint16_t e, double v) {
            gdcm::DataElement de(gdcm::Tag(0x0018, e));
            de.SetVR(gdcm::VR::FD);
            de.SetByteValue(reinterpret_cast<const char*>(&v), 8);
            nested.Replace(de);
        };
        putUSn(0x6012, 1);  // region spatial format: 2D
        putUSn(0x6014, 1);  // region data type: tissue
        putUSn(0x6024, 3);  // physical units X: cm
        putUSn(0x6026, 3);  // physical units Y: cm
        putFD(0x602C, *img.usPhysicalDeltaCm);
        putFD(0x602E, *img.usPhysicalDeltaCm);
        sq->AddItem(item);
        sq->SetLengthToUndefined();
        gdcm::DataElement seq(gdcm::Tag(0x0018, 0x6011));
        seq.SetVR(gdcm::VR::SQ);
        seq.SetValue(*sq);
        seq.SetVLToUndefined();
        d.Replace(seq);
    }
    if (img.sliceThickness) {
        putString(d, 0x0018, 0x0050, gdcm::VR::DS, ds(*img.sliceThickness));
    }
    if (img.slope) {
        putString(d, 0x0028, 0x1053, gdcm::VR::DS, ds(*img.slope));
    }
    if (img.intercept) {
        putString(d, 0x0028, 0x1052, gdcm::VR::DS, ds(*img.intercept));
    }
    if (img.windowCenter && img.windowWidth) {
        std::string centers = ds(*img.windowCenter);
        std::string widths = ds(*img.windowWidth);
        for (const auto& [wc, ww] : img.extraWindows) {
            centers += "\\" + ds(wc);
            widths += "\\" + ds(ww);
        }
        putString(d, 0x0028, 0x1050, gdcm::VR::DS, centers);
        putString(d, 0x0028, 0x1051, gdcm::VR::DS, widths);
        if (!img.windowExplanation.empty()) {
            putString(d, 0x0028, 0x1055, gdcm::VR::LO, img.windowExplanation);
        }
    }
    putUS(d, 0x0028, 0x0002, static_cast<std::uint16_t>(img.samplesPerPixel));
    putString(d, 0x0028, 0x0004, gdcm::VR::CS, img.photometric);
    if (img.samplesPerPixel == 3) {
        putUS(d, 0x0028, 0x0006, 0);
    }
    putUS(d, 0x0028, 0x0010, static_cast<std::uint16_t>(img.rows));
    putUS(d, 0x0028, 0x0011, static_cast<std::uint16_t>(img.columns));
    putUS(d, 0x0028, 0x0100, static_cast<std::uint16_t>(img.bitsAllocated));
    putUS(d, 0x0028, 0x0101, static_cast<std::uint16_t>(img.bitsStored));
    putUS(d, 0x0028, 0x0102, static_cast<std::uint16_t>(img.highBit));
    putUS(d, 0x0028, 0x0103, static_cast<std::uint16_t>(img.pixelRepresentation));

    if (!img.paletteRed.empty()) {
        const std::uint16_t desc[3] = {static_cast<std::uint16_t>(img.paletteRed.size()), 0, 16};
        for (std::uint16_t e : {0x1101, 0x1102, 0x1103}) {
            gdcm::DataElement de(gdcm::Tag(0x0028, e));
            de.SetVR(gdcm::VR::US);
            de.SetByteValue(reinterpret_cast<const char*>(desc), 6);
            d.Replace(de);
        }
        const std::vector<std::uint16_t>* luts[3] = {&img.paletteRed, &img.paletteGreen, &img.paletteBlue};
        for (int k = 0; k < 3; ++k) {
            gdcm::DataElement de(gdcm::Tag(0x0028, static_cast<std::uint16_t>(0x1201 + k)));
            de.SetVR(gdcm::VR::OW);
            de.SetByteValue(reinterpret_cast<const char*>(luts[k]->data()),
                            static_cast<std::uint32_t>(luts[k]->size() * 2));
            d.Replace(de);
        }
    }
    gdcm::DataElement pixels(gdcm::Tag(0x7fe0, 0x0010));
    if (img.bitsAllocated == 8) {
        pixels.SetVR(gdcm::VR::OB);
        pixels.SetByteValue(reinterpret_cast<const char*>(img.pixels8.data()),
                            static_cast<std::uint32_t>(img.pixels8.size()));
    } else {
        pixels.SetVR(gdcm::VR::OW);
        pixels.SetByteValue(reinterpret_cast<const char*>(img.pixels.data()),
                            static_cast<std::uint32_t>(img.pixels.size() * 2));
    }
    d.Replace(pixels);

    w.SetFileName(file.string().c_str());
    return w.Write();
}

bool transcode(const std::filesystem::path& in, const std::filesystem::path& out, Encoding encoding) {
    gdcm::ImageReader r;
    r.SetFileName(in.string().c_str());
    if (!r.Read()) {
        return false;
    }
    gdcm::ImageChangeTransferSyntax change;
    change.SetTransferSyntax(tsOf(encoding));
    change.SetInput(r.GetImage());
    if (!change.Change()) {
        return false;
    }
    gdcm::ImageWriter w;
    w.SetFileName(out.string().c_str());
    w.SetFile(r.GetFile());
    w.SetImage(change.GetOutput());
    return w.Write();
}

bool setPhotometric(const std::filesystem::path& file, const std::string& photometric) {
    gdcm::Reader r;
    r.SetFileName(file.string().c_str());
    if (!r.Read()) {
        return false;
    }
    putString(r.GetFile().GetDataSet(), 0x0028, 0x0004, gdcm::VR::CS, photometric);
    gdcm::Writer w;
    w.SetFile(r.GetFile());
    const auto tmp = file.string() + ".pi.tmp";
    w.SetFileName(tmp.c_str());
    if (!w.Write()) {
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

bool stripPart10Header(const std::filesystem::path& in, const std::filesystem::path& out) {
    std::ifstream f(in, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (bytes.size() < 144 || std::memcmp(bytes.data() + 128, "DICM", 4) != 0) {
        return false;
    }
    std::uint32_t groupLength = 0;
    std::memcpy(&groupLength, bytes.data() + 132 + 8, 4);
    const std::size_t start = 132 + 12 + groupLength;
    if (start >= bytes.size()) {
        return false;
    }
    std::ofstream o(out, std::ios::binary);
    o.write(bytes.data() + start, static_cast<std::streamsize>(bytes.size() - start));
    return static_cast<bool>(o);
}

TempDir::TempDir() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    path_ = std::filesystem::temp_directory_path() / ("visualtc-test-" + std::to_string(gen()));
    std::filesystem::create_directories(path_);
}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

}  // namespace vtc::test

namespace vtc::test {

namespace {
gdcm::DataElement sequenceOf(std::uint16_t g, std::uint16_t e, const std::vector<gdcm::DataSet>& items) {
    auto sq = gdcm::SequenceOfItems::New();
    for (const auto& ds : items) {
        gdcm::Item item;
        item.SetVLToUndefined();
        item.SetNestedDataSet(ds);
        sq->AddItem(item);
    }
    sq->SetLengthToUndefined();
    gdcm::DataElement de(gdcm::Tag(g, e));
    de.SetVR(gdcm::VR::SQ);
    de.SetValue(*sq);
    de.SetVLToUndefined();
    return de;
}
}  // namespace

bool writeEnhancedCt(const std::filesystem::path& file, const EnhancedSpec& spec) {
    gdcm::Writer w;
    gdcm::File& f = w.GetFile();
    gdcm::DataSet& d = f.GetDataSet();
    f.GetHeader().SetDataSetTransferSyntax(gdcm::TransferSyntax::ExplicitVRLittleEndian);
    const int frames = static_cast<int>(spec.origins.size());
    putString(d, 0x0008, 0x0008, gdcm::VR::CS, "ORIGINAL\\PRIMARY\\AXIAL");
    putString(d, 0x0008, 0x0016, gdcm::VR::UI, "1.2.840.10008.5.1.4.1.1.2.1");  // Enhanced CT
    putString(d, 0x0008, 0x0018, gdcm::VR::UI, makeUid("enh"));
    putString(d, 0x0008, 0x0020, gdcm::VR::DA, "20260105");
    putString(d, 0x0008, 0x0060, gdcm::VR::CS, "CT");
    putString(d, 0x0008, 0x103E, gdcm::VR::LO, "ENHANCED");
    putString(d, 0x0010, 0x0010, gdcm::VR::PN, "TESTE^ENHANCED");
    putString(d, 0x0010, 0x0020, gdcm::VR::LO, "ENH001");
    putString(d, 0x0020, 0x000D, gdcm::VR::UI, makeUid("enhstudy"));
    putString(d, 0x0020, 0x000E, gdcm::VR::UI, makeUid("enhseries"));
    putString(d, 0x0020, 0x0011, gdcm::VR::IS, "7");
    putString(d, 0x0020, 0x0013, gdcm::VR::IS, "1");
    putString(d, 0x0020, 0x0052, gdcm::VR::UI, makeUid("enhfor"));
    putString(d, 0x0028, 0x0008, gdcm::VR::IS, std::to_string(frames));
    putUS(d, 0x0028, 0x0002, 1);
    putString(d, 0x0028, 0x0004, gdcm::VR::CS, "MONOCHROME2");
    putUS(d, 0x0028, 0x0010, static_cast<std::uint16_t>(spec.rows));
    putUS(d, 0x0028, 0x0011, static_cast<std::uint16_t>(spec.columns));
    putUS(d, 0x0028, 0x0100, 16);
    putUS(d, 0x0028, 0x0101, 16);
    putUS(d, 0x0028, 0x0102, 15);
    putUS(d, 0x0028, 0x0103, 1);

    // Shared functional groups: pixel measures, orientation, rescale, window.
    gdcm::DataSet measures;
    putString(measures, 0x0028, 0x0030, gdcm::VR::DS, ds(spec.spacing) + "\\" + ds(spec.spacing));
    putString(measures, 0x0018, 0x0050, gdcm::VR::DS, "1");
    gdcm::DataSet orientation;
    putString(orientation, 0x0020, 0x0037, gdcm::VR::DS, "1\\0\\0\\0\\1\\0");
    gdcm::DataSet transform;
    putString(transform, 0x0028, 0x1052, gdcm::VR::DS, "-1024");
    putString(transform, 0x0028, 0x1053, gdcm::VR::DS, "1");
    putString(transform, 0x0028, 0x1054, gdcm::VR::LO, "HU");
    gdcm::DataSet voi;
    putString(voi, 0x0028, 0x1050, gdcm::VR::DS, ds(spec.windowCenter));
    putString(voi, 0x0028, 0x1051, gdcm::VR::DS, ds(spec.windowWidth));
    gdcm::DataSet shared;
    shared.Replace(sequenceOf(0x0028, 0x9110, {measures}));
    shared.Replace(sequenceOf(0x0020, 0x9116, {orientation}));
    shared.Replace(sequenceOf(0x0028, 0x9145, {transform}));
    shared.Replace(sequenceOf(0x0028, 0x9132, {voi}));
    d.Replace(sequenceOf(0x5200, 0x9229, {shared}));

    std::vector<gdcm::DataSet> perFrame;
    for (int k = 0; k < frames; ++k) {
        const auto& o = spec.origins[static_cast<size_t>(k)];
        gdcm::DataSet pos;
        putString(pos, 0x0020, 0x0032, gdcm::VR::DS, ds(o.x) + "\\" + ds(o.y) + "\\" + ds(o.z));
        gdcm::DataSet item;
        item.Replace(sequenceOf(0x0020, 0x9113, {pos}));
        perFrame.push_back(item);
    }
    d.Replace(sequenceOf(0x5200, 0x9230, perFrame));

    std::vector<std::uint16_t> pixels;
    for (const auto& fr : spec.hu) {
        for (auto v : fr) {
            pixels.push_back(static_cast<std::uint16_t>(static_cast<std::int16_t>(v + 1024)));
        }
    }
    gdcm::DataElement px(gdcm::Tag(0x7fe0, 0x0010));
    px.SetVR(gdcm::VR::OW);
    px.SetByteValue(reinterpret_cast<const char*>(pixels.data()), static_cast<std::uint32_t>(pixels.size() * 2));
    d.Replace(px);
    w.SetFileName(file.string().c_str());
    return w.Write();
}

}  // namespace vtc::test
