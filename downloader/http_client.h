//
// Created by 翔 on 25-11-13.
//
#pragma once
#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <string>
#include <vector>
#include <filesystem>
#include <regex>
#include <iostream>

// 获取m3u8文件
// sav.tw中该文件链接直接在网页前端中可以找到。

// 获取本地代理
static std::string getSystemHttpProxy();

class HttpClient {
public:
    HttpClient(const std::string& Url);
    // 获取url对应html
    std::string GetHtmlFromUrl();
    // 提取m3u8链接
    std::vector<std::string> ExtractLinkOfM3U8(const std::string& html);
    static std::string ExtractTitle(const std::string& html);
private:
    std::string baseUrl; // 域名
    std::string url;
};

// 删除指定路径下的文件
inline bool deleteFile(const std::string& path)
{
    try {
        if (std::filesystem::exists(path)) {
            return std::filesystem::remove(path);
        }
    } catch (const std::filesystem::filesystem_error& e) {
       return false;
    }
    return false;
}

#endif //HTTP_CLIENT_H
