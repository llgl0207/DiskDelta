#include "mft_reader.h"
#include <algorithm>
#include <set>
#include <queue>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <winioctl.h>

// ============================================================
// Debug logging - outputs to console and log file
// ============================================================
static void DebugLog(const std::string& msg) {
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_s(&tm_buf, &now);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);

    // Output to console (if available)
    std::cout << "[" << buf << "] " << msg << std::endl;

    // Also write to log file
    std::ofstream log("diskdelta_debug.log", std::ios::app);
    if (log) {
        log << "[" << buf << "] " << msg << std::endl;
    }
}

// ============================================================
// UTF-8 / UTF-16 conversion
// ============================================================
std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                                  nullptr, 0, nullptr, nullptr);
    std::string result(len, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(),
                        &result[0], len, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& str) {
    if (str.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(),
                                  nullptr, 0);
    std::wstring result(len, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(),
                        &result[0], len);
    return result;
}

uint64_t FileTimeToUnixSeconds(uint64_t filetime) {
    // Convert Windows FILETIME (100-ns intervals since 1601-01-01)
    // to Unix timestamp (seconds since 1970-01-01)
    const uint64_t EPOCH_DIFFERENCE = 11644473600ULL;
    return (filetime / 10000000ULL) - EPOCH_DIFFERENCE;
}

std::string MftTimeToString(uint64_t mft_time) {
    uint64_t unix_secs = FileTimeToUnixSeconds(mft_time);
    time_t t = (time_t)unix_secs;
    struct tm tm_buf;
    localtime_s(&tm_buf, &t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return std::string(buf);
}

// ============================================================
// MftReader implementation
// ============================================================
MftReader::MftReader()
    : m_volume_handle(INVALID_HANDLE_VALUE)
    , m_mft_handle(INVALID_HANDLE_VALUE)
    , m_mft_size(0)
    , m_bytes_per_cluster(0)
    , m_bytes_per_mft_record(0)
    , m_mft_start_lcn(0) {
}

MftReader::~MftReader() {
    CloseVolume();
}

bool MftReader::OpenVolume(const std::wstring& drive_letter) {
    // Store drive letter
    m_drive_letter = drive_letter.substr(0, 1) + L":";

    // Build volume path: \\.\C:
    std::wstring volume_path = L"\\\\.\\" + m_drive_letter;

    m_volume_handle = CreateFileW(
        volume_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
        nullptr
    );

    if (m_volume_handle == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        DebugLog("OpenVolume: CreateFileW failed for " + WideToUtf8(volume_path)
                 + " error=" + std::to_string(err));
        return false;
    }
    DebugLog("OpenVolume: " + WideToUtf8(volume_path) + " opened successfully");

    if (!GetVolumeInfo()) {
        DebugLog("OpenVolume: GetVolumeInfo failed");
        CloseVolume();
        return false;
    }
    DebugLog("OpenVolume: bytes_per_cluster=" + std::to_string(m_bytes_per_cluster));

    if (!LocateMft()) {
        DebugLog("OpenVolume: LocateMft failed");
        CloseVolume();
        return false;
    }
    DebugLog("OpenVolume: mft_size=" + std::to_string(m_mft_size)
             + " bytes_per_mft_record=" + std::to_string(m_bytes_per_mft_record)
             + " mft_start_lcn=" + std::to_string(m_mft_start_lcn));

    return true;
}

void MftReader::CloseVolume() {
    if (m_mft_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_mft_handle);
        m_mft_handle = INVALID_HANDLE_VALUE;
    }
    if (m_volume_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_volume_handle);
        m_volume_handle = INVALID_HANDLE_VALUE;
    }
}

bool MftReader::GetVolumeInfo() {
    DWORD bytes_per_sector, sectors_per_cluster, free_clusters, total_clusters;
    std::wstring root_path = m_drive_letter + L"\\";
    if (!GetDiskFreeSpaceW(root_path.c_str(), &sectors_per_cluster,
                           &bytes_per_sector, &free_clusters, &total_clusters)) {
        DebugLog("GetVolumeInfo: GetDiskFreeSpaceW failed for " + WideToUtf8(root_path));
        return false;
    }
    m_bytes_per_cluster = bytes_per_sector * sectors_per_cluster;
    return true;
}

