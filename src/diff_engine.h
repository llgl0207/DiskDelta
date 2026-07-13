#pragma once

#include "mft_reader.h"
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>

// Comparison result for a single path
enum class DiffStatus {
    UNCHANGED,
    NEW,
    DELETED,
    SIZE_INCREASED,
    SIZE_DECREASED
};

struct DiffEntry {
    std::wstring path;
    uint64_t     size_before;
    uint64_t     size_after;
    int64_t      size_delta;      // after - before (can be negative)
    DiffStatus   status;
    bool         is_directory;
};

class DiffEngine {
public:
    DiffEngine();

    // Load two snapshots and compute diff
    bool LoadSnapshots(const std::wstring& snapshot1_path,
                       const std::wstring& snapshot2_path);

    // Compute differences
    bool ComputeDiff();

    // Get results sorted by delta magnitude (descending)
    const std::vector<DiffEntry>& GetResults() const { return m_results; }

    // Get sorted results
    enum SortBy { BY_DELTA, BY_PATH, BY_SIZE_BEFORE, BY_SIZE_AFTER };
    std::vector<DiffEntry> GetSortedResults(SortBy sort_by, bool descending) const;

    // Filter results
    std::vector<DiffEntry> FilterByStatus(DiffStatus status) const;

    // Export to CSV
    bool ExportCsv(const std::wstring& filepath) const;

    // Get snapshot metadata
    const std::string& GetTimestamp1() const { return m_timestamp1; }
    const std::string& GetTimestamp2() const { return m_timestamp2; }

    // Summary stats
    struct DiffSummary {
        uint64_t total_paths;
        uint64_t unchanged;
        uint64_t new_paths;
        uint64_t deleted_paths;
        uint64_t increased;
        uint64_t decreased;
        int64_t  total_size_delta;
    };
    DiffSummary GetSummary() const;

private:
    std::vector<PathInfo> m_snapshot1;
    std::vector<PathInfo> m_snapshot2;
    std::vector<DiffEntry> m_results;
    std::string m_timestamp1;
    std::string m_timestamp2;
};
