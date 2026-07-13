#include "diff_engine.h"
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <map>
#include <set>
#include <cstdlib>
#include <cstdio>

DiffEngine::DiffEngine() {}

bool DiffEngine::LoadSnapshots(const std::wstring& snapshot1_path,
                                const std::wstring& snapshot2_path) {
    m_snapshot1.clear();
    m_snapshot2.clear();
    m_results.clear();

    if (!MftReader::LoadSnapshot(snapshot1_path, m_snapshot1, m_timestamp1)) {
        return false;
    }
    if (!MftReader::LoadSnapshot(snapshot2_path, m_snapshot2, m_timestamp2)) {
        return false;
    }
    return true;
}

bool DiffEngine::ComputeDiff() {
    m_results.clear();

    // Build maps: path -> PathInfo for each snapshot
    // Use case-insensitive comparison for Windows paths
    auto cmp = [](const std::wstring& a, const std::wstring& b) {
        return _wcsicmp(a.c_str(), b.c_str()) < 0;
    };
    std::map<std::wstring, const PathInfo*, decltype(cmp)> map1(cmp);
    std::map<std::wstring, const PathInfo*, decltype(cmp)> map2(cmp);

    for (const auto& e : m_snapshot1) {
        map1[e.path] = &e;
    }
    for (const auto& e : m_snapshot2) {
        map2[e.path] = &e;
    }

    // Process all paths from both snapshots
    std::set<std::wstring, decltype(cmp)> all_paths(cmp);

    // Collect all unique paths from snapshot 1
    for (const auto& e : m_snapshot1) {
        // For directories, we only want to compare them if the user
        // wants directory-level comparison. By default, compare every path.
        all_paths.insert(e.path);
    }

    // Also add paths from snapshot 2 that might not be in snapshot 1 (new paths)
    for (const auto& e : m_snapshot2) {
        all_paths.insert(e.path);
    }

    // Compute diff for each path
    for (const auto& path : all_paths) {
        DiffEntry de;
        de.path = path;
        de.size_before = 0;
        de.size_after = 0;
        de.size_delta = 0;
        de.status = DiffStatus::UNCHANGED;
        de.is_directory = false;

        auto it1 = map1.find(path);
        auto it2 = map2.find(path);

        if (it1 == map1.end() && it2 != map2.end()) {
            // NEW: only in snapshot 2
            de.size_after = it2->second->size;
            de.size_delta = (int64_t)it2->second->size;
            de.status = DiffStatus::NEW;
            de.is_directory = it2->second->is_directory;
        }
        else if (it1 != map1.end() && it2 == map2.end()) {
            // DELETED: only in snapshot 1
            de.size_before = it1->second->size;
            de.size_delta = -((int64_t)it1->second->size);
            de.status = DiffStatus::DELETED;
            de.is_directory = it1->second->is_directory;
        }
        else if (it1 != map1.end() && it2 != map2.end()) {
            // Both exist
            de.size_before = it1->second->size;
            de.size_after = it2->second->size;
            de.is_directory = it1->second->is_directory;

            if (it2->second->size > it1->second->size) {
                de.size_delta = (int64_t)(it2->second->size - it1->second->size);
                de.status = DiffStatus::SIZE_INCREASED;
            } else if (it2->second->size < it1->second->size) {
                de.size_delta = -((int64_t)(it1->second->size - it2->second->size));
                de.status = DiffStatus::SIZE_DECREASED;
            } else {
                de.size_delta = 0;
                de.status = DiffStatus::UNCHANGED;
            }
        }

        m_results.push_back(de);
    }

    // Sort by actual delta descending (positive first, negative last)
    std::sort(m_results.begin(), m_results.end(),
        [](const DiffEntry& a, const DiffEntry& b) {
            if (a.size_delta != b.size_delta) return a.size_delta > b.size_delta;
            return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
        });

    return true;
}

