/// @file hash.h
/// @brief SHA-256 文件哈希
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fssentry {

/// @brief 对文件计算 SHA-256，返回 hex 小写字符串（64KB 循环读取）
inline std::string ComputeFileHash(const std::wstring& path)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        CloseHandle(h);
        return {};
    }
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(h);
        return {};
    }

    uint8_t buf[65536];
    DWORD bytesRead;
    while (ReadFile(h, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
        if (BCryptHashData(hHash, buf, bytesRead, 0) < 0) {
            CloseHandle(h);
            BCryptDestroyHash(hHash);
            BCryptCloseAlgorithmProvider(hAlg, 0);
            return {};
        }
    }
    CloseHandle(h);

    DWORD hashLen = 0, cbData = 0;
    BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&hashLen, sizeof(hashLen), &cbData, 0);
    std::vector<uint8_t> hash(hashLen);
    BCryptFinishHash(hHash, hash.data(), hashLen, 0);
    std::string result(hashLen * 2, '\0');
    for (DWORD i = 0; i < hashLen; ++i)
        snprintf(&result[i * 2], 3, "%02x", hash[i]);

    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return result;
}

} // namespace fssentry
