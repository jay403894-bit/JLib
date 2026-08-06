#pragma once



#define WIN32_LEAN_AND_MEAN

#include <Windows.h> 
#include <exception>
#include <stdexcept>
#include <vector>
#include <fstream>
#include <cstdio>    

namespace JLib {
    inline void ThrowIfFailed(HRESULT hr)
    {
        if (FAILED(hr))
        {
            char buf[64];
            sprintf_s(buf, "HRESULT failed: 0x%08lX", static_cast<unsigned long>(hr));
            OutputDebugStringA(buf);   // shows in the VS Output window
            OutputDebugStringA("\n");
            throw std::runtime_error(buf);
        }
    }
    // Resolve a filename to sit next to the running .exe (= the build output dir, where
    // FxCompile writes the .cso). This makes shader loading independent of the working
    // directory -- which is what bit us: VS debugs with cwd = $(ProjectDir), so a stale
    // .cso left in the project root was loaded instead of the freshly built one.
    inline std::wstring ExeRelative(const std::wstring& filename) {
        wchar_t path[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring dir(path, n);
        size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) dir.resize(slash + 1);
        return dir + filename;
    }
    // Narrow-string sibling of ExeRelative -- same reasoning (cwd != exe dir when debugging from
    // VS), but for APIs that take const char* instead of const wchar_t* (e.g.
    // SoundManager::PlaySound/PlayLoop, which hand the path straight to miniaudio's ma_decoder,
    // a narrow-string API). "sound\\name.mp3" (no leading slash -- that would make it look
    // absolute/rooted and defeat the whole point) in, "<exeDir>\\sound\\name.mp3" out.
    inline std::string ExeRelativeA(const std::string& filename) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string dir(path, n);
        size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) dir.resize(slash + 1);
        return dir + filename;
    }

    inline std::vector<char> ReadFile(const std::wstring& filename) {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file) {
            std::string narrow(filename.begin(), filename.end());
            throw std::runtime_error("ReadFile: cannot open '" + narrow + "'");
        }
        std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<char> buffer(static_cast<size_t>(size));
        file.read(buffer.data(), size);
        return buffer;
    }
};