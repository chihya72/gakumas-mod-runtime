#define NOMINMAX

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct SectionInfo {
    std::string name;
    DWORD virtualAddress{};
    DWORD virtualSize{};
    DWORD rawSize{};
    DWORD rawOffset{};
    DWORD characteristics{};
};

struct PeImage {
    std::vector<std::uint8_t> bytes;
    DWORD fileAlignment{};
    DWORD sizeOfHeaders{};
    DWORD sizeOfImage{};
    std::size_t sectionTableOffset{};
    std::vector<SectionInfo> sections;
};

struct AddressRegion {
    std::uintptr_t base{};
    std::uintptr_t end{};
    std::size_t size{};
    DWORD protect{};
};

struct HeapRegion {
    AddressRegion address;
    std::vector<std::uint8_t> bytes;
    DWORD syntheticRva{};
};

static DWORD AlignUp(DWORD value, DWORD alignment) {
    if (alignment == 0) return value;
    const DWORD remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

static bool SafeCopy(const void* source, void* destination, std::size_t size) {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool ReadFileBytes(const fs::path& path, std::vector<std::uint8_t>& bytes) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) return false;
    const std::streamsize size = input.tellg();
    if (size <= 0) return false;
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    return input.read(reinterpret_cast<char*>(bytes.data()), size).good();
}

static bool ParsePe(const fs::path& path, PeImage& image) {
    if (!ReadFileBytes(path, image.bytes) || image.bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.bytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    image.fileAlignment = nt->OptionalHeader.FileAlignment;
    image.sizeOfHeaders = nt->OptionalHeader.SizeOfHeaders;
    image.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    image.sectionTableOffset = static_cast<std::size_t>(dos->e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt->FileHeader.SizeOfOptionalHeader;
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(image.bytes.data() + image.sectionTableOffset);
    image.sections.clear();
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        char name[9]{};
        std::memcpy(name, sections[index].Name, 8);
        image.sections.push_back(SectionInfo{name, sections[index].VirtualAddress, sections[index].Misc.VirtualSize,
                                             sections[index].SizeOfRawData, sections[index].PointerToRawData,
                                             sections[index].Characteristics});
    }
    return true;
}

static bool IsReadable(DWORD protect) {
    const DWORD value = protect & 0xFF;
    return value == PAGE_READONLY || value == PAGE_READWRITE || value == PAGE_WRITECOPY ||
           value == PAGE_EXECUTE_READ || value == PAGE_EXECUTE_READWRITE || value == PAGE_EXECUTE_WRITECOPY;
}

static bool ReadableAt(std::uintptr_t address, std::size_t size) {
    MEMORY_BASIC_INFORMATION memory{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == 0 || memory.State != MEM_COMMIT ||
        !IsReadable(memory.Protect)) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
    return address >= base && address + size <= base + memory.RegionSize;
}

static void EnumeratePrivateReadableRegions(std::vector<AddressRegion>& regions) {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    auto address = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == 0) break;
        const auto base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto next = base + memory.RegionSize;
        if (next <= address) break;
        if (memory.State == MEM_COMMIT && memory.Type == MEM_PRIVATE && IsReadable(memory.Protect) &&
            (memory.Protect & PAGE_GUARD) == 0 &&
            (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) == 0 &&
            memory.RegionSize != 0) {
            regions.push_back(AddressRegion{base, next, memory.RegionSize, memory.Protect});
        }
        address = next;
    }
}

static bool IsInModule(std::uintptr_t address, std::uintptr_t moduleBase, DWORD imageSize) {
    return address >= moduleBase && address < moduleBase + imageSize;
}

