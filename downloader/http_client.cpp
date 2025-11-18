//
// Created by 翔 on 25-11-13.
//

#include "http_client.h"
#include <curl/curl.h>
#include <iostream>
#include <filesystem>

std::string getSystemHttpProxy() {
    FILE* pipe = popen("networksetup -getwebproxy 'Wi-Fi'", "r");
    if (!pipe) return "";
    char buffer[128];
    std::string result;
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);

    // 解析 IP 和端口
    std::regex ipPortRegex(R"(Server: (.+)\nPort: (\d+))");
    std::smatch match;
    if (std::regex_search(result, match, ipPortRegex)) {
        std::string proxy = match[1].str() + ":" + match[2].str();
        std::cout << "[Proxy] System proxy " << proxy << std::endl;
        return proxy;
    }
    return "";
}

const static std::string proxy = getSystemHttpProxy();

// libcurl 写回调，把HTTP响应写入string
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

HttpClient::HttpClient(const std::string &Url){
    if(!Url.empty()) {
        url = Url;
        // 正则匹配：协议 + 域名（+ 可选端口）
        std::regex pattern(R"((https?:\/\/[^\/]+))");
        std::smatch match;

        if (std::regex_search(Url, match, pattern))
        {
            baseUrl = match[1].str();
        }
    }
}

// 本质上是对该url发出请求并返回响应
std::string HttpClient::GetHtmlFromUrl() {
    CURL* curl;
    CURLcode res;
    std::string readBuffer;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // 支持重定向
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);       // 超时15秒
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        // 临时关闭ssl校验
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        // 添加本地代理
        if (!proxy.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
        }
        // 一些常见网站可能需要模拟浏览器 UA
        curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)"
                         "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36");

        res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            std::cerr << "[Curl] curl_easy_perform() failed: "
                      << curl_easy_strerror(res) << std::endl;
        }

        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
    return readBuffer;
}

// 从HTML源码中提取m3u8链接
std::vector<std::string> HttpClient::ExtractLinkOfM3U8(const std::string& html) {
    std::vector<std::string> urls;

    // 这个正则支持：
    // - http://xxx/xxx.m3u8
    // - https://xxx/xxx.m3u8
    // - "/api/xxx.m3u8"
    // - '/api/xxx.m3u8'
    std::regex pattern(R"((["'])(\/[^"']*?\.m3u8)\1|https?:\/\/[^"']*?\.m3u8)");

    std::smatch match;
    std::string::const_iterator searchStart(html.cbegin());

    while (std::regex_search(searchStart, html.cend(), match, pattern))
    {
        // match[2] 是相对路径（例如 /api/xxx.m3u8）
        // match[0] 是整个匹配内容
        std::string url;
        if (match[2].matched)
            url = baseUrl + match[2].str();
        else
            url = match[0].str();

        urls.push_back(url);
        searchStart = match.suffix().first;
    }

    return urls;
}

std::string HttpClient::ExtractTitle(const std::string& html)
{
    std::regex pattern(R"(<div[^>]*class=["']videoDes["'][^>]*>(.*?)<\/div>)", std::regex::icase);
    std::smatch match;

    if (std::regex_search(html, match, pattern))
    {
        return match[1].str();  // 第一个捕获组即 div 内部文本
    }

    return "";
}
