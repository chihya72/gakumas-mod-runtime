#define NOMINMAX

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;

struct SectionInfo {
    std::string name;
    DWORD virtualAddress{};
    DWORD virtualSize{};
    DWORD originalRawSize{};
    DWORD originalRawOffset{};
    DWORD characteristics{};
};

struct PeImage {
    std::vector<std::uint8_t> bytes;
    DWORD fileAlignment{};
    DWORD sectionAlignment{};
    DWORD sizeOfHeaders{};
    DWORD sizeOfImage{};
    ULONGLONG preferredImageBase{};
    std::size_t sectionTableOffset{};
    std::vector<SectionInfo> sections;
};

struct ModuleInfo {
    DWORD pid{};
    HANDLE process{};
    std::uintptr_t base{};
    DWORD size{};
    std::wstring path;
};

struct Sample {
    bool materialized{};
    std::uint64_t hash{};
    std::size_t nonZeroBytes{};
    int readFailures{};
    DWORD lastReadError{};
};

static std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return value;
}

static bool EnableDebugPrivilege(std::string& error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        error = "OpenProcessToken failed: " + std::to_string(GetLastError());
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        error = "LookupPrivilegeValue failed: " + std::to_string(GetLastError());
        CloseHandle(token);
        return false;
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool adjusted = AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr) != FALSE;
    const DWORD status = GetLastError();
    CloseHandle(token);
    if (!adjusted || status == ERROR_NOT_ALL_ASSIGNED) {
        error = "SeDebugPrivilege is unavailable: " + std::to_string(status);
        return false;
    }
    return true;
}

static std::string Narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

static std::string Hex(std::uint64_t value) {
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

static DWORD AlignUp(DWORD value, DWORD alignment) {
    if (alignment == 0) return value;
    const DWORD remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

static bool ReadFileBytes(const fs::path& path, std::vector<std::uint8_t>& bytes, std::string& error) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        error = "cannot open " + path.string();
        return false;
    }
    const std::streamsize size = input.tellg();
    if (size <= 0) {
        error = "file is empty: " + path.string();
        return false;
    }
    input.seekg(0, std::ios::beg);
    bytes.resize(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()), size)) {
        error = "cannot read " + path.string();
        return false;
    }
    return true;
}

static bool ParsePe(const fs::path& path, PeImage& image, std::string& error) {
    if (!ReadFileBytes(path, image.bytes, error)) return false;
    if (image.bytes.size() < sizeof(IMAGE_DOS_HEADER)) {
        error = "file is too small for DOS header";
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.bytes.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        error = "not a PE file: " + path.string();
        return false;
    }
    if (static_cast<std::size_t>(dos->e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) > image.bytes.size()) {
        error = "invalid PE header offset";
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.bytes.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        error = "GameAssembly is not an AMD64 PE";
        return false;
    }
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = "GameAssembly is not a PE32+ image";
        return false;
    }
    image.fileAlignment = nt->OptionalHeader.FileAlignment;
    image.sectionAlignment = nt->OptionalHeader.SectionAlignment;
    image.sizeOfHeaders = nt->OptionalHeader.SizeOfHeaders;
    image.sizeOfImage = nt->OptionalHeader.SizeOfImage;
    image.preferredImageBase = nt->OptionalHeader.ImageBase;
    image.sectionTableOffset = static_cast<std::size_t>(dos->e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt->FileHeader.SizeOfOptionalHeader;
    const std::size_t tableSize = static_cast<std::size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (image.sectionTableOffset + tableSize > image.bytes.size()) {
        error = "invalid section table";
        return false;
    }
    const auto* sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(image.bytes.data() + image.sectionTableOffset);
    image.sections.clear();
    image.sections.reserve(nt->FileHeader.NumberOfSections);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index) {
        const auto& section = sections[index];
        char name[9]{};
        std::memcpy(name, section.Name, 8);
        image.sections.push_back(SectionInfo{
            name,
            section.VirtualAddress,
            section.Misc.VirtualSize,
            section.SizeOfRawData,
            section.PointerToRawData,
            section.Characteristics,
        });
    }
    return true;
}