static bool AddHeapRegionForPointer(std::uintptr_t pointer, std::uintptr_t moduleBase, DWORD imageSize,
                                    const std::vector<AddressRegion>& readableRegions,
                                    std::vector<HeapRegion>& regions, std::size_t& totalBytes) {
    // Only probe canonical user-mode addresses.  Besides avoiding bogus values
    // embedded in metadata/string data, this keeps the scan from issuing a
    // VirtualQuery call for every arbitrary 64-bit word in the image.
    if (pointer < 0x10000 || (pointer >> 47) != 0 || IsInModule(pointer, moduleBase, imageSize)) return false;
    const auto match = std::upper_bound(readableRegions.begin(), readableRegions.end(), pointer,
        [](std::uintptr_t value, const AddressRegion& region) { return value < region.base; });
    if (match == readableRegions.begin() || pointer >= (match - 1)->end) return false;
    const auto& address = *(match - 1);
    constexpr std::size_t maxRegionBytes = 64ull * 1024ull * 1024ull;
    constexpr std::size_t maxTotalBytes = 1024ull * 1024ull * 1024ull;
    if (address.size > maxRegionBytes || totalBytes + address.size > maxTotalBytes) return false;
    for (const auto& existing : regions) {
        if (existing.address.base == address.base) return false;
    }
    HeapRegion heap{address, std::vector<std::uint8_t>(address.size), 0};
    if (!SafeCopy(reinterpret_cast<const void*>(address.base), heap.bytes.data(), address.size)) return false;
    totalBytes += address.size;
    regions.push_back(std::move(heap));
    return true;
}

static std::size_t ScanForHeapRegions(const std::vector<std::uint8_t>& bytes, std::uintptr_t moduleBase,
                                      DWORD imageSize, const std::vector<AddressRegion>& readableRegions,
                                      std::vector<HeapRegion>& regions, std::size_t& totalBytes) {
    const auto before = regions.size();
    for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= bytes.size(); offset += sizeof(std::uintptr_t)) {
        std::uintptr_t pointer = 0;
        std::memcpy(&pointer, bytes.data() + offset, sizeof(pointer));
        AddHeapRegionForPointer(pointer, moduleBase, imageSize, readableRegions, regions, totalBytes);
    }
    return regions.size() - before;
}

