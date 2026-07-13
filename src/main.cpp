#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <ctime>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <thread>
#include <filesystem>
#include <cstring>
#include <cstdio>

#include "mft_reader.h"
#include "diff_engine.h"
#include "http_server.h"

// ============================================================
// Configuration
// ============================================================
static std::wstring g_data_dir = L"data";
static std::wstring g_web_dir = L"web";
static int g_port = 45678;

// ============================================================
// JSON utilities
// ============================================================
static std::string EscapeJson(const std::string& s) {
    std::string result;
    result.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '\"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    result += buf;
                } else {
                    result += c;
                }
        }
    }
    return result;
}

static std::string JsonBool(bool b) { return b ? "true" : "false"; }

static std::string SizeToString(uint64_t bytes) {
    if (bytes == 0) return "\"0 B\"";
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double size = (double)bytes;
    int unit_idx = 0;
    while (size >= 1024.0 && unit_idx < 5) {
        size /= 1024.0;
        unit_idx++;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "\"%.2f %s\"", size, units[unit_idx]);
    return std::string(buf);
}

static std::string PathsToJson(const std::vector<PathInfo>& paths) {
    std::ostringstream json;
    json << "[\n";
    for (size_t i = 0; i < paths.size(); i++) {
        const auto& p = paths[i];
        json << "  {\n";
        json << "    \"path\": \"" << EscapeJson(WideToUtf8(p.path)) << "\",\n";
        json << "    \"size\": " << p.size << ",\n";
        json << "    \"size_str\": " << SizeToString(p.size) << ",\n";
        json << "    \"modification_time\": " << p.modification_time << ",\n";
        json << "    \"modification_time_str\": \""
             << MftTimeToString(p.modification_time) << "\",\n";
        json << "    \"is_directory\": " << JsonBool(p.is_directory) << "\n";
        json << "  }";
        if (i < paths.size() - 1) json << ",";
        json << "\n";
    }
    json << "]";
    return json.str();
}

static std::string StatusToStr(DiffStatus s) {
    switch (s) {
        case DiffStatus::UNCHANGED:      return "unchanged";
        case DiffStatus::NEW:            return "new";
        case DiffStatus::DELETED:        return "deleted";
        case DiffStatus::SIZE_INCREASED: return "increased";
        case DiffStatus::SIZE_DECREASED: return "decreased";
    }
    return "unknown";
}

static std::string StatusToChinese(DiffStatus s) {
    switch (s) {
        case DiffStatus::UNCHANGED:      return "不变";
        case DiffStatus::NEW:            return "新增";
        case DiffStatus::DELETED:        return "删除";
        case DiffStatus::SIZE_INCREASED: return "增大";
        case DiffStatus::SIZE_DECREASED: return "减小";
    }
    return "未知";
}

static std::string DiffEntriesToJson(const std::vector<DiffEntry>& entries) {
    std::ostringstream json;
    json << "[\n";
    for (size_t i = 0; i < entries.size(); i++) {
        const auto& e = entries[i];
        json << "  {\n";
        json << "    \"path\": \"" << EscapeJson(WideToUtf8(e.path)) << "\",\n";
        json << "    \"size_before\": " << e.size_before << ",\n";
        json << "    \"size_after\": " << e.size_after << ",\n";
        json << "    \"size_delta\": " << e.size_delta << ",\n";
        json << "    \"size_before_str\": " << SizeToString(e.size_before) << ",\n";
        json << "    \"size_after_str\": " << SizeToString(e.size_after) << ",\n";
        json << "    \"size_delta_str\": \"";
        // Compute signed size string
        if (e.size_delta == 0) {
            json << "0 B";
        } else {
            uint64_t abs_delta = (uint64_t)llabs(e.size_delta);
            const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
            double sz = (double)abs_delta;
            int idx = 0;
            while (sz >= 1024.0 && idx < 5) { sz /= 1024.0; idx++; }
            char buf[48];
            snprintf(buf, sizeof(buf), "%c%.2f %s",
                     e.size_delta >= 0 ? '+' : '-', sz, units[idx]);
            json << buf;
        }
        json << "\",\n";
        json << "    \"status\": \"" << StatusToStr(e.status) << "\",\n";
        json << "    \"status_cn\": \"" << StatusToChinese(e.status) << "\",\n";
        json << "    \"is_directory\": " << JsonBool(e.is_directory) << "\n";
        json << "  }";
        if (i < entries.size() - 1) json << ",";
        json << "\n";
    }
    json << "]";
    return json.str();
}