static DWORD FindProcessId(const std::wstring& executableName) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD result = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (ToLower(entry.szExeFile) == ToLower(executableName)) {
                result = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

static bool FindGameAssemblyModule(DWORD pid, ModuleInfo& result, std::string& error) {
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        error = "OpenProcess failed: " + std::to_string(GetLastError());
        return false;
    }
    std::string psapiError;
    std::vector<HMODULE> modules(1024);
    DWORD bytesNeeded = 0;
    if (!EnumProcessModulesEx(process, modules.data(), static_cast<DWORD>(modules.size() * sizeof(HMODULE)), &bytesNeeded, LIST_MODULES_64BIT)) {
        psapiError = "EnumProcessModulesEx failed: " + std::to_string(GetLastError());
    } else {
        const DWORD moduleCount = std::min<DWORD>(bytesNeeded / sizeof(HMODULE), static_cast<DWORD>(modules.size()));
        for (DWORD index = 0; index < moduleCount; ++index) {
            wchar_t modulePath[MAX_PATH * 4]{};
            if (GetModuleFileNameExW(process, modules[index], modulePath, static_cast<DWORD>(std::size(modulePath))) == 0) continue;
            const fs::path path(modulePath);
            if (ToLower(path.filename().wstring()) != L"gameassembly.dll") continue;
            MODULEINFO module{};
            if (!GetModuleInformation(process, modules[index], &module, sizeof(module))) continue;
            result = ModuleInfo{pid, process, reinterpret_cast<std::uintptr_t>(module.lpBaseOfDll), module.SizeOfImage, modulePath};
            return true;
        }
    }

    // Some protected Unity processes reject PSAPI module enumeration while
    // still allowing the Toolhelp snapshot API. Use it as a fallback.
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Module32FirstW(snapshot, &entry)) {
            do {
                if (ToLower(entry.szModule) == L"gameassembly.dll") {
                    result = ModuleInfo{pid, process, reinterpret_cast<std::uintptr_t>(entry.modBaseAddr), entry.modBaseSize, entry.szExePath};
                    CloseHandle(snapshot);
                    return true;
                }
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }

    error = "GameAssembly.dll is not loaded or module enumeration is blocked";
    if (!psapiError.empty()) error += " (" + psapiError + ")";
    CloseHandle(process);
    return false;
}

