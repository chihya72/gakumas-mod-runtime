#define NOMINMAX

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;

static std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

static DWORD FindPid(const std::wstring& name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (Lower(entry.szExeFile) == Lower(name)) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

static const char* StateName(DWORD state) {
    if (state == MEM_COMMIT) return "COMMIT";
    if (state == MEM_RESERVE) return "RESERVE";
    if (state == MEM_FREE) return "FREE";
    return "?";
}

static const char* TypeName(DWORD type) {
    if (type == MEM_IMAGE) return "IMAGE";
    if (type == MEM_MAPPED) return "MAPPED";
    if (type == MEM_PRIVATE) return "PRIVATE";
    return "?";
}

static std::string ProtectName(DWORD protect) {
    const DWORD base = protect & 0xFF;
    switch (base) {
        case PAGE_NOACCESS: return "NOACCESS";
        case PAGE_READONLY: return "R";
        case PAGE_READWRITE: return "RW";
        case PAGE_WRITECOPY: return "WC";
        case PAGE_EXECUTE: return "X";
        case PAGE_EXECUTE_READ: return "XR";
        case PAGE_EXECUTE_READWRITE: return "XRW";
        case PAGE_EXECUTE_WRITECOPY: return "XWC";
        default: return "?";
    }
}

static bool Read(HANDLE process, std::uintptr_t address, void* data, std::size_t size) {
    SIZE_T read = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), data, size, &read) && read == size;
}

static bool IsPe(HANDLE process, std::uintptr_t address, IMAGE_NT_HEADERS64& nt, std::vector<IMAGE_SECTION_HEADER>& sections) {
    IMAGE_DOS_HEADER dos{};
    if (!Read(process, address, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 || dos.e_lfanew > 0x100000) return false;
    if (!Read(process, address + static_cast<std::uintptr_t>(dos.e_lfanew), &nt, sizeof(nt)) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || nt.FileHeader.NumberOfSections < 3 ||
        nt.FileHeader.NumberOfSections > 32) return false;
    const std::size_t tableOffset = static_cast<std::size_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
    sections.resize(nt.FileHeader.NumberOfSections);
    return Read(process, address + tableOffset, sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER));
}

int wmain(int argc, wchar_t** argv) {
    std::wstring processName = L"gakumas.exe";
    fs::path output = L"D:\\Games\\gakumas\\BepInEx\\gakumas-runtime-capture\\memory-diagnostic.txt";
    for (int i = 1; i < argc; ++i) {
        const std::wstring argument = argv[i];
        if (argument == L"--process" && i + 1 < argc) processName = argv[++i];
        else if (argument == L"--output" && i + 1 < argc) output = argv[++i];
        else if (argument == L"--help") {
            std::wcout << L"ProcessMemoryDiagnostic [--process gakumas.exe] [--output path]\n";
            return 0;
        }
    }
    const DWORD pid = FindPid(processName);
    if (pid == 0) {
        std::wcerr << L"Process not found: " << processName << L"\n";
        return 2;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_VM_OPERATION, FALSE, pid);
    if (!process) {
        std::cerr << "OpenProcess failed: " << GetLastError() << "\n";
        return 3;
    }
    std::error_code ec;
    fs::create_directories(output.parent_path(), ec);
    std::ofstream report(output, std::ios::trunc);
    report << "pid=" << pid << "\n";
    report << "regions:\n";

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    std::uintptr_t address = reinterpret_cast<std::uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const std::uintptr_t maximum = reinterpret_cast<std::uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    std::size_t regionCount = 0;
    std::size_t peCount = 0;
    while (address < maximum) {
        MEMORY_BASIC_INFORMATION memory{};
        if (VirtualQueryEx(process, reinterpret_cast<LPCVOID>(address), &memory, sizeof(memory)) == 0) break;
        const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(memory.BaseAddress);
        const std::uintptr_t allocation = reinterpret_cast<std::uintptr_t>(memory.AllocationBase);
        const std::uintptr_t next = address + std::max<std::size_t>(memory.RegionSize, 0x1000);
        ++regionCount;
        wchar_t mapped[MAX_PATH * 4]{};
        const DWORD mappedLength = GetMappedFileNameW(process, memory.BaseAddress, mapped, static_cast<DWORD>(std::size(mapped)));
        IMAGE_NT_HEADERS64 nt{};
        std::vector<IMAGE_SECTION_HEADER> sections;
        const bool pe = memory.State == MEM_COMMIT && base == allocation && memory.RegionSize >= 0x1000 && IsPe(process, base, nt, sections);
        if (pe) ++peCount;
        const bool interesting = pe || (memory.State == MEM_COMMIT && (memory.Type == MEM_IMAGE || memory.Type == MEM_PRIVATE) && memory.RegionSize >= 0x1000000);
        if (interesting) {
            report << std::hex << "base=" << base << " alloc=" << allocation << std::dec
                   << " size=" << memory.RegionSize << " state=" << StateName(memory.State)
                   << " type=" << TypeName(memory.Type) << " protect=" << ProtectName(memory.Protect)
                   << " pe=" << (pe ? "yes" : "no");
            if (pe) {
                report << " imageSize=" << nt.OptionalHeader.SizeOfImage << " sections=" << nt.FileHeader.NumberOfSections;
                for (const auto& section : sections) {
                    char name[9]{};
                    std::memcpy(name, section.Name, 8);
                    report << " [" << name << " va=0x" << std::hex << section.VirtualAddress << " vs=0x" << section.Misc.VirtualSize
                           << " ch=0x" << section.Characteristics << std::dec << "]";
                }
            }
            if (mappedLength != 0) {
                std::wstring mappedPath(mapped, mappedLength);
                report << " mapped=";
                for (const wchar_t c : mappedPath) report << static_cast<char>(c < 128 ? c : '?');
            }
            report << '\n';
        }
        if (next <= address) break;
        address = next;
    }
    report << "summary regions=" << regionCount << " peCandidates=" << peCount << "\n";
    report.flush();
    CloseHandle(process);
    std::cout << "Wrote " << output.string() << " regions=" << regionCount << " peCandidates=" << peCount << "\n";
    return 0;
}
