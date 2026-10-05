#include "dicom/DicomStudy.h"

#include <algorithm>
#include <tuple>

#include "dicom/DicomSorter.h"

namespace vtc {

std::string Series::description() const {
    if (frames.empty()) {
        return {};
    }
    std::string d = firstInstance().seriesDescription;
    if (d.empty()) {
        d = firstInstance().protocolName;
    }
    if (!subLabel.empty()) {
        d = d.empty() ? subLabel : d + " · " + subLabel;
    }
    return d;
}

bool Series::isMultiFrameClip() const {
    if (frames.empty()) {
        return false;
    }
    const auto& first = frames.front().instance;
    return first->numberOfFrames > 1 &&
           std::all_of(frames.begin(), frames.end(), [&first](const FrameRef& f) { return f.instance == first; });
}

std::optional<double> Series::sliceThickness() const {
    if (frames.empty()) {
        return std::nullopt;
    }
    return frames[frames.size() / 2].geometry().sliceThickness;
}

std::size_t StudyDatabase::addInstances(const std::vector<InstancePtr>& instances) {
    std::size_t added = 0;
    for (const auto& inst : instances) {
        if (!inst || !inst->hasPixelData || inst->frames.empty()) {
            continue;
        }
        // Same image found twice (copies in two folders, DICOMDIR + files):
        // keep the first one.
        const std::string sopKey = inst->sopInstanceUid.empty() ? "path:" + inst->filePath : inst->sopInstanceUid;
        if (!knownSop_.insert(sopKey).second) {
            ++duplicates_;
            continue;
        }
        instances_.push_back(inst);
        ++added;
    }
    if (added > 0) {
        rebuild();
    }
    return added;
}

void StudyDatabase::clear() {
    instances_.clear();
    knownSop_.clear();
    duplicates_ = 0;
    patients_.clear();
    seriesById_.clear();
    studyBySeries_.clear();
    patientBySeries_.clear();
}

SeriesPtr StudyDatabase::findSeries(const std::string& id) const {
    const auto it = seriesById_.find(id);
    return it == seriesById_.end() ? nullptr : it->second;
}

std::vector<SeriesPtr> StudyDatabase::allSeries() const {
    std::vector<SeriesPtr> out;
    for (const auto& p : patients_) {
        for (const auto& s : p->studies) {
            out.insert(out.end(), s->series.begin(), s->series.end());
        }
    }
    return out;
}

StudyPtr StudyDatabase::studyOf(const std::string& seriesId) const {
    const auto it = studyBySeries_.find(seriesId);
    return it == studyBySeries_.end() ? nullptr : it->second;
}

PatientPtr StudyDatabase::patientOf(const std::string& seriesId) const {
    const auto it = patientBySeries_.find(seriesId);
    return it == patientBySeries_.end() ? nullptr : it->second;
}

void StudyDatabase::rebuild() {
    // patientKey -> studyKey -> seriesKey -> frames
    std::map<std::string, std::map<std::string, std::map<std::string, std::vector<FrameRef>>>> tree;
    std::map<std::string, InstancePtr> patientSample;
    std::map<std::string, InstancePtr> studySample;

    for (const auto& inst : instances_) {
        const std::string patientKey = inst->patientId + "|" + inst->patientName;
        const std::string studyKey =
            inst->studyInstanceUid.empty() ? "nostudy|" + patientKey + "|" + inst->studyDate : inst->studyInstanceUid;
        const std::string seriesKey =
            inst->seriesInstanceUid.empty()
                ? "noseries|" + studyKey + "|" + inst->modality + "|" + std::to_string(inst->seriesNumber.value_or(0))
                : inst->seriesInstanceUid;
        auto& frames = tree[patientKey][studyKey][seriesKey];
        for (int f = 0; f < static_cast<int>(inst->frames.size()); ++f) {
            frames.push_back({inst, f});
        }
        patientSample.emplace(patientKey, inst);
        studySample.emplace(studyKey, inst);
    }

    patients_.clear();
    seriesById_.clear();
    studyBySeries_.clear();
    patientBySeries_.clear();

    for (auto& [patientKey, studies] : tree) {
        auto patient = std::make_shared<Patient>();
        const auto& ps = patientSample[patientKey];
        patient->key = patientKey;
        patient->name = ps->patientName;
        patient->id = ps->patientId;
        patient->birthDate = ps->patientBirthDate;
        patient->sex = ps->patientSex;

        for (auto& [studyKey, seriesMap] : studies) {
            auto study = std::make_shared<Study>();
            const auto& ss = studySample[studyKey];
            study->key = studyKey;
            study->studyInstanceUid = ss->studyInstanceUid;
            study->date = ss->studyDate;
            study->time = ss->studyTime;
            study->description = ss->studyDescription;
            study->accessionNumber = ss->accessionNumber;
            study->institution = ss->institutionName;

            for (auto& [seriesKey, frames] : seriesMap) {
                auto stacks = buildStacks(std::move(frames));
                int index = 0;
                for (auto& stack : stacks) {
                    auto series = std::make_shared<Series>();
                    series->seriesInstanceUid = stack.frames.front().instance->seriesInstanceUid;
                    series->id = seriesKey + (stacks.size() > 1 ? "#" + std::to_string(index) : std::string());
                    series->subLabel = stack.label;
                    series->frames = std::move(stack.frames);
                    series->geometry = analyzeStack(series->frames);
                    seriesById_[series->id] = series;
                    studyBySeries_[series->id] = study;
                    patientBySeries_[series->id] = patient;
                    study->series.push_back(series);
                    ++index;
                }
            }
            std::stable_sort(study->series.begin(), study->series.end(), [](const SeriesPtr& a, const SeriesPtr& b) {
                const int na = a->number().value_or(1 << 30);
                const int nb = b->number().value_or(1 << 30);
                if (na != nb) {
                    return na < nb;
                }
                return a->description() < b->description();
            });
            patient->studies.push_back(study);
        }
        std::stable_sort(patient->studies.begin(), patient->studies.end(), [](const StudyPtr& a, const StudyPtr& b) {
            return std::tie(a->date, a->time) > std::tie(b->date, b->time);
        });
        patients_.push_back(patient);
    }
    std::stable_sort(patients_.begin(), patients_.end(),
                     [](const PatientPtr& a, const PatientPtr& b) { return a->name < b->name; });
}

}  // namespace vtc