bool MftReader::LocateMft() {
    // Use NTFS_VOLUME_DATA_BUFFER via DeviceIoControl to get MFT info
    // This is more reliable than manually parsing the boot sector
    NTFS_VOLUME_DATA_BUFFER nvdb = {};

    // First try the extended buffer which is available on newer Windows
    std::vector<uint8_t> ext_buf(sizeof(NTFS_VOLUME_DATA_BUFFER) + sizeof(NTFS_EXTENDED_VOLUME_DATA));
    DWORD bytes_returned = 0;

    BOOL result = DeviceIoControl(
        m_volume_handle,
        FSCTL_GET_NTFS_VOLUME_DATA,
        nullptr, 0,
        ext_buf.data(), (DWORD)ext_buf.size(),
        &bytes_returned,
        nullptr
    );

    if (!result || bytes_returned < sizeof(NTFS_VOLUME_DATA_BUFFER)) {
        DebugLog("LocateMft: FSCTL_GET_NTFS_VOLUME_DATA failed, error="
                 + std::to_string(GetLastError()));
        return false;
    }

    memcpy(&nvdb, ext_buf.data(), sizeof(NTFS_VOLUME_DATA_BUFFER));

    m_mft_start_lcn = nvdb.MftStartLcn.QuadPart;
    m_bytes_per_mft_record = nvdb.BytesPerFileRecordSegment;
    m_mft_size = (uint64_t)nvdb.MftValidDataLength.QuadPart;

    DebugLog("LocateMft: MFT start LCN=" + std::to_string(m_mft_start_lcn)
             + " bytes_per_record=" + std::to_string(m_bytes_per_mft_record)
             + " mft_valid_size=" + std::to_string(m_mft_size));

    // Validate: if MFT size is 0 or record size is unreasonable, try boot sector
    if (m_mft_size == 0 || m_bytes_per_mft_record == 0 ||
        m_bytes_per_mft_record > 65536) {
        DebugLog("LocateMft: FSCTL returned invalid values, falling back to boot sector");

        // Read boot sector manually
        const DWORD SECTOR_SIZE = 512;
        std::vector<uint8_t> boot_sector(SECTOR_SIZE, 0);
        LARGE_INTEGER offset = {};
        SetFilePointerEx(m_volume_handle, offset, nullptr, FILE_BEGIN);
        DWORD br;
        ReadFile(m_volume_handle, boot_sector.data(), SECTOR_SIZE, &br, nullptr);

        // Validate OEM ID
        char oem[9] = {};
        memcpy(oem, &boot_sector[0x03], 8);
        if (memcmp(oem, "NTFS    ", 8) != 0) {
            std::string hex;
            for (int i = 0; i < 64; i++) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X ", boot_sector[i]);
                hex += buf;
            }
            DebugLog("LocateMft: Boot sector not NTFS! OEM=\"" + std::string(oem, 8)
                     + "\" hex=" + hex);
        }

        m_mft_start_lcn = *(uint64_t*)(&boot_sector[0x28]);
        int8_t record_size_clusters = (int8_t)boot_sector[0x38];
        if (record_size_clusters > 0) {
            m_bytes_per_mft_record = record_size_clusters * m_bytes_per_cluster;
        } else {
            m_bytes_per_mft_record = (uint64_t)1 << (-record_size_clusters);
        }

        // Validate MFT start LCN
        std::wstring rp = m_drive_letter + L"\\";
        DWORD sc, bs, fc, tc;
        if (GetDiskFreeSpaceW(rp.c_str(), &sc, &bs, &fc, &tc)) {
            if (m_mft_start_lcn >= (uint64_t)tc) {
                DebugLog("LocateMft: bad LCN=" + std::to_string(m_mft_start_lcn)
                         + " >= total_clusters=" + std::to_string(tc));
                m_mft_start_lcn = (4ULL * 1024 * 1024 * 1024) / m_bytes_per_cluster;
                m_bytes_per_mft_record = 1024;  // Standard MFT record size
                DebugLog("LocateMft: using fallback LCN=" + std::to_string(m_mft_start_lcn)
                         + " record_size=1024");
            }
        }

        // Estimate MFT size from volume
        DWORD sc2, bs2, fc2, tc2;
        if (GetDiskFreeSpaceW(rp.c_str(), &sc2, &bs2, &fc2, &tc2)) {
            uint64_t vol_bytes = (uint64_t)tc2 * sc2 * bs2;
            m_mft_size = (vol_bytes / 4096) * m_bytes_per_mft_record;
            DebugLog("LocateMft: estimated mft_size=" + std::to_string(m_mft_size));
        } else {
            m_mft_size = 1024ULL * 1024 * m_bytes_per_mft_record;
        }
    }

    return true;
}

