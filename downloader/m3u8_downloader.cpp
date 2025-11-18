//
// Created by 翔 on 25-11-13.
//

#include "m3u8_downloader.h"
#include "thread_pool.h"
#include "http_client.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <thread>
#include <filesystem>
#include <openssl/aes.h>
#include <curl/curl.h>
#include <atomic>

// 获取设备的逻辑核心数
// 在I/O密集型操作中可以分配更多的虚拟核心，而在cpu计算密集型中不要超过物理核心数
const unsigned int logical_cores = std::thread::hardware_concurrency();

size_t WriteFileCallback(void* ptr, size_t size, size_t nmemb, void* stream) {
    FILE* fp = (FILE*)stream;
    return fwrite(ptr, size, nmemb, fp);
}

// 确保每片ts文件都能被正确下载，否则在合并时会造成合并结果无法播放
bool m3u8Downloader::DownloadTsSegment(const std::string& url, const std::string& outputPath) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    FILE* fp = fopen(outputPath.c_str(), "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L); // 建立连接超时
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);       // 总超时
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);  // 关闭ssl校验
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
                         "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7)"
                         "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/138.0.0.0 Safari/537.36");

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

    if (res != CURLE_OK) {
        std::cerr << "[Error] Download failed: " << outputPath
              << " - " << curl_easy_strerror(res)
              << ", HTTP code: " << response_code
              << std::endl;
        return false;
    }

    fclose(fp);
    curl_easy_cleanup(curl);
    return true;
}

// 新增进度回调
bool m3u8Downloader::DownloadAllSegments(std::string& dirPath, std::function<void(int)> progressCallBack) {
    // 提前获取key，确保下载完成后能直接解析
    if (key.empty()) {
        key = FetchKey();
    }

    if (TsLinks.empty()) {
        std::cerr << "No TS segments to download!" << std::endl;
        return false;
    }

    ThreadPool pool(logical_cores);
    std::filesystem::create_directories(dirPath);
    std::vector<std::future<void>> results;

    std::cout << "Start downloading " << TsLinks.size() << " TS files..." << std::endl;

    // 初始进度（当前项目占比50%）
    std::atomic<int> doneCount = 0;
    for (size_t i = 0; i < TsLinks.size(); ++i) {
        std::string tsUrl = TsLinks[i];
        std::string outputFile = dirPath + "/segment_" + std::to_string(i) + ".ts";
        tsFiles.emplace_back(outputFile);

        results.emplace_back(pool.enqueue([=, &doneCount]() {
            int count = 1; bool success = true;
            success = this->DownloadTsSegment(tsUrl, outputFile);

            // 新增重试机制，确保能正确下载每一片分片
            while(!success && count++ <= 5) {
                std::cout << "retry " << std::to_string(count) << " times file: " << outputFile << std::endl;
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                success = this->DownloadTsSegment(tsUrl, outputFile);
            }

            // 3次重试后仍失败，直接退出下载
            // TODO: 后续可以将下载失败的片段作出标记，然后在重试时至下载失败片段
            if (success) {
                //std::cout << "download " << std::to_string(i) << " TS success path: " << outputFile << std::endl;
                // 不要直接使用整数除法否则会造成值为0即进度不走的情况
                // 不要使用序号，因为是并发执行，会导致进度条伸缩
                // 不要频繁的回调进度否则会造成很大的性能开销
                doneCount.fetch_add(1);
                if(progressCallBack && doneCount.load() % 5 == 0) {
                    progressCallBack(20 + static_cast<int>((doneCount.load() + 1) * 40.0 / TsLinks.size()));
                }
            } else {
                std::cerr << "download " << std::to_string(i) << " TS failed path: " << outputFile << std::endl;
                std::filesystem::remove(outputFile);
            }
        }));
    }

    // 等待所有任务完成
    for (auto& f : results) {
        f.get();
    }

    if (doneCount.load() == TsLinks.size()) {
        // 下载完成后及时释放TsLinks，减少内存占用
        TsLinks.clear();
        std::cout << "All TS segments downloaded." << std::endl;
        return true;
    } else {
        return false;
    }
}

