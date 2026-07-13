// MFT Dump Tool - 读取并显示 MFT 记录 0 的原始结构
// 编译: g++ -std=c++17 -O2 tools/mft_dump.cpp -o mft_dump.exe -lws2_32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <iostream>

#pragma pack(push, 1)
struct FILE_RECORD_HEADER {
    char     magic[4];
    uint16_t usa_offset;
    uint16_t usa_size;
    uint64_t log_seq_number;
    uint16_t seq_number;
    uint16_t hardlink_count;
    uint16_t attr_offset;
    uint16_t flags;
    uint32_t real_size;
    uint32_t allocated_size;
    uint64_t base_record_ref;
    uint16_t next_attr_id;
    uint16_t padding;
    uint32_t record_number;
};

struct ATTR_HEADER {
    uint32_t type;
    uint32_t length;
    uint8_t  non_resident;
    uint8_t  name_length;
    uint16_t name_offset;
    uint16_t flags;
    uint16_t instance;
};
#pragma pack(pop)

static void hex_dump(const uint8_t* data, int len, const char* label) {
    printf("%s (%d bytes):\n", label, len);
    for (int i = 0; i < len; i++) {
        if (i % 16 == 0) printf("  %04X: ", i);
        printf("%02X ", data[i]);
        if (i % 16 == 15) printf("\n");
    }
    if (len % 16 != 0) printf("\n");
}