static bool BuildHeapSnapshot(const PeImage& image, std::uintptr_t base,
                              const std::vector<std::uint8_t>& moduleImage,
                              std::vector<HeapRegion>& regions, std::string& error) {
    std::size_t totalBytes = 0;
    std::vector<AddressRegion> readableRegions;
    EnumeratePrivateReadableRegions(readableRegions);
    // Start with pointers in the reconstructed module. Newly captured heap
    // regions are scanned recursively because Il2CppType objects point to
    // Il2CppClass objects and those objects contain further pointers.
    // Metadata registrations and type tables live in non-executable sections;
    // skipping code sections makes this bounded scan practical for a nearly
    // 200 MB GameAssembly image.
    const auto* moduleSections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(moduleImage.data() + image.sectionTableOffset);
    for (std::size_t index = 0; index < image.sections.size(); ++index) {
        const auto& section = image.sections[index];
        if ((section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
            moduleSections[index].PointerToRawData >= moduleImage.size()) {
            continue;
        }
        const auto available = moduleImage.size() - static_cast<std::size_t>(moduleSections[index].PointerToRawData);
        const auto size = std::min<std::size_t>(section.virtualSize, available);
        if (size == 0) continue;
        const auto sectionOffset = static_cast<std::size_t>(moduleSections[index].PointerToRawData);
        std::vector<std::uint8_t> sectionBytes(moduleImage.begin() + sectionOffset,
                                               moduleImage.begin() + sectionOffset + size);
        ScanForHeapRegions(sectionBytes, base, image.sizeOfImage, readableRegions, regions, totalBytes);
    }
    for (std::size_t index = 0; index < regions.size(); ++index) {
        ScanForHeapRegions(regions[index].bytes, base, image.sizeOfImage, readableRegions, regions, totalBytes);
        if (regions.size() > 8192) {
            error = "too many external memory regions";
            return false;
        }
    }
    return true;
}

static std::uintptr_t TranslatePointer(std::uintptr_t pointer, std::uintptr_t moduleBase, DWORD imageSize,
                                       const std::vector<HeapRegion>& regions, DWORD heapRva) {
    if (pointer < 0x10000 || IsInModule(pointer, moduleBase, imageSize)) return pointer;
    const auto match = std::upper_bound(regions.begin(), regions.end(), pointer,
        [](std::uintptr_t value, const HeapRegion& region) { return value < region.address.base; });
    if (match != regions.begin()) {
        const auto& region = *(match - 1);
        if (pointer >= region.address.base && pointer < region.address.end)
            return moduleBase + heapRva + region.syntheticRva + (pointer - region.address.base);
    }
    return pointer;
}

static void RewritePointers(std::vector<std::uint8_t>& bytes, std::uintptr_t moduleBase, DWORD imageSize,
                            const std::vector<HeapRegion>& regions, DWORD heapRva) {
    for (std::size_t offset = 0; offset + sizeof(std::uintptr_t) <= bytes.size(); offset += sizeof(std::uintptr_t)) {
        std::uintptr_t pointer = 0;
        std::memcpy(&pointer, bytes.data() + offset, sizeof(pointer));
        const auto translated = TranslatePointer(pointer, moduleBase, imageSize, regions, heapRva);
        if (translated != pointer) std::memcpy(bytes.data() + offset, &translated, sizeof(translated));
    }
}

static bool FindBySectionLayout(const PeImage& image, std::uintptr_t& base) {
    const auto executable = std::find_if(image.sections.begin(), image.sections.end(), [](const SectionInfo& section) {
        return (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 && section.virtualSize >= 0x1000000;
    });
    if (executable == image.sections.end()) return false;
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == 0) break;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto next = address + std::max<std::size_t>(memory.RegionSize, 0x1000);
        const bool executablePage = memory.State == MEM_COMMIT && IsReadable(memory.Protect) &&
            (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (executablePage && memory.RegionSize >= executable->virtualSize / 2 && memory.RegionSize <= executable->virtualSize * 2 &&
            regionBase >= executable->virtualAddress) {
            const auto candidate = regionBase - executable->virtualAddress;
            int matched = 0;
            for (const auto& section : image.sections) {
                const auto sectionAddress = candidate + section.virtualAddress;
                if (ReadableAt(sectionAddress, std::min<DWORD>(section.virtualSize, 0x1000))) ++matched;
            }
            if (matched >= 3) {
                base = candidate;
                return true;
            }
        }
        if (next <= address) break;
        address = next;
    }
    return false;
}

static std::uint64_t HashSection(std::uintptr_t base, const SectionInfo& section, bool& readable) {
    const std::size_t sampleSize = 0x1000;
    const DWORD offset = std::min<DWORD>(section.virtualSize > sampleSize ? section.virtualSize - static_cast<DWORD>(sampleSize) : 0,
                                         std::max<DWORD>(0x1000, section.rawSize));
    const auto address = base + section.virtualAddress + offset;
    std::vector<std::uint8_t> sample(sampleSize);
    readable = ReadableAt(address, sample.size()) && SafeCopy(reinterpret_cast<const void*>(address), sample.data(), sample.size());
    if (!readable) return 0;
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto byte : sample) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool Capture(const PeImage& image, std::uintptr_t base, const fs::path& output, const fs::path& mapPath, std::string& error) {
    std::vector<std::uint8_t> result(image.sizeOfHeaders, 0);
    if (result.size() > image.bytes.size()) {
        error = "invalid source header size";
        return false;
    }
    std::copy_n(image.bytes.begin(), result.size(), result.begin());
    auto* targetDos = reinterpret_cast<IMAGE_DOS_HEADER*>(result.data());
    auto* targetNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(result.data() + targetDos->e_lfanew);
    targetNt->OptionalHeader.ImageBase = base;
    DWORD nextRaw = AlignUp(image.sizeOfHeaders, image.fileAlignment);
    std::ostringstream map;
    map << "{\n  \"base\": \"0x" << std::hex << base << "\",\n  \"sections\": [\n";
    for (std::size_t index = 0; index < image.sections.size(); ++index) {
        const auto& section = image.sections[index];
        const DWORD virtualSize = std::max(section.virtualSize, section.rawSize);
        const DWORD rawSize = AlignUp(virtualSize, image.fileAlignment);
        // result.resize() may move the vector. Reacquire this pointer before
        // every header write instead of retaining a stale pointer from the
        // previous iteration.
        auto* targetSections = reinterpret_cast<IMAGE_SECTION_HEADER*>(result.data() + image.sectionTableOffset);
        targetSections[index].PointerToRawData = nextRaw;
        targetSections[index].SizeOfRawData = rawSize;
        if (nextRaw + rawSize > 0x7FFFFFFF) {
            error = "capture is too large";
            return false;
        }
        result.resize(nextRaw + rawSize, 0);
        bool readComplete = ReadableAt(base + section.virtualAddress, section.virtualSize) &&
            SafeCopy(reinterpret_cast<const void*>(base + section.virtualAddress), result.data() + nextRaw, section.virtualSize);
        if (!readComplete && section.rawOffset < image.bytes.size()) {
            const std::size_t available = std::min<std::size_t>(section.rawSize, image.bytes.size() - section.rawOffset);
            std::copy_n(image.bytes.begin() + section.rawOffset, available, result.data() + nextRaw);
        }
        char name[9]{};
        std::memcpy(name, section.name.data(), std::min<std::size_t>(8, section.name.size()));
        map << "    {\"name\": \"" << name << "\", \"va\": \"0x" << std::hex << section.virtualAddress
            << "\", \"size\": " << std::dec << section.virtualSize << ", \"readComplete\": " << (readComplete ? "true" : "false") << "}";
        if (index + 1 < image.sections.size()) map << ',';
        map << '\n';
        nextRaw += rawSize;
    }

    // Il2CppType and Il2CppClass instances are allocated by the running
    // process, not stored in GameAssembly's image.  Preserve the readable
    // regions reached from the metadata sections in a synthetic PE section,
    // then translate every captured pointer to the corresponding RVA.
    std::vector<HeapRegion> heapRegions;
    if (!BuildHeapSnapshot(image, base, result, heapRegions, error)) return false;
    std::sort(heapRegions.begin(), heapRegions.end(), [](const HeapRegion& left, const HeapRegion& right) {
        return left.address.base < right.address.base;
    });
    DWORD heapRva = 0;
    DWORD heapVirtualSize = 0;
    DWORD heapRawOffset = 0;
    DWORD heapRawSize = 0;
    if (!heapRegions.empty()) {
        const auto* currentDos = reinterpret_cast<const IMAGE_DOS_HEADER*>(result.data());
        const auto* currentNt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(result.data() + currentDos->e_lfanew);
        const DWORD sectionAlignment = std::max<DWORD>(currentNt->OptionalHeader.SectionAlignment, 0x1000);
        heapRva = AlignUp(image.sizeOfImage, sectionAlignment);
        for (auto& region : heapRegions) {
            const auto alignedOffset = AlignUp(heapVirtualSize, sectionAlignment);
            if (alignedOffset < heapVirtualSize || region.bytes.size() > 0xFFFFFFFFu - alignedOffset) {
                error = "heap snapshot is too large";
                return false;
            }
            region.syntheticRva = alignedOffset;
            heapVirtualSize = alignedOffset + static_cast<DWORD>(region.bytes.size());
        }
        heapRawOffset = AlignUp(nextRaw, image.fileAlignment);
        heapRawSize = AlignUp(heapVirtualSize, image.fileAlignment);
        if (heapRva > 0xFFFFFFFFu - heapVirtualSize || heapRawOffset > 0xFFFFFFFFu - heapRawSize) {
            error = "heap snapshot exceeds PE address space";
            return false;
        }
        result.resize(static_cast<std::size_t>(heapRawOffset) + heapRawSize, 0);

        RewritePointers(result, base, image.sizeOfImage, heapRegions, heapRva);
        for (auto& region : heapRegions) {
            RewritePointers(region.bytes, base, image.sizeOfImage, heapRegions, heapRva);
            const auto rawOffset = static_cast<std::size_t>(heapRawOffset) + region.syntheticRva;
            std::copy(region.bytes.begin(), region.bytes.end(), result.begin() + rawOffset);
        }

        auto* finalDos = reinterpret_cast<IMAGE_DOS_HEADER*>(result.data());
        auto* finalNt = reinterpret_cast<IMAGE_NT_HEADERS64*>(result.data() + finalDos->e_lfanew);
        const WORD oldSectionCount = finalNt->FileHeader.NumberOfSections;
        const auto sectionHeaderOffset = image.sectionTableOffset +
            static_cast<std::size_t>(oldSectionCount) * sizeof(IMAGE_SECTION_HEADER);
        if (sectionHeaderOffset + sizeof(IMAGE_SECTION_HEADER) > finalNt->OptionalHeader.SizeOfHeaders) {
            error = "no room for heap section header";
            return false;
        }
        auto* finalSections = reinterpret_cast<IMAGE_SECTION_HEADER*>(result.data() + image.sectionTableOffset);
        auto& heapSection = finalSections[oldSectionCount];
        std::memset(&heapSection, 0, sizeof(heapSection));
        std::memcpy(heapSection.Name, ".heap", 5);
        heapSection.Misc.VirtualSize = heapVirtualSize;
        heapSection.VirtualAddress = heapRva;
        heapSection.SizeOfRawData = heapRawSize;
        heapSection.PointerToRawData = heapRawOffset;
        heapSection.Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
        finalNt->FileHeader.NumberOfSections = oldSectionCount + 1;
        finalNt->OptionalHeader.SizeOfImage = AlignUp(heapRva + heapVirtualSize, sectionAlignment);
    }

    map << "  ],\n  \"heapRegions\": " << heapRegions.size()
        << ",\n  \"heapBytes\": " << std::dec << (static_cast<std::uint64_t>(heapVirtualSize)) << "\n}\n";
    std::ofstream outputFile(output, std::ios::binary | std::ios::trunc);
    if (!outputFile.write(reinterpret_cast<const char*>(result.data()), static_cast<std::streamsize>(result.size()))) {
        error = "cannot write capture";
        return false;
    }
    std::ofstream mapFile(mapPath, std::ios::binary | std::ios::trunc);
    const auto mapText = map.str();
    if (!mapFile.write(mapText.data(), static_cast<std::streamsize>(mapText.size()))) {
        error = "cannot write map";
        return false;
    }
    return true;
}

static bool CaptureThread() {
    wchar_t executablePath[MAX_PATH * 4]{};
    const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
    if (pathLength == 0) return false;
    const fs::path gameRoot = fs::path(executablePath).parent_path();
    const fs::path source = gameRoot / L"GameAssembly.dll";
    const fs::path outputDirectory = gameRoot / L"BepInEx" / L"gakumas-runtime-capture";
    std::error_code directoryError;
    fs::create_directories(outputDirectory, directoryError);
    const fs::path logPath = outputDirectory / L"inprocess-capture.log";
    std::ofstream log(logPath, std::ios::trunc);
    auto logLine = [&](const std::string& line) {
        log << line << '\n';
        log.flush();
    };
    PeImage image{};
    if (!ParsePe(source, image)) {
        logLine("source GameAssembly.dll parse failed");
        return false;
    }
    logLine("in-process capture started");
    std::uintptr_t base = 0;
    for (int attempt = 0; attempt < 180 && base == 0; ++attempt) {
        HMODULE module = GetModuleHandleW(L"GameAssembly.dll");
        if (module != nullptr) {
            base = reinterpret_cast<std::uintptr_t>(module);
        } else {
            FindBySectionLayout(image, base);
        }
        if (base == 0) Sleep(1000);
    }
    if (base == 0) {
        logLine("runtime image not found");
        return false;
    }
    std::ostringstream found;
    found << "candidate base=0x" << std::hex << base;
    logLine(found.str());
    std::uint64_t previous = 0;
    int stable = 0;
    for (int attempt = 0; attempt < 120; ++attempt) {
        std::uint64_t hash = 14695981039346656037ull;
        int readableSections = 0;
        for (const auto& section : image.sections) {
            if ((section.characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) continue;
            bool readable = false;
            const auto sectionHash = HashSection(base, section, readable);
            if (readable) {
                ++readableSections;
                hash ^= sectionHash;
                hash *= 1099511628211ull;
            }
        }
        stable = readableSections >= 2 && hash == previous ? stable + 1 : 0;
        std::ostringstream status;
        status << "probe readableSections=" << readableSections << " hash=0x" << std::hex << hash << " stable=" << std::dec << stable;
        logLine(status.str());
        if (stable >= 3) {
            std::string error;
            const fs::path output = outputDirectory / L"GameAssembly.inprocess.dll";
            const fs::path map = outputDirectory / L"GameAssembly.inprocess.map.json";
            if (Capture(image, base, output, map, error)) {
                logLine("capture complete");
            } else {
                logLine("capture failed: " + error);
            }
                return true;
        }
        previous = hash;
        Sleep(1000);
    }
    logLine("capture timeout");
    return false;
}

static volatile LONG captureStarted = 0;

extern "C" __declspec(dllexport) void GakumasCaptureStart() {
    if (InterlockedExchange(&captureStarted, 1) != 0) return;
    HANDLE thread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
        CaptureThread();
        return 0;
    }, nullptr, 0, nullptr);
    if (thread != nullptr) {
        CloseHandle(thread);
    }
}

extern "C" __declspec(dllexport) int GakumasCaptureRun() {
    return CaptureThread() ? 1 : 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