bool m3u8Downloader::parseM3U8() {
    HttpClient client(m3u8Link);
    std::string content = client.GetHtmlFromUrl();

    if (content.empty()) {
        std::cerr << "Failed to download m3u8 file: " << m3u8Link << std::endl;
        return false;
    }

    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        // 去掉回车符
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        if (line.rfind("#EXT-X-KEY", 0) == 0) {
            parseKey(line);
        }
        else if (!line.empty() && line[0] != '#') {
            // ts 文件链接
            std::string tsUrl = line;
            if (tsUrl.find("http") != 0) {
                // 相对路径补全
                tsUrl = baseUrl + "/" + tsUrl;
            }
            TsLinks.push_back(tsUrl);
        }
    }

    return !TsLinks.empty();
}

void m3u8Downloader::printInfo () const {
    std::cout << "METHOD: " << key_ << std::endl;
    std::cout << "URI: " << uri_ << std::endl;
    std::cout << "IV: " << iv_ << std::endl;
    std::cout << "Total TS files: " << TsLinks.size() << std::endl;

    for (size_t i = 0; i < TsLinks.size(); ++i) {
        std::cout << i + 1 << ": " << TsLinks[i] << std::endl;
    }
}

void m3u8Downloader::parseKey(const std::string& line) {
    // 示例：#EXT-X-KEY:METHOD=AES-128,URI="https://xxx.key",IV=0x123456
    std::regex methodRe(R"(METHOD=([^,]+))");
    std::regex uriRe(R"(URI=\"([^\"]+)\")");
    std::regex ivRe(R"(IV=0x([0-9a-fA-F]+))");

    std::smatch match;
    if (std::regex_search(line, match, methodRe))
        key_ = match[1];
    if (std::regex_search(line, match, uriRe))
        uri_ = match[1];
    if (std::regex_search(line, match, ivRe))
        iv_ = match[1];

    // URI 如果是相对路径则补全
    if (!uri_.empty() && uri_.find("http") != 0) {
        uri_ = baseUrl + "/" + uri_;
    }
}

std::vector<unsigned char> m3u8Downloader::FetchKey() {
    HttpClient client(uri_);
    // 新增重试机制，确保key能正确被获取
    int retry = 3;
    std::string keyStr;
    while(keyStr.empty() && retry-- > 0) {
        keyStr = client.GetHtmlFromUrl();
    }
    return std::vector<unsigned char>(keyStr.begin(), keyStr.end()); // 转二进制
}

std::vector<unsigned char> m3u8Downloader::HexToBytes(const std::string& hex) {
    std::vector<unsigned char> bytes;
    for (size_t i = 2; i < hex.length(); i += 2) { // 去掉前缀 0x
        std::string byteString = hex.substr(i, 2);
        unsigned char byte = (unsigned char) std::stoi(byteString, nullptr, 16);
        bytes.push_back(byte);
    }
    return bytes;
}

// AES-128-CBC 解密单个 TS 文件
bool m3u8Downloader::DecryptTsFile(const std::string& inputFile, const std::string& outputFile) {
    // 仅仅只是打开文件，当开始read的时候才开始读区数据
    std::ifstream ifs(inputFile, std::ios::binary);
    std::ofstream ofs(outputFile, std::ios::binary);
    if (!ifs || !ofs) return false;

    AES_KEY aesKey;
    AES_set_decrypt_key(key.data(), 128, &aesKey);

    std::vector<unsigned char> inbuf(16);
    std::vector<unsigned char> outbuf(16);
    if (iv.empty()) {
        iv = HexToBytes("0x" + iv_);    // 转换为子节序
    }
    std::vector<unsigned char> current_iv = iv;
    while (ifs.read(reinterpret_cast<char*>(inbuf.data()), 16) || ifs.gcount() > 0) {
        size_t bytesRead = ifs.gcount();
        // AES CBC 只能解密满块的部分
        if (bytesRead == 16) {
            AES_cbc_encrypt(inbuf.data(), outbuf.data(), 16, &aesKey, current_iv.data(), AES_DECRYPT);
            ofs.write(reinterpret_cast<char*>(outbuf.data()), 16);
        }
        else {
            // TS 文件不是 AES-PKCS7，剩余不足 16 字节的部分无需解密，直接写原文
            ofs.write(reinterpret_cast<char*>(inbuf.data()), bytesRead);
        }
    }
    return true;
}