static bool FindGameAssemblyByMemoryScan(HANDLE process, DWORD pid, ModuleInfo& result) {
    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    auto address = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const auto maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == 0) break;
        const auto next = address + std::max<std::size_t>(memory.RegionSize, 0x1000);
        if (memory.State == MEM_COMMIT && memory.BaseAddress == memory.AllocationBase &&
            (memory.Type == MEM_IMAGE || memory.Type == MEM_PRIVATE) && memory.RegionSize >= 0x1000) {
            IMAGE_DOS_HEADER dos{};
            SIZE_T read = 0;
            if (ReadProcessMemory(process, memory.AllocationBase, &dos, sizeof(dos), &read) &&
                read == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 && dos.e_lfanew < 0x100000) {
                IMAGE_NT_HEADERS64 nt{};
                if (ReadProcessMemory(process, reinterpret_cast<const std::uint8_t*>(memory.AllocationBase) + dos.e_lfanew,
                                      &nt, sizeof(nt), &read) && read >= sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) &&
                    nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
                    nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC && nt.FileHeader.NumberOfSections >= 3 &&
                    nt.FileHeader.NumberOfSections <= 32 && nt.OptionalHeader.SizeOfImage >= 0x1000000 &&
                    nt.OptionalHeader.SizeOfImage <= 0x20000000 && nt.OptionalHeader.SizeOfHeaders >= 0x400) {
                    const std::size_t tableOffset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD) +
                        sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
                    std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
                    if (ReadProcessMemory(process, reinterpret_cast<const std::uint8_t*>(memory.AllocationBase) + tableOffset,
                                          sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER), &read) &&
                        read == sections.size() * sizeof(IMAGE_SECTION_HEADER)) {
                        bool hasIl2CppSection = false;
                        bool hasLargeCodeSection = false;
                        for (const auto& section : sections) {
                            char name[9]{};
                            std::memcpy(name, section.Name, 8);
                            const std::string sectionName(name);
                            if (sectionName == "il2cpp") hasIl2CppSection = true;
                            if (section.Misc.VirtualSize >= 0x1000000 &&
                                (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) hasLargeCodeSection = true;
                        }
                        if (hasIl2CppSection || hasLargeCodeSection) {
                            wchar_t processPath[MAX_PATH * 4]{};
                            DWORD processPathLength = static_cast<DWORD>(std::size(processPath));
                            fs::path sourcePath;
                            if (QueryFullProcessImageNameW(process, 0, processPath, &processPathLength)) {
                                sourcePath = fs::path(processPath).parent_path() / L"GameAssembly.dll";
                            } else {
                                sourcePath = L"GameAssembly.dll";
                            }
                            result = ModuleInfo{pid, process, reinterpret_cast<std::uintptr_t>(memory.AllocationBase),
                                                nt.OptionalHeader.SizeOfImage, sourcePath.wstring()};
                            return true;
                        }
                    }
                }
            }
        }
        if (next <= address) break;
        address = next;
    }
    return false;
}

static bool FindGameAssemblyBySectionLayout(HANDLE process, DWORD pid, const PeImage& sourcePe,
                                             const fs::path& sourcePath, ModuleInfo& result) {
    const auto executableSection = std::find_if(sourcePe.sections.begin(), sourcePe.sections.end(), [](const SectionInfo& section) {
        return (section.characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 && section.virtualSize >= 0x1000000;
    });
    if (executableSection == sourcePe.sections.end()) return false;

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == 0) break;
        const auto regionBase = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const auto next = address + std::max<std::size_t>(memory.RegionSize, 0x1000);
        const bool executable = (memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (memory.State == MEM_COMMIT && executable && memory.RegionSize >= executableSection->virtualSize / 2 &&
            memory.RegionSize <= executableSection->virtualSize * 2) {
            if (regionBase >= executableSection->virtualAddress) {
                const auto candidateBase = regionBase - executableSection->virtualAddress;
                int matchedSections = 0;
                bool hasWritable = false;
                bool hasReadable = false;
                for (const auto& section : sourcePe.sections) {
                    MEMORY_BASIC_INFORMATION sectionMemory{};
                    const auto sectionAddress = candidateBase + section.virtualAddress;
                    if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(sectionAddress), &sectionMemory, sizeof(sectionMemory)) == 0) continue;
                    const auto sectionBase = reinterpret_cast<std::uintptr_t>(sectionMemory.BaseAddress);
                    const auto sectionEnd = sectionBase + sectionMemory.RegionSize;
                    const bool coversStart = sectionAddress >= sectionBase;
                    const bool readable = (sectionMemory.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
                    if (sectionMemory.State == MEM_COMMIT && coversStart && readable && sectionEnd > sectionAddress) {
                        ++matchedSections;
                        if ((sectionMemory.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0)
                            hasWritable = true;
                        if ((sectionMemory.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                                                      PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0)
                            hasReadable = true;
                    }
                }
                if (matchedSections >= 3 && hasWritable && hasReadable) {
                    result = ModuleInfo{pid, process, candidateBase, sourcePe.sizeOfImage, sourcePath.wstring()};
                    return true;
                }
            }
        }
        if (next <= address) break;
        address = next;
    }
    return false;
}

static bool FindGameAssemblyModuleRobust(DWORD pid, const PeImage& sourcePe, const fs::path& sourcePath,
                                         ModuleInfo& result, std::string& error) {
    if (FindGameAssemblyModule(pid, result, error)) return true;
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_OPERATION, FALSE, pid);
    if (!process) {
        error = "OpenProcess for memory scan failed: " + std::to_string(GetLastError());
        return false;
    }
    if (FindGameAssemblyByMemoryScan(process, pid, result)) return true;
    if (FindGameAssemblyBySectionLayout(process, pid, sourcePe, sourcePath, result)) return true;
    CloseHandle(process);
    error = "GameAssembly.dll was not found by module enumeration, PE-header scan, or section-layout scan";
    return false;
}