// ============================================================
// Global state
// ============================================================
static std::vector<PathInfo> g_last_scan;
static std::string g_last_scan_timestamp;
static std::vector<std::wstring> g_snapshot_files;
static std::vector<std::string> g_snapshot_timestamps;

static void RefreshSnapshotList() {
    g_snapshot_files.clear();
    g_snapshot_timestamps.clear();

    namespace fs = std::filesystem;
    if (!fs::exists(g_data_dir)) return;

    // Collect .json files
    std::vector<std::pair<std::string, std::wstring>> snapshots;

    for (const auto& entry : fs::directory_iterator(g_data_dir)) {
        if (entry.path().extension() == L".json") {
            std::wstring filepath = entry.path().wstring();
            std::vector<PathInfo> temp;
            std::string timestamp;
            if (MftReader::LoadSnapshot(filepath, temp, timestamp)) {
                snapshots.push_back({timestamp, filepath});
            }
        }
    }

    // Sort by timestamp (most recent last)
    std::sort(snapshots.begin(), snapshots.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });

    for (auto& s : snapshots) {
        g_snapshot_timestamps.push_back(s.first);
        g_snapshot_files.push_back(s.second);
    }
}

// ============================================================
// API request handler
// ============================================================
using Response = HttpServer::Response;

static Response JsonOK(const std::string& json_body) {
    Response r;
    r.body = json_body;
    r.content_type = "application/json";
    return r;
}

static Response JsonError(const std::string& msg, int code = 500) {
    Response r;
    r.status_code = code;
    switch (code) {
        case 400: r.status_text = "Bad Request"; break;
        case 404: r.status_text = "Not Found"; break;
        case 500: r.status_text = "Internal Server Error"; break;
        default:  r.status_text = "Error"; break;
    }
    r.body = "{\"error\": \"" + msg + "\"}\n";
    r.content_type = "application/json";
    return r;
}