bool m3u8Downloader::DecryptAllTs(std::function<void(int)> progressCallBack) {
    if (key.empty()) {
        key = FetchKey();
    }

    decryptedFiles.clear();
    ThreadPool pool(logical_cores >> 1);
    std::vector<std::future<std::string>> futures;

    std::atomic<int> doneCount{0};
    for (size_t i = 0; i < tsFiles.size(); ++i) {
        std::string inputFile = tsFiles[i];
        std::string outputFile = inputFile.substr(0, inputFile.rfind("/") + 1) + "decrypt_" + std::to_string(i) + ".ts";
        decryptedFiles.emplace_back(outputFile);

        futures.emplace_back(pool.enqueue([=, &doneCount]() {
            bool ok = DecryptTsFile(inputFile, outputFile);
            if (!ok) {
                std::cerr << "[Error] Failed to decrypt " << inputFile << std::endl;
            } else {
                //std::cout << "[Info] Decrypted " << inputFile << std::endl;
                doneCount.fetch_add(1);
                if(progressCallBack && doneCount.load() % 5 == 0) {
                    progressCallBack(60 + static_cast<int>((doneCount.load() + 1) * 30.0 / tsFiles.size()));
                }
                if (doneCount.load() == tsFiles.size()) {
                    progressCallBack(90);
                }
            }
            return outputFile;
        }));
    }

    // 等待所有解密完成
    for (auto& f : futures) {
        f.get();
    }

    if (doneCount.load() == tsFiles.size()) {
        return true;
    } else {
        return false;
    }
}

bool m3u8Downloader::MergeToVideo(const std::string& outputFile, std::function<void(int)> progressCallBack, m3u8Downloader::VideoFormat format) {
    std::ofstream ofs(outputFile, std::ios::binary);
    if (!ofs) {
        std::cerr << "[Error] Cannot open output file: " << outputFile << std::endl;
        return false;
    }

    // 按照解密后的顺序合并，避免乱序
    // 合并占50%，转换占50%
    for (const auto& decryptedFile : decryptedFiles) {
        std::ifstream ifs(decryptedFile, std::ios::binary);
        if (!ifs) {
            std::cerr << "[Error] Cannot open decrypted file: " << decryptedFile << std::endl;
            return false;
        }

        // ofs << ifs.rdbuf();  // 直接读区整个文件内存占用较大，按分块（8K）处理
        char buf[8192];
        // read()函数只有在读取满缓冲区后才回返回true，这会导致最后一部分未被写入
        while (ifs) {
            ifs.read(buf, sizeof(buf));
            ofs.write(buf, ifs.gcount());
        }
        ifs.close();
    }
    ofs.close();

    if (format != m3u8Downloader::VideoFormat::TS) {
        progressCallBack(95);
        std::filesystem::path tsPath = outputFile;
        std::filesystem::path transformed = tsPath;
        //replace_extension操作会修改原对象
        transformed.replace_extension(Format2String(format));
        // 使用ffmpeg进行容器转换(允许路径中包含空白字符)
        // 可以使用caffeinate -i 命令来制定执行时避免休眠而中断
        std::string cmd = "ffmpeg -i \"" + outputFile + "\" -c copy \"" + transformed.c_str() + "\"";
        system(cmd.c_str()); // 同步执行
        // 删除默认TS格式
        std::filesystem::remove(tsPath);
    }
    progressCallBack(100);

    return true;
}

// 删除所有中间Ts文件
void m3u8Downloader::DeleteTemplateFile() {
    for(auto item: tsFiles) {
        std::filesystem::remove(item);
    }
    tsFiles.clear();

    for(auto item: decryptedFiles) {
        std::filesystem::remove(item);
    }
    decryptedFiles.clear();
}
