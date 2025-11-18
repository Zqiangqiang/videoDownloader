//
// Created by 翔 on 25-11-13.
//

#ifndef M3U8_DOWNLOADER_H
#define M3U8_DOWNLOADER_H

#include <string>
#include <vector>
#include <regex>

// 实现对m3u8中分片ts文件的下载
class m3u8Downloader {
public:
    // 从m3u8文件中读取数据并填充
    explicit m3u8Downloader(const std::string& url)
        : m3u8Link(url)
    {
        if (!url.empty()) {
            baseUrl = extractBaseUrl(url); // 自动提取域名
        }
    }

    bool parseM3U8();
    void printInfo() const;
    bool DownloadAllSegments(std::string& dirPath, std::function<void(int)> progressCallBack = nullptr);
    bool DecryptAllTs(std::function<void(int)> progressCallBack = nullptr);
    bool MergeToVideo(const std::string& outputFile);
    void DeleteTemplateFile();

private:
    void parseKey(const std::string& line);
    bool DownloadTsSegment(const std::string& url, const std::string& outputPath);
    static std::string extractBaseUrl(const std::string& fullUrl) {
        std::regex pattern(R"((https?:\/\/[^\/]+))");
        std::smatch match;
        if (std::regex_search(fullUrl, match, pattern))
            return match[1].str();
        return {};
    }
    //从链接中获取到实际的解密key
    std::vector<unsigned char> FetchKey();
    std::vector<unsigned char> HexToBytes(const std::string& hex);
    bool DecryptTsFile(const std::string& inputFile, const std::string& outputFile);

private:
    const std::string m3u8Link;
    std::string baseUrl;
    std::vector<std::string> TsLinks;            // 保存所有ts下载路径
    std::vector<std::string> tsFiles;            // 下载到本地的TS 文件路径
    std::vector<std::string> decryptedFiles;     // 解密后所有TS 文件路径
    std::string key_; // 加密方式
    std::string uri_; // 密钥下载地址
    std::string iv_;  // IV解密向量
    // key 和 iv 均需要使用长度为16子节
    std::vector<unsigned char> key;  // AES key
    std::vector<unsigned char> iv;   // AES IV
};

#endif //M3U8_DOWNLOADER_H