bool MftReader::ReadMftRecord(uint64_t record_number, std::vector<uint8_t>& buffer) {
    // Calculate byte offset: MFT starts at m_mft_start_lcn * bytes_per_cluster
    // Each record is m_bytes_per_mft_record bytes
    uint64_t record_offset = record_number * m_bytes_per_mft_record;
    uint64_t abs_offset = m_mft_start_lcn * m_bytes_per_cluster + record_offset;

    // Seek to position
    LARGE_INTEGER li;
    li.QuadPart = abs_offset;
    if (!SetFilePointerEx(m_volume_handle, li, nullptr, FILE_BEGIN)) {
        DWORD err = GetLastError();
        DebugLog("ReadMftRecord: SetFilePointerEx failed at offset "
                 + std::to_string(abs_offset) + " for record "
                 + std::to_string(record_number) + " error=" + std::to_string(err));
        return false;
    }

    // Read the record
    buffer.resize((size_t)m_bytes_per_mft_record);
    DWORD bytes_read;
    if (!ReadFile(m_volume_handle, buffer.data(), (DWORD)m_bytes_per_mft_record,
                  &bytes_read, nullptr)) {
        DWORD err = GetLastError();
        DebugLog("ReadMftRecord: ReadFile failed at offset "
                 + std::to_string(abs_offset) + " for record "
                 + std::to_string(record_number) + " error=" + std::to_string(err));
        return false;
    }

    if (bytes_read < (DWORD)m_bytes_per_mft_record) {
        DebugLog("ReadMftRecord: Short read at offset " + std::to_string(abs_offset)
                 + " for record " + std::to_string(record_number)
                 + " got " + std::to_string(bytes_read) + " expected "
                 + std::to_string(m_bytes_per_mft_record));
        return false;
    }

    // Verify magic: "FILE" or "BAAD"
    if (memcmp(buffer.data(), "FILE", 4) != 0 && memcmp(buffer.data(), "BAAD", 4) != 0) {
        DebugLog("ReadMftRecord: Bad magic at record " + std::to_string(record_number));
        return false;
    }

    // Apply USA fixup
    FILE_RECORD_HEADER* hdr = (FILE_RECORD_HEADER*)buffer.data();
    return ApplyFixup(buffer, hdr->usa_offset, hdr->usa_size);
}

bool MftReader::ApplyFixup(std::vector<uint8_t>& buffer,
                           uint16_t usa_offset, uint16_t usa_size) {
    if (usa_offset == 0 || usa_size < 2) return true;

    // The USA is at the end of each sector
    // usa_offset points to the USA array in the record
    // The first entry is the "magic" (USA signature)
    // Each subsequent entry is the fixup value for the last 2 bytes of each sector

    uint16_t* usa = (uint16_t*)(buffer.data() + usa_offset);
    uint16_t usa_magic = usa[0];
    const uint16_t SECTOR_SIZE = 512;

    size_t num_sectors = buffer.size() / SECTOR_SIZE;
    if (num_sectors == 0) return true;

    size_t max_fixups = (size_t)(usa_size - 1);
    if (num_sectors > max_fixups) {
        num_sectors = max_fixups;
    }

    for (size_t i = 0; i < num_sectors; i++) {
        uint16_t* last_word = (uint16_t*)(buffer.data() + (i + 1) * SECTOR_SIZE - 2);
        if (*last_word == usa_magic) {
            *last_word = usa[i + 1];
        }
    }

    return true;
}

