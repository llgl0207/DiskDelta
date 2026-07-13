#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <map>
#include <memory>

#pragma pack(push, 1)

// MFT Record Header ("FILE")
struct FILE_RECORD_HEADER {
    char     magic[4];          // "FILE"
    uint16_t usa_offset;        // Offset to Update Sequence Array
    uint16_t usa_size;          // Size of Update Sequence Array
    uint64_t log_seq_number;    // $LogFile sequence number
    uint16_t seq_number;        // Sequence number
    uint16_t hardlink_count;    // Hard link count
    uint16_t attr_offset;       // Offset to first attribute
    uint16_t flags;             // Flags: 0x01=inuse, 0x02=directory
    uint32_t real_size;         // Real size of the record
    uint32_t allocated_size;    // Allocated size of the record
    uint64_t base_record_ref;   // Base record reference
    uint16_t next_attr_id;      // Next attribute ID
    uint16_t padding;           // Align to 4 bytes
    uint32_t record_number;     // MFT record number
};

// Attribute Header
struct ATTR_HEADER {
    uint32_t type;              // Attribute type
    uint32_t length;            // Total length including header
    uint8_t  non_resident;      // 0=resident, 1=non-resident
    uint8_t  name_length;       // Name length in characters
    uint16_t name_offset;       // Offset to name
    uint16_t flags;             // Flags
    uint16_t instance;          // Instance
    // Union follows:
    // Resident: value_length(4), value_offset(2), ...
    // Non-resident: low_vcn(8), high_vcn(8), ...
};

struct ATTR_HEADER_RESIDENT {
    uint32_t value_length;
    uint16_t value_offset;
    uint8_t  flags;             // Resident flags
    uint8_t  reserved;
};

struct ATTR_HEADER_NONRESIDENT {
    uint64_t low_vcn;
    uint64_t high_vcn;
    uint16_t run_array_offset;  // Offset to run array
    uint8_t  compression_unit;
    uint8_t  reserved[5];
    uint64_t allocated_size;
    uint64_t real_size;
    uint64_t initial_size;
};

// Attribute types
const uint32_t ATTR_STANDARD_INFORMATION  = 0x10;
const uint32_t ATTR_ATTRIBUTE_LIST        = 0x20;
const uint32_t ATTR_FILE_NAME             = 0x30;
const uint32_t ATTR_OBJECT_ID             = 0x40;
const uint32_t ATTR_SECURITY_DESCRIPTOR   = 0x50;
const uint32_t ATTR_VOLUME_NAME           = 0x60;
const uint32_t ATTR_VOLUME_INFORMATION    = 0x70;
const uint32_t ATTR_DATA                  = 0x80;
const uint32_t ATTR_INDEX_ROOT            = 0x90;
const uint32_t ATTR_INDEX_ALLOCATION      = 0xA0;
const uint32_t ATTR_BITMAP                = 0xB0;
const uint32_t ATTR_REPARSE_POINT         = 0xC0;
const uint32_t ATTR_EA_INFORMATION        = 0xD0;
const uint32_t ATTR_EA                    = 0xE0;
const uint32_t ATTR_LOGGED_UTILITY_STREAM = 0x100;

// $FILE_NAME attribute (resident)
struct FILE_NAME_ATTR {
    uint64_t parent_ref;        // Parent directory reference
    uint64_t creation_time;
    uint64_t modification_time;
    uint64_t mft_change_time;
    uint64_t access_time;
    uint64_t allocated_size;    // Allocated size
    uint64_t real_size;         // Real file size (end of file)
    uint32_t flags;             // File flags
    uint32_t reparse_value;
    uint8_t  name_length;       // Name length in characters
    uint8_t  name_type;         // 0x01=long name, 0x02=short name
    wchar_t  name[1];           // Variable length name (UTF-16LE)
};

// Data Run
struct DataRun {
    int64_t  vcn;               // Virtual cluster number
    int64_t  lcn;               // Logical cluster number (-1 = sparse)
    uint64_t length;            // Number of clusters
};

#pragma pack(pop)

// MFT entry information
struct MftEntry {
    uint64_t    record_number;
    uint64_t    parent_ref;
    uint16_t    parent_seq;
    uint64_t    parent_record;
    std::wstring filename;
    std::wstring parent_path;
    std::wstring full_path;
    uint64_t    real_size;
    uint64_t    allocated_size;
    uint64_t    modification_time;
    uint64_t    creation_time;
    bool        is_directory;
    bool        valid;
};

// Final result per path
struct PathInfo {
    std::wstring path;
    uint64_t     size;
    uint64_t     modification_time;
    bool         is_directory;
};

// MFT Reader class
class MftReader {
public:
    MftReader();
    ~MftReader();

    // Open volume and read MFT
    bool OpenVolume(const std::wstring& drive_letter);
    void CloseVolume();

    // Scan all MFT entries
    bool ScanMft(std::vector<MftEntry>& entries);

    // Build full path tree and calculate recursive sizes
    static bool BuildPathTree(
        const std::vector<MftEntry>& entries,
        std::map<uint64_t, PathInfo>& path_map,
        std::vector<PathInfo>& results
    );

    // Save snapshot to JSON
    static bool SaveSnapshot(const std::vector<PathInfo>& results,
                             const std::wstring& filepath);

    // Load snapshot from JSON
    static bool LoadSnapshot(const std::wstring& filepath,
                             std::vector<PathInfo>& results,
                             std::string& timestamp);

private:
    HANDLE      m_volume_handle;
    HANDLE      m_mft_handle;
    uint64_t    m_mft_size;
    uint64_t    m_bytes_per_cluster;
    uint64_t    m_bytes_per_mft_record;
    uint64_t    m_mft_start_lcn;
    std::wstring m_drive_letter;

    // Read a single MFT record
    bool ReadMftRecord(uint64_t record_number, std::vector<uint8_t>& buffer);

    // Fixup USA (Update Sequence Array)
    bool ApplyFixup(std::vector<uint8_t>& buffer, uint16_t usa_offset, uint16_t usa_size);

    // Parse attributes from a record
    bool ParseAttributes(const std::vector<uint8_t>& buffer,
                         MftEntry& entry);

    // Parse $FILE_NAME attribute
    bool ParseFileName(const uint8_t* attr_start, uint32_t attr_length,
                       MftEntry& entry, bool is_resident);

    // Parse $DATA attribute for size
    bool ParseDataSize(const uint8_t* attr_start, uint32_t attr_length,
                       bool non_resident, uint64_t& real_size,
                       uint64_t& allocated_size);

    // Get volume cluster size
    bool GetVolumeInfo();

    // Locate $MFT on disk
    bool LocateMft();
};

// JSON helper
std::string WideToUtf8(const std::wstring& wstr);
std::wstring Utf8ToWide(const std::string& str);
std::string MftTimeToString(uint64_t mft_time);
uint64_t FileTimeToUnixSeconds(uint64_t filetime);
