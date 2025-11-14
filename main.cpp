#include <iostream>

#include "downloader/thread_pool.h"
#include "downloader/http_client.h"
#include "downloader/ffmpeg_downloader.h"
#include "m3u8_downloader.h"

int main() {
    std::cout << "input url(s) : ";
    std::string urls;
    getline(std::cin, urls);

    std::string basePath;
    std::cout << "input destination path: ";
    getline(std::cin, basePath);

    HttpClient hClient(urls);
    std::string html = hClient.GetHtmlFromUrl();
    std::string title = HttpClient::ExtractTitle(html);

    if (html.empty()) {
        std::cerr << "Failed to fetch HTML." << std::endl;
        return 1;
    }

    std::cout << "Extracting m3u8 links..." << std::endl;
    auto m3u8Urls = hClient.ExtractLinkOfM3U8(html);

    if (m3u8Urls.empty()) {
        std::cout << "No m3u8 links found." << std::endl;
        return -1;
    } else {
        std::cout << "Found m3u8 URLs:\n";
        for (const auto& url : m3u8Urls)
            std::cout << " - " << url << std::endl;
    }

    for(auto item: m3u8Urls) {
        // 解析m3u8文件
        m3u8Downloader m3u8_downloader(item);
        m3u8_downloader.parseM3U8();
        //m3u8_downloader.printInfo();
        basePath = basePath.back() == '/' ? basePath : basePath + '/';
        std::string dirPath = title.empty() ? basePath + "video" : basePath + title;

        // 下载所有ts分片
        m3u8_downloader.DownloadAllSegments(dirPath);
        // 将下载好的所有ts分片进行解密
        m3u8_downloader.DecryptAllTs();
        // 将所有分片和并为完整视频，如需转换格式，则需要使用ffmpeg
        bool ret = m3u8_downloader.MergeToVideo(basePath + title + "/" + title + ".ts");
        m3u8_downloader.DeleteTemplateFile();

        if (ret) break;
    }

    // 方案1:调用ffmpeg 下载到指定目录
    //return FFmpegDownloader::download(m3u8Urls.front(), outputPath) ? 0 : -1;
    return 0;
}