bool MftReader::ScanMft(std::vector<MftEntry>& entries) {
    if (m_volume_handle == INVALID_HANDLE_VALUE) {
        DebugLog("ScanMft: invalid volume handle");
        return false;
    }

    uint64_t num_records = m_mft_size / m_bytes_per_mft_record;
    uint64_t mft_start_offset = m_mft_start_lcn * m_bytes_per_cluster;
    DebugLog("ScanMft: scanning " + std::to_string(num_records) + " records (mft_size="
             + std::to_string(m_mft_size) + ")");

    // Reserve space
    entries.reserve((size_t)std::min(num_records, (uint64_t)2000000));

    // Read MFT in large chunks (8MB each) instead of record-by-record
    const uint64_t CHUNK_SIZE = 8ULL * 1024 * 1024; // 8MB
    std::vector<uint8_t> chunk(CHUNK_SIZE);

    uint64_t total_valid = 0;
    uint64_t records_processed = 0;
    uint64_t bytes_remaining = m_mft_size;

    // Seek to start of MFT once
    LARGE_INTEGER base_offset;
    base_offset.QuadPart = mft_start_offset;
    if (!SetFilePointerEx(m_volume_handle, base_offset, nullptr, FILE_BEGIN)) {
        DebugLog("ScanMft: failed to seek to MFT start");
        return false;
    }

    while (bytes_remaining > 0 && records_processed < num_records) {
        uint64_t to_read = std::min(CHUNK_SIZE, bytes_remaining);
        // Align to page boundary for efficient reading
        to_read = (to_read + 4095) & ~(uint64_t)4095;
        if (to_read > chunk.size()) {
            chunk.resize((size_t)to_read);
        }

        DWORD bytes_read = 0;
        if (!ReadFile(m_volume_handle, chunk.data(), (DWORD)to_read,
                      &bytes_read, nullptr)) {
            DWORD err = GetLastError();
            DebugLog("ScanMft: ReadFile failed at offset "
                     + std::to_string(mft_start_offset + (m_mft_size - bytes_remaining))
                     + " error=" + std::to_string(err));
            break; // Continue with what we have
        }

        if (bytes_read == 0) break;

        // Process all complete records in this chunk
        uint64_t chunk_offset = 0;
        uint64_t max_offset = (uint64_t)bytes_read;

        while (chunk_offset + m_bytes_per_mft_record <= max_offset) {
            uint64_t current_record = records_processed;
            uint8_t* record_start = chunk.data() + chunk_offset;

            // Quick check: "FILE" or "BAAD" magic
            if (memcmp(record_start, "FILE", 4) == 0 ||
                memcmp(record_start, "BAAD", 4) == 0) {

                FILE_RECORD_HEADER* hdr = (FILE_RECORD_HEADER*)record_start;

                // Check in-use flag before copying
                if (hdr->flags & 0x01) {
                    // Skip extension records quickly
                    if (hdr->base_record_ref == 0) {
                        // Copy record to work buffer and apply fixup
                        std::vector<uint8_t> rec_buf(
                            record_start,
                            record_start + (size_t)m_bytes_per_mft_record);

                        if (ApplyFixup(rec_buf, hdr->usa_offset, hdr->usa_size)) {
                            MftEntry entry;
                            entry.record_number = current_record;
                            entry.valid = false;
                            entry.is_directory = (hdr->flags & 0x02) != 0;
                            entry.parent_ref = 0;
                            entry.parent_seq = 0;
                            entry.parent_record = 0;
                            entry.real_size = 0;
                            entry.allocated_size = 0;
                            entry.modification_time = 0;
                            entry.creation_time = 0;

                            if (ParseAttributes(rec_buf, entry) && entry.valid) {
                                entries.push_back(std::move(entry));
                                total_valid++;

                                if (total_valid % 50000 == 0) {
                                    DebugLog("ScanMft: " + std::to_string(total_valid)
                                             + " valid entries (record "
                                             + std::to_string(current_record) + ")");
                                }
                            }
                        }
                    }
                }
            }

            chunk_offset += m_bytes_per_mft_record;
            records_processed++;
        }

        bytes_remaining -= std::min(bytes_remaining, (uint64_t)bytes_read);
    }

    DebugLog("ScanMft: complete - " + std::to_string(total_valid)
             + " valid entries out of " + std::to_string(records_processed)
             + " records scanned");
    return !entries.empty();
}