std::vector<DiffEntry> DiffEngine::GetSortedResults(SortBy sort_by,
                                                     bool descending) const {
    auto results = m_results;

    switch (sort_by) {
        case BY_PATH:
            std::sort(results.begin(), results.end(),
                [descending](const DiffEntry& a, const DiffEntry& b) {
                    int cmp = _wcsicmp(a.path.c_str(), b.path.c_str());
                    return descending ? (cmp > 0) : (cmp < 0);
                });
            break;
        case BY_SIZE_BEFORE:
            std::sort(results.begin(), results.end(),
                [descending](const DiffEntry& a, const DiffEntry& b) {
                    return descending ? (a.size_before > b.size_before)
                                      : (a.size_before < b.size_before);
                });
            break;
        case BY_SIZE_AFTER:
            std::sort(results.begin(), results.end(),
                [descending](const DiffEntry& a, const DiffEntry& b) {
                    return descending ? (a.size_after > b.size_after)
                                      : (a.size_after < b.size_after);
                });
            break;
        case BY_DELTA:
        default:
            std::sort(results.begin(), results.end(),
                [descending](const DiffEntry& a, const DiffEntry& b) {
                    if (descending) return a.size_delta > b.size_delta;
                    return a.size_delta < b.size_delta;
                });
            break;
    }

    return results;
}

std::vector<DiffEntry> DiffEngine::FilterByStatus(DiffStatus status) const {
    std::vector<DiffEntry> filtered;
    for (const auto& e : m_results) {
        if (e.status == status) {
            filtered.push_back(e);
        }
    }
    return filtered;
}

// Forward declaration
static std::string FormatSize(int64_t bytes);

bool DiffEngine::ExportCsv(const std::wstring& filepath) const {
    std::string filepath_utf8 = WideToUtf8(filepath);
    std::ofstream out(filepath_utf8, std::ios::binary);
    if (!out) return false;

    // BOM for UTF-8
    out << "\xEF\xBB\xBF";

    // Header
    out << "路径,第一次大小,第二次大小,变化量,变化量(bytes),状态,是目录\n";

    for (const auto& e : m_results) {
        std::string path_utf8 = WideToUtf8(e.path);

        std::string status_str;
        switch (e.status) {
            case DiffStatus::UNCHANGED:    status_str = "不变"; break;
            case DiffStatus::NEW:          status_str = "新增"; break;
            case DiffStatus::DELETED:      status_str = "删除"; break;
            case DiffStatus::SIZE_INCREASED: status_str = "增大"; break;
            case DiffStatus::SIZE_DECREASED: status_str = "减小"; break;
        }

        out << "\"" << path_utf8 << "\","
            << e.size_before << ","
            << e.size_after << ","
            << FormatSize(e.size_delta) << ","
            << e.size_delta << ","
            << status_str << ","
            << (e.is_directory ? "是" : "否") << "\n";
    }

    return true;
}

DiffEngine::DiffSummary DiffEngine::GetSummary() const {
    DiffSummary summary = {};
    summary.total_paths = (uint64_t)m_results.size();

    for (const auto& e : m_results) {
        switch (e.status) {
            case DiffStatus::UNCHANGED:    summary.unchanged++; break;
            case DiffStatus::NEW:          summary.new_paths++; break;
            case DiffStatus::DELETED:      summary.deleted_paths++; break;
            case DiffStatus::SIZE_INCREASED: summary.increased++; break;
            case DiffStatus::SIZE_DECREASED: summary.decreased++; break;
        }
        summary.total_size_delta += e.size_delta;
    }

    return summary;
}

// Helper function for formatting sizes (also used by the HTTP server)
static std::string FormatSize(int64_t bytes) {
    if (bytes == 0) return "0 B";
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double size = (double)llabs(bytes);
    int unit_idx = 0;
    while (size >= 1024.0 && unit_idx < 5) {
        size /= 1024.0;
        unit_idx++;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%s%.2f %s",
             bytes < 0 ? "-" : "", size, units[unit_idx]);
    return std::string(buf);
}
