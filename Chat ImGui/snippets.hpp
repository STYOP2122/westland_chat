#pragma once

#include <string>
#include <string_view>

#include <Windows.h>

inline std::string cp1251_to_utf8(std::string_view str) {
    int result_w = MultiByteToWideChar(1251, 0, str.data(),
        static_cast<int>(str.size()), NULL, 0);
    if (result_w == 0)
        return "";

    std::wstring wres(result_w, '\0');
    if (!MultiByteToWideChar(1251, 0, str.data(), static_cast<int>(str.size()),
        wres.data(), result_w))
        return "";

    int result_c =
        WideCharToMultiByte(CP_UTF8, 0, wres.data(),
            static_cast<int>(wres.size()), NULL, 0, NULL, NULL);
    if (result_c == 0)
        return "";

    std::string res(result_c, '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, wres.data(),
        static_cast<int>(wres.size()), res.data(), result_c,
        0, 0))
        return "";

    return res;
}

inline std::string utf8_to_cp1251(std::string_view str) {
    int result_w = MultiByteToWideChar(CP_UTF8, 0, str.data(),
        static_cast<int>(str.size()), NULL, 0);
    if (result_w == 0)
        return "";

    std::wstring wres(result_w, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, 0, str.data(), static_cast<int>(str.size()),
        wres.data(), result_w))
        return "";

    int result_c = WideCharToMultiByte(1251, 0, wres.data(),
        static_cast<int>(wres.size()), NULL, 0, NULL, NULL);
    if (result_c == 0)
        return "";

    std::string res(result_c, '\0');
    if (!WideCharToMultiByte(1251, 0, wres.data(),
        static_cast<int>(wres.size()), res.data(), result_c,
        NULL, NULL))
        return "";

    return res;
}