static bool ReadRemote(HANDLE process, std::uintptr_t address, void* destination, std::size_t size, std::size_t& bytesRead) {
    bytesRead = 0;
    auto* output = static_cast<std::uint8_t*>(destination);
    constexpr std::size_t chunkSize = 1024 * 1024;
    while (bytesRead < size) {
        const std::size_t chunk = std::min(chunkSize, size - bytesRead);
        SIZE_T read = 0;
        if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address + bytesRead), output + bytesRead, chunk, &read) || read == 0) {
            return false;
        }
        bytesRead += static_cast<std::size_t>(read);
        if (read < chunk) return false;
    }
    return true;
}

static std::uint64_t Fnv1a(const std::uint8_t* data, std::size_t size, std::uint64_t hash = 14695981039346656037ull) {
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

static Sample ProbeMaterialization(const ModuleInfo& module, const PeImage& pe) {
    Sample result{};
    for (const auto& section : pe.sections) {
        if (section.virtualSize <= 0x2000) continue;
        const bool codeLike = section.name == ".text" || section.name == "il2cpp" || section.name == ".pdata";
        if (!codeLike) continue;
        const DWORD offset = std::min<DWORD>(section.virtualSize - 0x1000, std::max<DWORD>(0x1000, section.originalRawSize));
        std::vector<std::uint8_t> sample(0x1000);
        std::size_t read = 0;
        if (!ReadRemote(module.process, module.base + section.virtualAddress + offset, sample.data(), sample.size(), read)) {
            ++result.readFailures;
            result.lastReadError = GetLastError();
            continue;
        }
        result.hash = Fnv1a(sample.data(), sample.size(), result.hash == 0 ? 14695981039346656037ull : result.hash);
        const std::size_t nonZero = static_cast<std::size_t>(std::count_if(sample.begin(), sample.end(), [](std::uint8_t value) { return value != 0; }));
        result.nonZeroBytes += nonZero;
        if (nonZero >= 32) result.materialized = true;
    }
    return result;
}

static std::string JsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (const char character : value) {
        if (character == '\\' || character == '"') result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

static bool CaptureImage(const ModuleInfo& module, const PeImage& pe, const fs::path& outputPath, const fs::path& mapPath, std::string& error) {
    if (module.size < pe.sizeOfImage) {
        error = "loaded module is smaller than PE SizeOfImage";
        return false;
    }
    std::vector<std::uint8_t> output(pe.sizeOfHeaders, 0);
    if (output.size() > pe.bytes.size()) {
        error = "PE headers exceed source file";
        return false;
    }
    std::copy_n(pe.bytes.begin(), output.size(), output.begin());

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(output.data());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(output.data() + dos->e_lfanew);
    nt->OptionalHeader.ImageBase = module.base;
    auto* sectionHeaders = reinterpret_cast<IMAGE_SECTION_HEADER*>(output.data() + pe.sectionTableOffset);

    DWORD nextRawOffset = AlignUp(pe.sizeOfHeaders, pe.fileAlignment);
    struct CapturedSection { SectionInfo info; DWORD rawOffset; DWORD rawSize; bool readComplete; };
    std::vector<CapturedSection> captured;
    captured.reserve(pe.sections.size());
    for (std::size_t index = 0; index < pe.sections.size(); ++index) {
        const auto& section = pe.sections[index];
        const DWORD virtualSize = std::max<DWORD>(section.virtualSize, section.originalRawSize);
        const DWORD rawSize = AlignUp(virtualSize, pe.fileAlignment);
        nextRawOffset = AlignUp(nextRawOffset, pe.fileAlignment);
        if (nextRawOffset > 0x7FFFFFFF || rawSize > 0x7FFFFFFF - nextRawOffset) {
            error = "reconstructed PE is too large";
            return false;
        }
        sectionHeaders[index].PointerToRawData = nextRawOffset;
        sectionHeaders[index].SizeOfRawData = rawSize;
        captured.push_back(CapturedSection{section, nextRawOffset, rawSize, false});
        nextRawOffset += rawSize;
    }
    output.resize(nextRawOffset, 0);

    std::ostringstream map;
    map << "{\n"
        << "  \"pid\": " << module.pid << ",\n"
        << "  \"moduleBase\": \"" << Hex(module.base) << "\",\n"
        << "  \"moduleSize\": " << module.size << ",\n"
        << "  \"preferredImageBase\": \"" << Hex(pe.preferredImageBase) << "\",\n"
        << "  \"sourcePath\": \"" << JsonEscape(Narrow(module.path)) << "\",\n"
        << "  \"sections\": [\n";

    for (std::size_t index = 0; index < captured.size(); ++index) {
        auto& section = captured[index];
        auto* destination = output.data() + section.rawOffset;
        std::size_t read = 0;
        section.readComplete = ReadRemote(module.process, module.base + section.info.virtualAddress, destination, section.info.virtualSize, read);
        if (!section.readComplete) {
            const std::size_t fallback = std::min<std::size_t>(section.info.originalRawSize, section.info.virtualSize);
            if (section.info.originalRawOffset < pe.bytes.size()) {
                const std::size_t available = std::min(fallback, pe.bytes.size() - section.info.originalRawOffset);
                std::copy_n(pe.bytes.begin() + section.info.originalRawOffset, available, destination);
            }
        }
        map << "    {\"name\": \"" << JsonEscape(section.info.name) << "\", \"virtualAddress\": \""
            << Hex(section.info.virtualAddress) << "\", \"virtualSize\": " << section.info.virtualSize
            << ", \"rawOffset\": " << section.rawOffset << ", \"rawSize\": " << section.rawSize
            << ", \"remoteReadComplete\": " << (section.readComplete ? "true" : "false") << "}";
        if (index + 1 < captured.size()) map << ',';
        map << '\n';
    }
    map << "  ]\n}\n";

    std::ofstream imageFile(outputPath, std::ios::binary | std::ios::trunc);
    if (!imageFile.write(reinterpret_cast<const char*>(output.data()), static_cast<std::streamsize>(output.size()))) {
        error = "cannot write " + outputPath.string();
        return false;
    }
    std::ofstream mapFile(mapPath, std::ios::binary | std::ios::trunc);
    const std::string mapText = map.str();
    if (!mapFile.write(mapText.data(), static_cast<std::streamsize>(mapText.size()))) {
        error = "cannot write " + mapPath.string();
        return false;
    }
    return true;
}

static void PrintUsage() {
    std::wcout << L"Gakumas GameAssembly runtime capture\n"
               << L"Waits for a manually started gakumas.exe, captures the unpacked GameAssembly.dll, and exits.\n\n"
               << L"Options:\n"
               << L"  --process <name>    Process name (default: gakumas.exe)\n"
               << L"  --output <dir>      Output directory\n"
               << L"  --timeout <seconds> Wait timeout (default: 180)\n"
               << L"  --help              Show this help\n";
}

int wmain(int argc, wchar_t** argv) {
    std::wstring processName = L"gakumas.exe";
    fs::path outputDirectory = L"D:\\Games\\gakumas\\BepInEx\\gakumas-runtime-capture";
    int timeoutSeconds = 180;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--help") {
            PrintUsage();
            return 0;
        }
        if (argument == L"--process" && index + 1 < argc) {
            processName = argv[++index];
            continue;
        }
        if (argument == L"--output" && index + 1 < argc) {
            outputDirectory = argv[++index];
            continue;
        }
        if (argument == L"--timeout" && index + 1 < argc) {
            timeoutSeconds = std::max(1, _wtoi(argv[++index]));
            continue;
        }
        std::wcerr << L"Unknown argument: " << argument << L"\n";
        PrintUsage();
        return 2;
    }

    std::error_code directoryError;
    fs::create_directories(outputDirectory, directoryError);
    if (directoryError) {
        std::wcerr << L"Cannot create output directory: " << outputDirectory << L"\n";
        return 3;
    }
    const fs::path logPath = outputDirectory / L"capture.log";
    std::ofstream log(logPath, std::ios::trunc);
    auto writeLog = [&](const std::string& message) {
        std::cout << message << std::endl;
        log << message << '\n';
        log.flush();
    };

    std::string privilegeError;
    if (EnableDebugPrivilege(privilegeError)) {
        writeLog("SeDebugPrivilege enabled.");
    } else {
        writeLog("Warning: " + privilegeError);
    }

    const fs::path sourcePath = outputDirectory.parent_path().parent_path() / L"GameAssembly.dll";
    PeImage pe{};
    std::string error;
    if (!ParsePe(sourcePath, pe, error)) {
        writeLog("Cannot parse source GameAssembly.dll: " + error);
        return 6;
    }
    writeLog("Source PE sections=" + std::to_string(pe.sections.size()) + ", preferredBase=" + Hex(pe.preferredImageBase));

    writeLog("Waiting for " + Narrow(processName) + "; start the game manually now.");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    DWORD pid = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        pid = FindProcessId(processName);
        if (pid != 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (pid == 0) {
        writeLog("Timed out waiting for process.");
        return 4;
    }
    writeLog("Found process pid=" + std::to_string(pid));

    ModuleInfo module{};
    int moduleAttempts = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (FindGameAssemblyModuleRobust(pid, pe, sourcePath, module, error)) break;
        ++moduleAttempts;
        if (moduleAttempts % 10 == 0) writeLog("Still waiting for GameAssembly.dll: " + error);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!module.process) {
        writeLog("Timed out waiting for GameAssembly.dll: " + error);
        return 5;
    }
    writeLog("GameAssembly loaded at " + Hex(module.base) + ", size=" + std::to_string(module.size));

    Sample previous{};
    int stableSamples = 0;
    bool captured = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (WaitForSingleObject(module.process, 0) == WAIT_OBJECT_0) {
            writeLog("Game process exited before the runtime image stabilized.");
            break;
        }
        const Sample current = ProbeMaterialization(module, pe);
        const bool stable = current.materialized && current.hash == previous.hash;
        stableSamples = stable ? stableSamples + 1 : 0;
        writeLog("Probe materialized=" + std::string(current.materialized ? "true" : "false") +
                 " nonZero=" + std::to_string(current.nonZeroBytes) +
                 " readFailures=" + std::to_string(current.readFailures) +
                 " lastReadError=" + std::to_string(current.lastReadError) +
                 " hash=" + Hex(current.hash) +
                 " stableSamples=" + std::to_string(stableSamples));
        if (stableSamples >= 3) {
            const fs::path imagePath = outputDirectory / L"GameAssembly.runtime.dll";
            const fs::path mapPath = outputDirectory / L"GameAssembly.runtime.map.json";
            if (!CaptureImage(module, pe, imagePath, mapPath, error)) {
                writeLog("Capture failed: " + error);
                CloseHandle(module.process);
                return 7;
            }
            writeLog("Capture complete: " + imagePath.string());
            writeLog("Map complete: " + mapPath.string());
            captured = true;
            break;
        }
        previous = current;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    CloseHandle(module.process);
    if (!captured) {
        writeLog("Timed out before capture completed.");
        return 8;
    }
    return 0;
}
