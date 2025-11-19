//
// Created by 翔 on 25-11-13.
//

#ifndef M3U8_DOWNLOADER_H
#define M3U8_DOWNLOADER_H

#include <string>
#include <vector>
#include <unordered_map>
#include <regex>
#include <iomanip>
#include <sstream>
#include <__filesystem/filesystem_error.h>
#include <openssl/sha.h>

// 计算文件hash值
std::string sha256(const std::vector<unsigned char>& data);

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
    ~m3u8Downloader() {
        TsLinks.clear();
        tsFiles.clear();
        decryptedFiles.clear();
        key.clear();
        iv.clear();
        videoHashMap.clear();
    };

    // 常见video格式
    enum class VideoFormat{ TS = 0, MP4, MKV, MOV};
    std::atomic<bool> isRepeat = false; // 当前视频是否重复

    using VF = m3u8Downloader::VideoFormat;
    // inline函数不适成员函数，属于类外函数
    inline VF GetFormatFromExtension(const std::string& ext) {
        std::string e = ext;
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);

        if (e == "ts") return m3u8Downloader::VideoFormat::TS;
        if (e == "mp4") return m3u8Downloader::VideoFormat::MP4;
        if (e == "mkv") return m3u8Downloader::VideoFormat::MKV;
        if (e == "mov") return m3u8Downloader::VideoFormat::MOV;

        return m3u8Downloader::VideoFormat::TS;
    }
    inline std::string Format2String(const VF format) {
        switch (format) {
            case VF::TS: return ".ts";
            case VF::MP4: return ".mp4";
            case VF::MKV: return ".mkv";
            case VF::MOV: return ".mov";
        }
    }

    bool parseM3U8();
    void printInfo() const;
    bool DownloadAllSegments(const std::filesystem::path& dirPath, std::function<void(int)> progressCallBack = nullptr);
    bool DecryptAllTs(std::function<void(int)> progressCallBack = nullptr);
    bool MergeToVideo(const std::filesystem::path& outputFile, std::function<void(int)> progressCallBack = nullptr, m3u8Downloader::VideoFormat format = m3u8Downloader::VideoFormat::TS);
    void DeleteTemplateFile();

private:
    void parseKey(const std::string& line);
    bool DownloadTsSegment(const std::string& url, const std::filesystem::path& outputFile);
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
    bool DecryptTsFile(const std::filesystem::path& inputFile, const std::filesystem::path& outputFile);

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
    std::unordered_map<std::string, std::filesystem::path> videoHashMap; // [videohash, outputPath]
    std::mutex mapMutex;
};

#endif //M3U8_DOWNLOADER_H