bool MftReader::ParseAttributes(const std::vector<uint8_t>& buffer,
                                 MftEntry& entry) {
    FILE_RECORD_HEADER* header = (FILE_RECORD_HEADER*)buffer.data();
    uint16_t attr_offset = header->attr_offset;

    bool has_file_name = false;

    while (attr_offset < header->allocated_size) {
        ATTR_HEADER* attr = (ATTR_HEADER*)(buffer.data() + attr_offset);

        if (attr->type == 0xFFFFFFFF || attr->length == 0) break;

        switch (attr->type) {
            case ATTR_FILE_NAME: {
                // Parse $FILE_NAME (always resident)
                if (ParseFileName(buffer.data() + attr_offset, attr->length,
                                  entry, !attr->non_resident)) {
                    has_file_name = true;
                }
                break;
            }
            case ATTR_DATA: {
                // Parse $DATA for size
                uint64_t real_sz = 0, alloc_sz = 0;
                if (ParseDataSize(buffer.data() + attr_offset, attr->length,
                                  attr->non_resident != 0, real_sz, alloc_sz)) {
                    entry.real_size = real_sz;
                    entry.allocated_size = alloc_sz;
                }
                break;
            }
        }

        attr_offset += attr->length;
        if (attr->length == 0) break;
    }

    entry.valid = has_file_name;
    return has_file_name;
}

bool MftReader::ParseFileName(const uint8_t* attr_start, uint32_t attr_length,
                               MftEntry& entry, bool is_resident) {
    if (!is_resident) return false;

    ATTR_HEADER* attr = (ATTR_HEADER*)attr_start;
    ATTR_HEADER_RESIDENT* res = (ATTR_HEADER_RESIDENT*)((uint8_t*)attr + sizeof(ATTR_HEADER));

    // IMPORTANT: value_offset is relative to the START of the attribute,
    // NOT from the resident structure!
    // So value_start = value_offset (not sizeof(ATTR_HEADER) + value_offset)
    uint32_t value_start = res->value_offset;
    uint32_t value_bytes_avail = attr->length - value_start;

    if (value_bytes_avail < sizeof(FILE_NAME_ATTR)) {
        // Debug: dump attribute for analysis
        std::string hex;
        for (uint32_t i = 0; i < 48 && i < attr->length; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", attr_start[i]);
            hex += buf;
        }
        DebugLog("ParseFileName: bounds fail! value_off="
                 + std::to_string(res->value_offset)
                 + " sizeof(FN)=" + std::to_string(sizeof(FILE_NAME_ATTR))
                 + " avail=" + std::to_string(value_bytes_avail)
                 + " attr_len=" + std::to_string(attr->length)
                 + " hex=" + hex);
        return false;
    }

    FILE_NAME_ATTR* fn = (FILE_NAME_ATTR*)(attr_start + value_start);

    // Validate name_length before using it
    if (fn->name_length == 0 || fn->name_length > 255) {
        std::string hex;
        int dump_len = (int)sizeof(FILE_NAME_ATTR) + 16;
        if (dump_len > (int)value_bytes_avail)
            dump_len = (int)value_bytes_avail;
        if (dump_len > 80) dump_len = 80;
        for (int i = 0; i < dump_len; i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", ((uint8_t*)fn)[i]);
            hex += buf;
        }
        DebugLog("ParseFileName: bad name_len=" + std::to_string(fn->name_length)
                 + " type=" + std::to_string(fn->name_type)
                 + " v_off=" + std::to_string(res->value_offset)
                 + " v_len=" + std::to_string(res->value_length)
                 + " a_len=" + std::to_string(attr->length)
                 + " FN=" + hex);
        return false;
    }

    // We want the long name (name_type == 0x01 or 0x03)
    // 0x01 = Win32 long name, 0x02 = short (8.3) name, 0x03 = both
    if (fn->name_type == 0x02) return false;

    // Extract parent reference
    entry.parent_ref = fn->parent_ref;
    entry.parent_seq = (uint16_t)(fn->parent_ref >> 48);
    entry.parent_record = fn->parent_ref & 0xFFFFFFFFFFFFULL;

    // Extract file name
    uint32_t name_len = fn->name_length;
    if (name_len > 0 && name_len < 512) {
        entry.filename.assign(fn->name, name_len);
    }

    // Extract timestamps
    entry.creation_time = fn->creation_time;
    entry.modification_time = fn->modification_time;

    // Update size from $FILE_NAME as fallback
    if (entry.real_size == 0) {
        entry.real_size = fn->real_size;
        entry.allocated_size = fn->allocated_size;
    }

    return true;
}

