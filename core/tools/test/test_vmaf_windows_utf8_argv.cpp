/**
 * Copyright 2026 Lusoris
 * SPDX-License-Identifier: EUPL-1.2
 */

#include <array>
#include <cstdio>
#include <string>

#include <windows.h>

namespace
{

constexpr DWORD kChildWaitMs = 30'000;
constexpr DWORD kFrameBytes = 24u * 24u * 3u / 2u;

struct FixturePaths {
    std::wstring directory;
    std::wstring reference;
    std::wstring distorted;
    std::wstring output;
};

bool make_fixture_paths(FixturePaths *paths)
{
    std::array<wchar_t, MAX_PATH + 1u> temp_root{};
    const DWORD root_len = GetTempPathW(static_cast<DWORD>(temp_root.size()), temp_root.data());
    if (root_len == 0u || root_len >= temp_root.size())
        return false;

    const std::wstring unique = L"vmafx-cli-utf8-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64());
    paths->directory.assign(temp_root.data(), root_len);
    paths->directory += unique;
    paths->reference = paths->directory + L"\\référence_日本.yuv";
    paths->distorted = paths->directory + L"\\déformé_日本.yuv";
    paths->output = paths->directory + L"\\résultat_日本.json";
    return CreateDirectoryW(paths->directory.c_str(), nullptr) != 0;
}

bool write_zero_frame(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        (void)std::fprintf(stderr, "CreateFileW failed (win32=%lu)\n", GetLastError());
        return false;
    }

    const std::array<unsigned char, kFrameBytes> frame{};
    DWORD written = 0;
    const BOOL write_ok = WriteFile(file, frame.data(), kFrameBytes, &written, nullptr);
    const DWORD write_error = write_ok != 0 ? ERROR_SUCCESS : GetLastError();
    const BOOL close_ok = CloseHandle(file);
    if (write_ok == 0 || written != kFrameBytes || close_ok == 0) {
        (void)std::fprintf(stderr, "fixture write failed (win32=%lu, bytes=%lu)\n", write_error,
                           written);
    }
    return write_ok != 0 && written == kFrameBytes && close_ok != 0;
}

size_t last_separator(const std::wstring &path)
{
    return path.find_last_of(L"\\/");
}

std::wstring vmaf_binary_path()
{
    std::array<wchar_t, 32'768> module{};
    const DWORD len = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (len == 0u || len >= module.size())
        return {};
    std::wstring path(module.data(), len);
    const size_t filename = last_separator(path);
    if (filename == std::wstring::npos)
        return {};
    path.resize(filename);
    const size_t test_directory = last_separator(path);
    if (test_directory == std::wstring::npos)
        return {};
    path.resize(test_directory + 1u);
    path += L"vmaf.exe";
    return path;
}

std::wstring quoted(const std::wstring &path)
{
    return L"\"" + path + L"\"";
}

} // namespace

namespace
{

bool run_vmaf(const FixturePaths &paths)
{
    const std::wstring binary = vmaf_binary_path();
    std::wstring command = quoted(binary) + L" -r " + quoted(paths.reference) + L" -d " +
                           quoted(paths.distorted) +
                           L" -w 24 -h 24 -p 420 -b 8 --feature psnr --no_prediction --json "
                           L"--output " +
                           quoted(paths.output);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                       &startup, &process) == 0) {
        return false;
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, kChildWaitMs);
    DWORD exit_code = 1u;
    const BOOL exit_ok = wait == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &exit_code);
    if (wait == WAIT_TIMEOUT) {
        (void)TerminateProcess(process.hProcess, 1u);
        (void)WaitForSingleObject(process.hProcess, kChildWaitMs);
    }
    const BOOL thread_closed = CloseHandle(process.hThread);
    const BOOL process_closed = CloseHandle(process.hProcess);
    return exit_ok != 0 && exit_code == 0u && thread_closed != 0 && process_closed != 0;
}

bool output_is_nonempty(const std::wstring &path)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER size{};
    const BOOL size_ok = GetFileSizeEx(file, &size);
    const BOOL close_ok = CloseHandle(file);
    return size_ok != 0 && size.QuadPart > 0 && close_ok != 0;
}

void cleanup(const FixturePaths &paths)
{
    (void)DeleteFileW(paths.output.c_str());
    (void)DeleteFileW(paths.distorted.c_str());
    (void)DeleteFileW(paths.reference.c_str());
    (void)RemoveDirectoryW(paths.directory.c_str());
}

} // namespace

int main() noexcept
{
    FixturePaths paths;
    try {
        if (!make_fixture_paths(&paths)) {
            (void)std::fprintf(stderr, "failed to create Unicode fixture directory\n");
            return 1;
        }
        const bool fixtures_ok =
            write_zero_frame(paths.reference) && write_zero_frame(paths.distorted);
        const bool child_ok = fixtures_ok && run_vmaf(paths);
        const bool output_ok = child_ok && output_is_nonempty(paths.output);
        cleanup(paths);

        if (!fixtures_ok) {
            (void)std::fprintf(stderr, "failed to create Unicode input paths\n");
        } else if (!child_ok) {
            (void)std::fprintf(stderr, "vmaf rejected the exact Unicode command line\n");
        } else if (!output_ok) {
            (void)std::fprintf(stderr, "vmaf did not create the exact Unicode output path\n");
        }
        return fixtures_ok && child_ok && output_ok ? 0 : 1;
    } catch (...) {
        cleanup(paths);
        (void)std::fprintf(stderr, "Unicode CLI regression setup raised an exception\n");
        return 1;
    }
}
