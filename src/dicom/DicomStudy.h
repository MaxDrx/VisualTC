#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dicom/DicomSeries.h"

namespace vtc {

struct Study {
    std::string key;
    std::string studyInstanceUid;
    std::string date;
    std::string time;
    std::string description;
    std::string accessionNumber;
    std::string institution;
    std::vector<SeriesPtr> series;  // ordered by series number
};
using StudyPtr = std::shared_ptr<Study>;

struct Patient {
    std::string key;
    std::string name;
    std::string id;
    std::string birthDate;
    std::string sex;
    std::vector<StudyPtr> studies;  // newest first
};
using PatientPtr = std::shared_ptr<Patient>;

// In-memory Patient -> Study -> Series -> Frame hierarchy built from scanned
// instances. Thread-compatible (guard externally); rebuilds are cheap
// (headers only, no pixels).
class StudyDatabase {
public:
    // Adds instances, ignoring exact duplicates (same SOPInstanceUID). Returns
    // the number of new instances.
    std::size_t addInstances(const std::vector<InstancePtr>& instances);
    void clear();

    [[nodiscard]] const std::vector<PatientPtr>& patients() const { return patients_; }
    [[nodiscard]] SeriesPtr findSeries(const std::string& id) const;
    [[nodiscard]] std::vector<SeriesPtr> allSeries() const;
    [[nodiscard]] std::size_t instanceCount() const { return instances_.size(); }
    [[nodiscard]] std::size_t duplicateCount() const { return duplicates_; }
    // Study that contains a series.
    [[nodiscard]] StudyPtr studyOf(const std::string& seriesId) const;
    [[nodiscard]] PatientPtr patientOf(const std::string& seriesId) const;

private:
    void rebuild();

    std::vector<InstancePtr> instances_;
    std::set<std::string> knownSop_;
    std::size_t duplicates_ = 0;
    std::vector<PatientPtr> patients_;
    std::map<std::string, SeriesPtr> seriesById_;
    std::map<std::string, StudyPtr> studyBySeries_;
    std::map<std::string, PatientPtr> patientBySeries_;
};

}  // namespace vtc