bool MftReader::ParseDataSize(const uint8_t* attr_start, uint32_t attr_length,
                               bool non_resident, uint64_t& real_size,
                               uint64_t& allocated_size) {
    if (non_resident) {
        ATTR_HEADER_NONRESIDENT* nr =
            (ATTR_HEADER_NONRESIDENT*)(attr_start + sizeof(ATTR_HEADER));
        real_size = nr->real_size;
        allocated_size = nr->allocated_size;
        return true;
    } else {
        ATTR_HEADER_RESIDENT* res =
            (ATTR_HEADER_RESIDENT*)(attr_start + sizeof(ATTR_HEADER));
        real_size = res->value_length;
        allocated_size = res->value_length;
        return true;
    }
}

// ============================================================
// Path tree building
// ============================================================
struct TreeNode {
    uint64_t record_number;
    std::wstring name;
    std::vector<uint64_t> children;
    uint64_t recursive_size;
    uint64_t modification_time;
    bool is_directory;
};

bool MftReader::BuildPathTree(
    const std::vector<MftEntry>& entries,
    std::map<uint64_t, PathInfo>& path_map,
    std::vector<PathInfo>& results)
{
    // Build record_number -> entry map
    std::map<uint64_t, const MftEntry*> entry_map;
    for (const auto& e : entries) {
        entry_map[e.record_number] = &e;
    }

    // Build parent -> children map
    std::map<uint64_t, std::vector<uint64_t>> parent_children;
    for (const auto& e : entries) {
        // Skip root
        if (e.record_number == 5) continue; // Root directory is record 5
        parent_children[e.parent_record].push_back(e.record_number);
    }

    // Build path for each entry using iterative DFS or BFS
    // We'll walk from root (record 5) downwards
    std::map<uint64_t, std::wstring> record_paths;
    record_paths[5] = L""; // Root

    // BFS to build paths
    std::queue<uint64_t> q;
    q.push(5);

    while (!q.empty()) {
        uint64_t current = q.front();
        q.pop();

        auto it = parent_children.find(current);
        if (it == parent_children.end()) continue;

        for (uint64_t child_record : it->second) {
            auto entry_it = entry_map.find(child_record);
            if (entry_it == entry_map.end()) continue;

            const MftEntry& child = *entry_it->second;
            std::wstring child_path;

            if (current == 5) {
                // Direct child of root
                child_path = L"\\" + child.filename;
            } else {
                child_path = record_paths[current] + L"\\" + child.filename;
            }

            record_paths[child_record] = child_path;

            // Store in path_map
            PathInfo pi;
            pi.path = child_path;
            pi.size = child.real_size;
            pi.modification_time = child.modification_time;
            pi.is_directory = child.is_directory;
            path_map[child_record] = pi;

            q.push(child_record);
        }
    }

    // Calculate recursive sizes for directories
    // Process in reverse order (children before parents)
    // Since record numbers roughly increase, but we need proper topological order
    // Build a reverse map: record -> list of ancestors up to root
    std::vector<uint64_t> ordered_records;
    for (auto& p : path_map) {
        uint64_t rec = p.first;
        // Only include entries that had their paths built
        if (!p.second.path.empty() || rec == 5) {
            ordered_records.push_back(rec);
        }
    }

    // Sort by path depth (longer paths = children first)
    std::sort(ordered_records.begin(), ordered_records.end(),
        [&path_map](uint64_t a, uint64_t b) {
            return path_map[a].path.size() > path_map[b].path.size();
        });

    // For each entry, add its size to all parent directories
    for (uint64_t rec : ordered_records) {
        auto it = entry_map.find(rec);
        if (it == entry_map.end()) continue;

        // Add file's own size to its directory
        uint64_t parent_rec = it->second->parent_record;

        // Walk up the tree adding size
        uint64_t current_rec = parent_rec;
        uint64_t file_size = path_map[rec].size;

        while (current_rec != 5 && path_map.count(current_rec)) {
            path_map[current_rec].size += file_size;

            auto parent_it = entry_map.find(current_rec);
            if (parent_it == entry_map.end()) break;
            current_rec = parent_it->second->parent_record;
        }
        // Add to root too
        if (path_map.count(5)) {
            path_map[5].size += file_size;
        }
    }

    // Collect results (skip root)
    results.clear();
    for (auto& p : path_map) {
        if (p.first == 5) continue; // Skip root
        if (!p.second.path.empty()) {
            results.push_back(p.second);
        }
    }

    // Sort results by path
    std::sort(results.begin(), results.end(),
        [](const PathInfo& a, const PathInfo& b) {
            return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
        });

    return !results.empty();
}

