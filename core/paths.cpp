// core/paths.cpp
#include "paths.h"

#include <cstdio>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace pb {

namespace {

bool fileExists(const std::string& path) {
    if (path.empty()) return false;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

}  // namespace

std::wstring utf8ToWide(const std::string& s) {
#ifdef _WIN32
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
#else
    return std::wstring(s.begin(), s.end());
#endif
}

std::string wideToUtf8(const std::wstring& s) {
#ifdef _WIN32
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0,
                                      nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
#else
    return std::string(s.begin(), s.end());
#endif
}

std::string executableDir() {
#ifdef _WIN32
    // 路径可能很长，留足余量（MAX_PATH 在启用长路径时不够用）
    wchar_t buf[4096];
    const DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= (DWORD)(sizeof(buf) / sizeof(buf[0]))) return std::string();

    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::string();
    return wideToUtf8(p.substr(0, slash + 1));
#else
    // Linux: /proc/self/exe 指向自身
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    std::string p(buf);
    const size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) return std::string();
    return p.substr(0, slash + 1);
#endif
}

std::string findAsset(const std::string& relativePath) {
    if (relativePath.empty()) return std::string();

    // 绝对路径直接判定，不做拼接
    const bool absolute =
        (relativePath.size() > 1 && relativePath[1] == ':') || relativePath[0] == '/' ||
        relativePath[0] == '\\';
    if (absolute) return fileExists(relativePath) ? relativePath : std::string();

    // 1. 当前工作目录（允许用户用同名文件覆盖）
    if (fileExists(relativePath)) return relativePath;

    const std::string dir = executableDir();
    if (!dir.empty()) {
        // 2. 可执行文件所在目录
        const std::string beside = dir + relativePath;
        if (fileExists(beside)) return beside;

        // 3. 上一级（build/ 的上一级就是项目根）
        const std::string parent = dir + "..\\" + relativePath;
        if (fileExists(parent)) return parent;

        // 某些布局下分隔符是 '/'，两种都试一遍，避免因分隔符不匹配而漏掉
        const std::string parentAlt = dir + "../" + relativePath;
        if (fileExists(parentAlt)) return parentAlt;
    }

    return std::string();
}

}  // namespace pb
