#include "StringUtils.h"
#include <windows.h>
#include <random>

std::string AnsiToUtf8(const std::string& ansiStr) {
    if (ansiStr.empty()) return "";
    int wLen = MultiByteToWideChar(CP_ACP, 0, ansiStr.c_str(), -1, NULL, 0);
    std::wstring wStr(wLen, 0);
    MultiByteToWideChar(CP_ACP, 0, ansiStr.c_str(), -1, &wStr[0], wLen);
    int uLen = WideCharToMultiByte(CP_UTF8, 0, wStr.c_str(), -1, NULL, 0, NULL, NULL);
    std::string utf8Str(uLen - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, wStr.c_str(), -1, &utf8Str[0], uLen, NULL, NULL);
    return utf8Str;
}

std::string GenerateRandomID(int length) {
    const std::string CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<> dist(0, CHARS.size() - 1);
    std::string random_string;
    for (int i = 0; i < length; ++i) random_string += CHARS[dist(generator)];
    return random_string;
}

std::string GenerateRandomPW(int length) {
    const std::string CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!@#$%^&*";
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<> dist(0, CHARS.size() - 1);
    std::string random_string;
    for (int i = 0; i < length; ++i) random_string += CHARS[dist(generator)];
    return random_string;
}