// ============================================================
// JSON Snapshot I/O
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

bool MftReader::SaveSnapshot(const std::vector<PathInfo>& results,
                              const std::wstring& filepath) {
    std::string filepath_utf8 = WideToUtf8(filepath);
    std::ofstream out(filepath_utf8, std::ios::binary);
    if (!out) return false;

    // Get current time
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_s(&tm_buf, &now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

    out << "{\n";
    out << "  \"timestamp\": \"" << time_buf << "\",\n";
    out << "  \"version\": 1,\n";
    out << "  \"entry_count\": " << results.size() << ",\n";
    out << "  \"entries\": [\n";

    for (size_t i = 0; i < results.size(); i++) {
        const auto& r = results[i];
        std::string path_utf8 = WideToUtf8(r.path);
        out << "    {\n";
        out << "      \"path\": \"" << EscapeJson(path_utf8) << "\",\n";
        out << "      \"size\": " << r.size << ",\n";
        out << "      \"modification_time\": " << r.modification_time << ",\n";
        out << "      \"modification_time_str\": \""
            << MftTimeToString(r.modification_time) << "\",\n";
        out << "      \"is_directory\": " << (r.is_directory ? "true" : "false") << "\n";
        out << "    }";
        if (i < results.size() - 1) out << ",";
        out << "\n";
    }

    out << "  ]\n";
    out << "}\n";

    return true;
}

bool MftReader::LoadSnapshot(const std::wstring& filepath,
                              std::vector<PathInfo>& results,
                              std::string& timestamp) {
    std::string filepath_utf8 = WideToUtf8(filepath);
    std::ifstream in(filepath_utf8, std::ios::binary);
    if (!in) return false;

    std::string content((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

    // Simple JSON parser (handles our specific format)
    results.clear();

    // Parse timestamp
    auto ts_pos = content.find("\"timestamp\"");
    if (ts_pos != std::string::npos) {
        auto colon = content.find(':', ts_pos);
        auto first_quote = content.find('\"', colon + 1);
        auto second_quote = content.find('\"', first_quote + 1);
        if (first_quote != std::string::npos && second_quote != std::string::npos) {
            timestamp = content.substr(first_quote + 1, second_quote - first_quote - 1);
        }
    }

    // Parse entries
    auto entries_pos = content.find("\"entries\"");
    if (entries_pos == std::string::npos) return false;

    auto bracket = content.find('[', entries_pos);
    if (bracket == std::string::npos) return false;

    size_t pos = bracket + 1;
    while (true) {
        auto next_brace = content.find('{', pos);
        if (next_brace == std::string::npos) break;

        auto close_brace = content.find('}', next_brace);
        if (close_brace == std::string::npos) break;

        std::string entry_str = content.substr(next_brace, close_brace - next_brace + 1);

        PathInfo pi = {};

        // Parse path
        auto path_pos = entry_str.find("\"path\"");
        if (path_pos != std::string::npos) {
            auto col = entry_str.find(':', path_pos);
            auto fq = entry_str.find('\"', col + 1);
            auto sq = entry_str.find('\"', fq + 1);
            if (fq != std::string::npos && sq != std::string::npos) {
                std::string path_utf8 = entry_str.substr(fq + 1, sq - fq - 1);
                pi.path = Utf8ToWide(path_utf8);
            }
        }

        // Parse size
        auto size_pos = entry_str.find("\"size\"");
        if (size_pos != std::string::npos) {
            auto col = entry_str.find(':', size_pos);
            auto end = entry_str.find_first_of(",\n}", col);
            if (end != std::string::npos) {
                pi.size = std::stoull(entry_str.substr(col + 1, end - col - 1));
            }
        }

        // Parse modification_time
        auto mtime_pos = entry_str.find("\"modification_time\"");
        if (mtime_pos != std::string::npos) {
            auto col = entry_str.find(':', mtime_pos);
            auto end = entry_str.find_first_of(",\n}", col);
            if (end != std::string::npos) {
                pi.modification_time = std::stoull(
                    entry_str.substr(col + 1, end - col - 1));
            }
        }

        // Parse is_directory
        pi.is_directory = (entry_str.find("\"is_directory\": true") != std::string::npos);

        if (!pi.path.empty()) {
            results.push_back(pi);
        }

        pos = close_brace + 1;
    }

    return !results.empty();
}