static Response HandleRequest(
    const std::string& method,
    const std::string& path,
    const std::string& body)
{
    // Parse query parameters from path
    auto q_pos = path.find('?');
    std::string base_path = path;
    std::string query_str;
    if (q_pos != std::string::npos) {
        base_path = path.substr(0, q_pos);
        query_str = path.substr(q_pos + 1);
    }
    auto query = HttpServer::ParseQueryString(query_str);

    // CORS preflight already handled in HttpServer

    // === API Routes ===

    // GET /api/status
    if (base_path == "/api/status" && method == "GET") {
        std::ostringstream json;
        json << "{\n";
        json << "  \"status\": \"ok\",\n";
        json << "  \"version\": \"1.0.0\",\n";
        json << "  \"name\": \"DiskDelta - NTFS MFT Scanner\"\n";
        json << "}\n";
        return JsonOK(json.str());
    }

    // GET /api/snapshots - list all snapshots
    if (base_path == "/api/snapshots" && method == "GET") {
        RefreshSnapshotList();
        std::ostringstream json;
        json << "{\n";
        json << "  \"snapshots\": [\n";
        for (size_t i = 0; i < g_snapshot_files.size(); i++) {
            namespace fs = std::filesystem;
            std::wstring filename = fs::path(g_snapshot_files[i]).filename().wstring();
            json << "    {\n";
            json << "      \"index\": " << i << ",\n";
            json << "      \"timestamp\": \"" << EscapeJson(g_snapshot_timestamps[i]) << "\",\n";
            json << "      \"filename\": \"" << EscapeJson(WideToUtf8(filename)) << "\",\n";
            json << "      \"filepath\": \"" << EscapeJson(WideToUtf8(g_snapshot_files[i])) << "\"\n";
            json << "    }";
            if (i < g_snapshot_files.size() - 1) json << ",";
            json << "\n";
        }
        json << "  ]\n";
        json << "}\n";
        return JsonOK(json.str());
    }

    // GET /api/snapshot/<index> - get snapshot details
    if (base_path.find("/api/snapshot/") == 0 && method == "GET") {
        std::string idx_str = base_path.substr(strlen("/api/snapshot/"));
        int idx = std::stoi(idx_str);

        RefreshSnapshotList();
        if (idx < 0 || idx >= (int)g_snapshot_files.size()) {
            return JsonError("snapshot index out of range");
        }

        std::vector<PathInfo> paths;
        std::string ts;
        MftReader::LoadSnapshot(g_snapshot_files[idx], paths, ts);

        // Support sorting: ?sort=size&order=desc
        auto sort_it = query.find("sort");
        if (sort_it != query.end() && sort_it->second == "size") {
            bool asc = false;
            auto order_it = query.find("order");
            if (order_it != query.end() && order_it->second == "asc") asc = true;
            std::sort(paths.begin(), paths.end(),
                [asc](const PathInfo& a, const PathInfo& b) {
                    return asc ? (a.size < b.size) : (a.size > b.size);
                });
        }

        std::ostringstream json;
        json << "{\n";
        json << "  \"index\": " << idx << ",\n";
        json << "  \"timestamp\": \"" << EscapeJson(ts) << "\",\n";
        json << "  \"entry_count\": " << paths.size() << ",\n";
        json << "  \"entries\": " << PathsToJson(paths) << "\n";
        json << "}\n";
        return JsonOK(json.str());
    }

    // POST /api/scan - start a new scan
    if (base_path == "/api/scan" && method == "POST") {
        // Parse drive letter from body (JSON)
        std::string drive = "C";
        auto dl_pos = body.find("\"drive\"");
        if (dl_pos != std::string::npos) {
            auto colon = body.find(':', dl_pos);
            auto quote1 = body.find('\"', colon + 1);
            auto quote2 = body.find('\"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                drive = body.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }

        std::wstring wdrive = Utf8ToWide(drive);

        MftReader reader;
        if (!reader.OpenVolume(wdrive)) {
            DWORD err = GetLastError();
            std::string err_msg = "Failed to open volume (error " + std::to_string(err)
                + "). Make sure to run as Administrator.";
            return JsonError(err_msg);
        }

        std::vector<MftEntry> entries;
        if (!reader.ScanMft(entries)) {
            reader.CloseVolume();
            return JsonError("Failed to scan MFT. No entries found.");
        }

        std::map<uint64_t, PathInfo> path_map;
        std::vector<PathInfo> results;
        MftReader::BuildPathTree(entries, path_map, results);
        reader.CloseVolume();

        // Save snapshot
        namespace fs = std::filesystem;
        fs::create_directories(g_data_dir);

        time_t now = time(nullptr);
        struct tm tm_buf;
        localtime_s(&tm_buf, &now);
        char time_buf[64];
        strftime(time_buf, sizeof(time_buf), "%Y%m%d_%H%M%S", &tm_buf);

        std::wstring filename = g_data_dir + L"\\snapshot_" +
                                Utf8ToWide(time_buf) + L".json";
        MftReader::SaveSnapshot(results, filename);

        g_last_scan = results;
        g_last_scan_timestamp = time_buf;

        std::ostringstream json;
        json << "{\n";
        json << "  \"success\": true,\n";
        json << "  \"timestamp\": \"" << time_buf << "\",\n";
        json << "  \"entry_count\": " << results.size() << ",\n";
        json << "  \"filepath\": \"" << EscapeJson(WideToUtf8(filename)) << "\",\n";
        json << "  \"drive\": \"" << EscapeJson(drive) << "\"\n";
        json << "}\n";
        return JsonOK(json.str());
    }

    // POST /api/diff - compare two snapshots
    if (base_path == "/api/diff" && method == "POST") {
        int idx1 = -1, idx2 = -1;

        // Parse from JSON body
        auto parse_int = [&](const std::string& key) -> int {
            auto pos = body.find("\"" + key + "\"");
            if (pos == std::string::npos) return -1;
            auto colon = body.find(':', pos);
            auto end = body.find_first_of(",\n}", colon + 1);
            if (end == std::string::npos) return -1;
            return std::stoi(body.substr(colon + 1, end - colon - 1));
        };

        idx1 = parse_int("snapshot1");
        idx2 = parse_int("snapshot2");

        if (idx1 < 0 || idx2 < 0) {
            return JsonError("Both snapshot1 and snapshot2 indices are required.");
        }

        RefreshSnapshotList();

        if (idx1 >= (int)g_snapshot_files.size() ||
            idx2 >= (int)g_snapshot_files.size()) {
            return JsonError("Snapshot index out of range.");
        }

        DiffEngine diff;
        if (!diff.LoadSnapshots(g_snapshot_files[idx1], g_snapshot_files[idx2])) {
            return JsonError("Failed to load snapshots.");
        }

        if (!diff.ComputeDiff()) {
            return JsonError("Failed to compute diff.");
        }

        auto summary = diff.GetSummary();
        const auto& results = diff.GetResults();

        std::ostringstream json;
        json << "{\n";
        json << "  \"snapshot1_timestamp\": \"" << EscapeJson(diff.GetTimestamp1()) << "\",\n";
        json << "  \"snapshot2_timestamp\": \"" << EscapeJson(diff.GetTimestamp2()) << "\",\n";
        json << "  \"total_entries\": " << results.size() << ",\n";
        json << "  \"summary\": {\n";
        json << "    \"total_paths\": " << summary.total_paths << ",\n";
        json << "    \"unchanged\": " << summary.unchanged << ",\n";
        json << "    \"new_paths\": " << summary.new_paths << ",\n";
        json << "    \"deleted_paths\": " << summary.deleted_paths << ",\n";
        json << "    \"increased\": " << summary.increased << ",\n";
        json << "    \"decreased\": " << summary.decreased << ",\n";
        json << "    \"total_size_delta\": " << summary.total_size_delta << "\n";
        json << "  },\n";

        // Support pagination via query params
        int page = 0;
        uint64_t page_size = (uint64_t)results.size(); // Default: return all
        auto it = query.find("page");
        if (it != query.end()) page = std::stoi(it->second);
        it = query.find("page_size");
        if (it != query.end()) page_size = std::stoi(it->second);

        size_t start = (size_t)page * (size_t)page_size;
        if (start >= results.size()) start = 0;
        size_t end = std::min(start + (size_t)page_size, results.size());

        std::vector<DiffEntry> page_results;
        for (size_t i = start; i < end; i++) {
            page_results.push_back(results[i]);
        }

        json << "  \"page\": " << page << ",\n";
        json << "  \"page_size\": " << page_size << ",\n";
        json << "  \"returned_count\": " << page_results.size() << ",\n";
        json << "  \"entries\": " << DiffEntriesToJson(page_results) << "\n";
        json << "}\n";
        return JsonOK(json.str());
    }

    // GET /api/export/<idx1>/<idx2> - export diff as CSV
    if (base_path.find("/api/export/") == 0 && method == "GET") {
        std::string remaining = base_path.substr(strlen("/api/export/"));
        auto slash = remaining.find('/');
        if (slash == std::string::npos) {
            return JsonError("Need two snapshot indices: /api/export/idx1/idx2");
        }
        int idx1 = std::stoi(remaining.substr(0, slash));
        int idx2 = std::stoi(remaining.substr(slash + 1));

        RefreshSnapshotList();
        if (idx1 >= (int)g_snapshot_files.size() ||
            idx2 >= (int)g_snapshot_files.size()) {
            return JsonError("Snapshot index out of range.");
        }

        DiffEngine diff;
        if (!diff.LoadSnapshots(g_snapshot_files[idx1], g_snapshot_files[idx2])) {
            return JsonError("Failed to load snapshots.");
        }
        if (!diff.ComputeDiff()) {
            return JsonError("Failed to compute diff.");
        }

        namespace fs = std::filesystem;
        fs::create_directories(g_data_dir);
        std::wstring csv_path = g_data_dir + L"\\diff_result.csv";
        diff.ExportCsv(csv_path);

        // Return CSV as text
        std::string csv_path_utf8 = WideToUtf8(csv_path);
        std::ifstream csv_file(csv_path_utf8, std::ios::binary);
        if (!csv_file) {
            return JsonError("Failed to create CSV.");
        }

        std::string csv_content((std::istreambuf_iterator<char>(csv_file)),
                                 std::istreambuf_iterator<char>());

        Response csv_resp;
        csv_resp.body = csv_content;
        csv_resp.content_type = "text/csv; charset=utf-8";
        return csv_resp;
    }

    // GET /api/lastscan - get last scan results
    if (base_path == "/api/lastscan" && method == "GET") {
        std::ostringstream json;
        json << "{\n";
        json << "  \"timestamp\": \"" << EscapeJson(g_last_scan_timestamp) << "\",\n";
        json << "  \"entry_count\": " << g_last_scan.size() << ",\n";
        json << "  \"entries\": " << PathsToJson(g_last_scan) << "\n";
        json << "}\n";
        return JsonOK(json.str());
    }

    // === Static file serving (web directory) ===
    {
        std::string serve_path = base_path;
        if (serve_path == "/" || serve_path.empty()) {
            serve_path = "/index.html";
        }

        // Build file path
        std::wstring wpath = g_web_dir + Utf8ToWide(serve_path);
        std::string wpath_utf8 = WideToUtf8(wpath);
        std::ifstream file(wpath_utf8, std::ios::binary);
        if (file) {
            std::string content((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());

            // Determine content type (C++17 compatible)
            auto has_suffix = [](const std::string& str, const std::string& suffix) {
                if (str.size() < suffix.size()) return false;
                return str.compare(str.size() - suffix.size(), suffix.size(), suffix) == 0;
            };
            std::string ct = "application/octet-stream";
            if (has_suffix(serve_path, ".html")) ct = "text/html; charset=utf-8";
            else if (has_suffix(serve_path, ".css"))  ct = "text/css; charset=utf-8";
            else if (has_suffix(serve_path, ".js"))   ct = "application/javascript";
            else if (has_suffix(serve_path, ".json")) ct = "application/json";
            else if (has_suffix(serve_path, ".png"))  ct = "image/png";
            else if (has_suffix(serve_path, ".svg"))  ct = "image/svg+xml";
            else if (has_suffix(serve_path, ".ico"))  ct = "image/x-icon";

            Response file_resp;
            file_resp.body = content;
            file_resp.content_type = ct;
            return file_resp;
        }
    }

    // Fallback - return 404
    return JsonError("Not found: " + base_path, 404);
}

// ============================================================
// Entry point
// ============================================================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    // Allocate console window for debug output
    AllocConsole();
    FILE* console_file;
    freopen_s(&console_file, "CONOUT$", "w", stdout);
    SetConsoleTitleW(L"DiskDelta - Debug Console");
    std::cout << "========================================" << std::endl;
    std::cout << "  DiskDelta - NTFS MFT Scanner & Diff" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    // Single-instance check: kill any previous DiskDelta process
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"DiskDelta_Instance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::cout << "[Init] Previous instance detected, terminating..." << std::endl;
        CloseHandle(mutex);
        // Kill other instances
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = { sizeof(pe) };
            DWORD current_pid = GetCurrentProcessId();
            if (Process32FirstW(snapshot, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, L"DiskDelta.exe") == 0
                        && pe.th32ProcessID != current_pid) {
                        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                        if (h) {
                            TerminateProcess(h, 0);
                            CloseHandle(h);
                            std::cout << "[Init] Killed old process (PID "
                                      << pe.th32ProcessID << ")" << std::endl;
                        }
                    }
                } while (Process32NextW(snapshot, &pe));
            }
            CloseHandle(snapshot);
        }
        // Re-create mutex after cleanup
        mutex = CreateMutexW(nullptr, FALSE, L"DiskDelta_Instance_Mutex");
    }
    std::cout << "[Init] Starting DiskDelta..." << std::endl;

    // Parse command line
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    int port = g_port;
    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg == L"--port" && i + 1 < argc) {
            port = _wtoi(argv[++i]);
        } else if (arg == L"--datadir" && i + 1 < argc) {
            g_data_dir = argv[++i];
        } else if (arg == L"--webdir" && i + 1 < argc) {
            g_web_dir = argv[++i];
        } else if (arg == L"--help" || arg == L"/?" || arg == L"-h") {
            MessageBoxW(nullptr,
                L"DiskDelta - NTFS MFT Scanner & Diff Tool\n\n"
                L"Usage: DiskDelta.exe [options]\n\n"
                L"Options:\n"
                L"  --port <number>     HTTP server port (default: 45678)\n"
                L"  --datadir <path>    Data directory (default: data)\n"
                L"  --webdir <path>     Web UI directory (default: web)\n"
                L"  --help              Show this help",
                L"DiskDelta Help", MB_OK);
            return 0;
        }
    }

    LocalFree(argv);

    // Ensure data directory exists
    namespace fs = std::filesystem;
    fs::create_directories(g_data_dir);
    std::cout << "[Setup] Data directory: " << WideToUtf8(g_data_dir) << std::endl;

    // Refresh snapshot list
    RefreshSnapshotList();
    std::cout << "[Setup] Found " << g_snapshot_files.size() << " existing snapshots" << std::endl;

    // Start HTTP server
    HttpServer server;
    if (!server.Start(port, HandleRequest)) {
        std::wstring msg = L"Failed to start HTTP server on port " +
                           std::to_wstring(port) +
                           L".\n\nMake sure the port is not in use.";
        std::cout << "[ERROR] Failed to start HTTP server on port "
                  << port << std::endl;
        MessageBoxW(nullptr, msg.c_str(), L"DiskDelta Error", MB_ICONERROR);
        return 1;
    }
    std::cout << "[Server] HTTP server started on http://127.0.0.1:"
              << port << "/" << std::endl;

    // Open browser to the web UI
    {
        std::wstring url = L"http://127.0.0.1:" + std::to_wstring(port) + L"/";
        // Use cmd /c start to work across UAC integrity levels
        std::wstring cmd = L"cmd.exe /c start \"\" \"" + url + L"\"";
        PROCESS_INFORMATION pi = {};
        STARTUPINFOW si = { sizeof(si) };
        CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    // Run message loop
    std::cout << "[Server] Ready. Close this window to stop the server." << std::endl;
    std::cout << std::endl;
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    std::cout << "[Server] Shutting down..." << std::endl;

    server.Stop();
    return 0;
}