int main() {
    printf("========================================\n");
    printf("  MFT Dump Tool\n");
    printf("========================================\n\n");

    // Open volume
    HANDLE vol = CreateFileW(L"\\\\.\\C:", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (vol == INVALID_HANDLE_VALUE) {
        printf("ERROR: Cannot open volume (run as admin)\n");
        return 1;
    }
    printf("[OK] Volume opened\n\n");

    // Get NTFS volume data
    NTFS_VOLUME_DATA_BUFFER nvdb = {};
    DWORD bytes_ret = 0;
    if (DeviceIoControl(vol, FSCTL_GET_NTFS_VOLUME_DATA,
                        nullptr, 0, &nvdb, sizeof(nvdb),
                        &bytes_ret, nullptr)) {
        printf("=== FSCTL_GET_NTFS_VOLUME_DATA ===\n");
        printf("MftStartLcn:          %lld\n", nvdb.MftStartLcn.QuadPart);
        printf("Mft2StartLcn:         %lld\n", nvdb.Mft2StartLcn.QuadPart);
        printf("MftZoneStart:         %lld\n", nvdb.MftZoneStart.QuadPart);
        printf("MftValidDataLength:   %lld\n", nvdb.MftValidDataLength.QuadPart);
        printf("BytesPerRecord:       %u\n", nvdb.BytesPerFileRecordSegment);
        printf("ClustersPerRecord:    %u\n", nvdb.ClustersPerFileRecordSegment);
        printf("\n");
    } else {
        printf("FSCTL failed, error=%lu\n", GetLastError());
    }

    // Get volume info for cluster size
    DWORD sectorsPerCluster, bytesPerSector, freeClusters, totalClusters;
    GetDiskFreeSpaceW(L"C:\\", &sectorsPerCluster, &bytesPerSector,
                      &freeClusters, &totalClusters);
    DWORD bytesPerCluster = sectorsPerCluster * bytesPerSector;
    printf("=== Volume Info ===\n");
    printf("Bytes per cluster:    %u\n", bytesPerCluster);
    printf("Total clusters:       %u\n", totalClusters);
    printf("Volume size:          %llu GB\n",
           (uint64_t)totalClusters * bytesPerCluster / 1024 / 1024 / 1024);
    printf("\n");

    // Read boot sector
    uint8_t boot[512];
    LARGE_INTEGER li = {};
    SetFilePointerEx(vol, li, nullptr, FILE_BEGIN);
    DWORD br;
    ReadFile(vol, boot, 512, &br, nullptr);
    
    // Validate OEM
    printf("=== Boot Sector ===\n");
    printf("OEM ID: \"%.8s\"\n", &boot[0x03]);
    
    // Read MFT parameters from boot sector
    uint64_t mft_lcn_boot = *(uint64_t*)&boot[0x28];
    int8_t mft_rec_clusters = (int8_t)boot[0x38];
    uint64_t mft_rec_size;
    if (mft_rec_clusters > 0)
        mft_rec_size = mft_rec_clusters * bytesPerCluster;
    else
        mft_rec_size = (uint64_t)1 << (-mft_rec_clusters);
    
    printf("MFT start LCN:        %llu (0x%llX)\n", mft_lcn_boot, mft_lcn_boot);
    printf("MFT rec size (boot):  %llu bytes\n", mft_rec_size);
    printf("Raw byte[0x38]:       0x%02X (%d)\n", boot[0x38], (int8_t)boot[0x38]);
    printf("\n");

    // Use FSCTL values if available
    uint64_t actual_mft_lcn = nvdb.MftStartLcn.QuadPart;
    uint64_t actual_rec_size = nvdb.BytesPerFileRecordSegment;
    uint64_t actual_mft_size = nvdb.MftValidDataLength.QuadPart;

    if (actual_rec_size == 0) {
        actual_rec_size = mft_rec_size;
        actual_mft_lcn = mft_lcn_boot;
        printf("[WARN] Using boot sector values (FSCTL failed)\n");
    }

    printf("=== Using ===\n");
    printf("MFT LCN:              %llu\n", actual_mft_lcn);
    printf("MFT byte offset:      %llu\n", actual_mft_lcn * bytesPerCluster);
    printf("MFT record size:      %llu bytes\n", actual_rec_size);
    printf("MFT valid data:       %llu bytes (%llu records)\n",
           actual_mft_size, actual_mft_size / actual_rec_size);
    printf("\n");

    // Read MFT record 0
    uint64_t mft_offset = actual_mft_lcn * bytesPerCluster;
    std::vector<uint8_t> rec(actual_rec_size);
    
    li.QuadPart = mft_offset;
    SetFilePointerEx(vol, li, nullptr, FILE_BEGIN);
    ReadFile(vol, rec.data(), (DWORD)actual_rec_size, &br, nullptr);
    
    // Dump record header
    FILE_RECORD_HEADER* hdr = (FILE_RECORD_HEADER*)rec.data();
    printf("=== MFT Record 0 Header ===\n");
    printf("Magic:              %.4s\n", hdr->magic);
    printf("USA offset:         %u\n", hdr->usa_offset);
    printf("USA size:           %u\n", hdr->usa_size);
    printf("Seq number:         %u\n", hdr->seq_number);
    printf("Hardlink count:     %u\n", hdr->hardlink_count);
    printf("Attr offset:        %u\n", hdr->attr_offset);
    printf("Flags:              0x%04X (%s)\n", hdr->flags,
           (hdr->flags & 1) ? "inuse" : "free");
    printf("Real size:          %u\n", hdr->real_size);
    printf("Allocated size:     %u\n", hdr->allocated_size);
    printf("Record number:      %u\n", hdr->record_number);
    printf("\n");

    // Apply USA fixup
    printf("=== USA Fixup ===\n");
    uint16_t* usa = (uint16_t*)(rec.data() + hdr->usa_offset);
    uint16_t usa_magic = usa[0];
    printf("USA magic:          0x%04X\n", usa_magic);
    for (int i = 1; i < hdr->usa_size && i <= 16; i++) {
        uint16_t* sector_end = (uint16_t*)(rec.data() + i * 512 - 2);
        printf("  Sector %d: last word=0x%04X -> fixup=0x%04X %s\n",
               i-1, *sector_end, usa[i],
               (*sector_end == usa_magic) ? "✓" : "✗ MISMATCH");
        if (*sector_end == usa_magic) {
            *sector_end = usa[i];
        }
    }
    printf("\n");

    // Dump first 128 bytes of record
    hex_dump(rec.data(), 128, "Record 0 first 128 bytes");

    // Walk attributes
    printf("\n=== Attributes ===\n");
    uint16_t attr_off = hdr->attr_offset;
    int attr_idx = 0;
    while (attr_off + sizeof(ATTR_HEADER) <= hdr->allocated_size && attr_off < 512) {
        ATTR_HEADER* attr = (ATTR_HEADER*)(rec.data() + attr_off);
        if (attr->type == 0xFFFFFFFF || attr->length == 0) break;
        
        const char* type_name = "UNKNOWN";
        switch (attr->type) {
            case 0x10: type_name = "$STANDARD_INFO"; break;
            case 0x20: type_name = "$ATTRIBUTE_LIST"; break;
            case 0x30: type_name = "$FILE_NAME"; break;
            case 0x40: type_name = "$OBJECT_ID"; break;
            case 0x50: type_name = "$SECURITY"; break;
            case 0x60: type_name = "$VOLUME_NAME"; break;
            case 0x70: type_name = "$VOLUME_INFO"; break;
            case 0x80: type_name = "$DATA"; break;
            case 0x90: type_name = "$INDEX_ROOT"; break;
            case 0xA0: type_name = "$INDEX_ALLOCATION"; break;
            case 0xB0: type_name = "$BITMAP"; break;
            case 0xC0: type_name = "$REPARSE"; break;
        }
        
        printf("  [%d] %s (0x%02X) offset=%u len=%u resident=%s\n",
               attr_idx++, type_name, attr->type, attr_off, attr->length,
               attr->non_resident ? "no" : "yes");
        
        // Dump $FILE_NAME value data
        if (attr->type == 0x30 && !attr->non_resident) {
            uint8_t* res_start = rec.data() + attr_off + sizeof(ATTR_HEADER);
            uint32_t val_len = *(uint32_t*)res_start;
            uint16_t val_off = *(uint16_t*)(res_start + 4);
            uint8_t* fn_data = rec.data() + attr_off + sizeof(ATTR_HEADER) + val_off;
            
            printf("         value_len=%u value_off=%u\n", val_len, val_off);
            
            // Parse FILE_NAME
            uint64_t parent = *(uint64_t*)fn_data;
            uint64_t crtime = *(uint64_t*)(fn_data + 8);
            uint64_t mtime = *(uint64_t*)(fn_data + 24);
            uint64_t realsize = *(uint64_t*)(fn_data + 48);
            uint8_t  name_len = *(uint8_t*)(fn_data + 64);
            uint8_t  name_type = *(uint8_t*)(fn_data + 65);
            uint64_t parent_rec = parent & 0xFFFFFFFFFFFFULL;
            uint16_t parent_seq = (parent >> 48);
            
            printf("         parent_ref=0x%llX (rec=%llu seq=%u)\n",
                   parent, parent_rec, parent_seq);
            printf("         name_len=%u name_type=%u (0=POSIX,1=Win32,2=DOS,3=Win32+DOS)\n",
                   name_len, name_type);
            
            if (name_len > 0 && name_len < 256) {
                wchar_t* wname = (wchar_t*)(fn_data + 66);
                printf("         name=\"");
                for (int i = 0; i < name_len; i++) {
                    if (wname[i] >= 32 && wname[i] < 128)
                        putchar((char)wname[i]);
                    else
                        printf("\\u%04X", wname[i]);
                }
                printf("\"\n");
            }
            
            // Hex dump FILE_NAME data
            int dump_len = val_len;
            if (dump_len > 80) dump_len = 80;
            hex_dump(fn_data, dump_len, "         FILE_NAME data");
        }
        
        attr_off += attr->length;
    }

    printf("\n=== Done ===\n");
    printf("Press Enter to exit...\n");
    getchar();
    CloseHandle(vol);
    return 0;
}
