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
#include <qhash.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// 获取设备的逻辑核心数
// 在I/O密集型操作中可以分配更多的虚拟核心，而在cpu计算密集型中不要超过物理核心数
const unsigned int logical_cores = std::thread::hardware_concurrency();
// 文件操作锁
static std::mutex fileMutex;

// 使用mmap技术读文件
std::vector<unsigned char> mmapReadFile(const std::string& filePath) {
    int fd = open(filePath.c_str(), O_RDONLY);
    if (fd == -1) {
        perror("open");
        return {};
    }

    // 获取文件大小
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        return {};
    }
    size_t fileSize = st.st_size;

    // 映射文件到内存
    void* mapped = mmap(nullptr, fileSize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return {};
    }

    // 拷贝到 vector
    unsigned char* start = static_cast<unsigned char*>(mapped);
    std::vector<unsigned char> buffer(start, start + fileSize);

    // 解除映射
    if (munmap(mapped, fileSize) == -1) {
        perror("munmap");
    }

    close(fd);
    return buffer;
}

std::string sha256(const std::vector<unsigned char>& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(data.data(), data.size(), hash);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        out << std::setw(2) << (int)hash[i];
    }
    return out.str();
}

size_t WriteFileCallback(void* ptr, size_t size, size_t nmemb, void* stream) {
    FILE* fp = (FILE*)stream;
    return fwrite(ptr, size, nmemb, fp);
}

// 确保每片ts文件都能被正确下载，否则在合并时会造成合并结果无法播放
bool m3u8Downloader::DownloadTsSegment(const std::string& url, const std::filesystem::path& outputPath) {
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
        std::cerr << "[Segment] Download failed: " << outputPath
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
bool m3u8Downloader::DownloadAllSegments(const std::filesystem::path& dirPath, std::function<void(int)> progressCallBack) {
    // 提前获取key，确保下载完成后能直接解析
    if (key.empty()) {
        key = FetchKey();
    }

    if (TsLinks.empty()) {
        std::cerr << "[Download] No TS segments to download!" << std::endl;
        return false;
    }

    ThreadPool pool(logical_cores);
    std::filesystem::create_directories(dirPath);
    std::vector<std::future<void>> results;

    std::cout << "[Download] Start downloading " << TsLinks.size() << " TS files..." << std::endl;

    // 初始进度（当前项目占比50%）
    std::atomic<int> doneCount = 0;
    std::atomic<bool> repeat(false);
    // atomic不支持std::string
    std::array<std::string, 3> before3Hashes;
    std::mutex hashMutex;
    for (size_t i = 0; i < TsLinks.size(); ++i) {
        std::string tsUrl = TsLinks[i];
        std::filesystem::path temp = dirPath;
        std::string outputFile = temp.append("segment_" + std::to_string(i) + ".ts");
        tsFiles.emplace_back(outputFile);

        results.emplace_back(pool.enqueue([=, &doneCount, &before3Hashes, &hashMutex, &repeat]() {
            if (repeat.load(std::memory_order_acquire)) {
                // 如果已经确定有重复，直接跳过后续分片的下载
                return;
            }

            int count = 0; bool success = true;
            success = this->DownloadTsSegment(tsUrl, outputFile);
            // 新增重试机制，确保能正确下载每一片分片
            while(!success && count++ < 5) {
                std::cout << "[Download] retry " << std::to_string(count) << " times file: " << outputFile << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                if (repeat.load(std::memory_order_acquire)) return;
                success = this->DownloadTsSegment(tsUrl, outputFile);
                if (repeat.load(std::memory_order_acquire)) return;
            }

            // 5次重试后仍失败，直接退出下载
            // TODO: 后续可以将下载失败的片段作出标记，然后在重试时至下载失败片段
            if (success) {
                // 通过前3片Ts文件混合计算hash来进行文件去重
                if (i < 3) {
                    // 使用mmap读文件减小内存开销
                    std::string h = sha256(mmapReadFile(outputFile));
                    std::lock_guard<std::mutex> locker(hashMutex);
                    before3Hashes[i] = h;
                }

                {
                    std::unique_lock<std::mutex> locker(hashMutex);
                    if (!before3Hashes[0].empty() && !before3Hashes[1].empty() && !before3Hashes[2].empty()) {
                        locker.unlock();

                        // 仅保留一个线程计算Fingerprint，其余线程直接退出
                        if (repeat.load(std::memory_order_acquire)) return;

                        // 计算最终指纹
                        std::string combined;
                        combined.reserve(64 * 3);

                        locker.lock();
                        for (auto& hash: before3Hashes) {
                            combined.append(hash);
                        }
                        locker.unlock();

                        std::string Fingerprint = sha256(std::vector<unsigned char>(combined.begin(), combined.end()));

                        // 典型模式：发布者 / 订阅者
                        if (!repeat.load(std::memory_order_acquire)) {
                            std::unique_lock<std::mutex> mapLocker(mapMutex);
                            auto it = videoHashMap.find(Fingerprint);
                            mapLocker.unlock();

                            if (it != videoHashMap.end()) {
                                // 将目录名更长的更新到已下载目录上
                                std::filesystem::path exitPath =  it->second;
                                if (exitPath != dirPath) {
                                    std::string exitDirName = exitPath.filename();
                                    std::string currDirName = dirPath.filename();
                                    if (exitDirName.size() >= currDirName.size()) {
                                        // 通知其他线程repeat更新情况
                                        repeat.store(true, std::memory_order_release);
                                        isRepeat = true;
                                        progressCallBack(60);
                                    } else {
                                        std::unique_lock<std::mutex> fileLocker(fileMutex);
                                        std::filesystem::rename(exitPath, dirPath);
                                        fileLocker.unlock();

                                        std::filesystem::path originFileName;
                                        for (auto enty: std::filesystem::directory_iterator(exitPath)) {
                                            if (enty.is_regular_file()) {
                                                originFileName = enty.path();
                                                break;
                                            }
                                        }
                                        std::filesystem::path temp = exitPath;
                                        std::filesystem::path newFileName = temp.append(currDirName);

                                        fileLocker.lock();
                                        std::filesystem::rename(originFileName, newFileName);
                                        fileLocker.unlock();
                                    }
                                    return ;
                                } else {
                                    // 避免同一任务中的不同线程误认为自己是重复视频
                                    // 此处什么也不做直接返回
                                }
                            } else {
                                std::unique_lock<std::mutex> mapLocker(mapMutex);
                                videoHashMap.insert({Fingerprint, dirPath});
                                mapLocker.unlock();
                            }
                        }
                    }
                }

                // 不要直接使用整数除法否则会造成值为0即进度不走的情况
                // 不要使用序号算进度，因为是并发执行，会导致进度条伸缩
                // 不要频繁的回调进度否则会造成很大的性能开销
                doneCount.fetch_add(1, std::memory_order_relaxed);
                if(progressCallBack && doneCount.load() % 5 == 0) {
                    progressCallBack(20 + static_cast<int>((doneCount.load() + 1) * 40.0 / TsLinks.size()));
                }
            } else {
                std::cerr << "[Download] " << std::to_string(i) << " TS failed path: " << outputFile << std::endl;
                std::filesystem::remove(outputFile);
            }
        }));
    }

    // 等待所有任务完成
    for (auto& f : results) {
        f.get();
    }

    if (doneCount.load() == TsLinks.size() && !repeat.load(std::memory_order_acquire)) {
        // 下载完成后及时释放TsLinks，减少内存占用
        TsLinks.clear();
        std::cout << "[Download] All TS segments downloaded. "  << dirPath << std::endl;
        return true;
    } else if (repeat.load(std::memory_order_acquire)) {
        std::filesystem::remove_all(dirPath);
        std::cout << "[RepeatVideo] Remove repeated video " << dirPath << std::endl;
        return true;
    } else {
        return false;
    }
}

bool m3u8Downloader::parseM3U8() {
    HttpClient client(m3u8Link);
    std::string content = client.GetHtmlFromUrl();

    if (content.empty()) {
        std::cerr << "[ParseM3U8] Failed to download m3u8 file: " << m3u8Link << std::endl;
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
            TsLinks.emplace_back(tsUrl);
        }
    }

    return !TsLinks.empty();
}

void m3u8Downloader::printInfo () const {
    std::cout << "[PrintInfo] METHOD: " << key_ << std::endl;
    std::cout << "[PrintInfo] URI: " << uri_ << std::endl;
    std::cout << "[PrintInfo] IV: " << iv_ << std::endl;
    std::cout << "[PrintInfo] Total TS files: " << TsLinks.size() << std::endl;
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
bool m3u8Downloader::DecryptTsFile(const std::filesystem::path& inputFile, const std::filesystem::path& outputFile) {
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
        std::filesystem::path inputPath(tsFiles[i]);
        // 不要使用字符串拼接，直接使用std::fileSystem::path
        std::string outputFile = inputPath.parent_path().append("decrypt_" + std::to_string(i) + ".ts");
        decryptedFiles.emplace_back(outputFile);

        futures.emplace_back(pool.enqueue([=, &doneCount]() {
            bool ok = DecryptTsFile(inputPath.c_str(), outputFile);

            int count = 0;
            while (!ok && count++ < 3) {
                ok = DecryptTsFile(inputPath.c_str(), outputFile);
                std::cerr << "[Decrypt] Retry " << std::to_string(count) << " times decrypt " << inputPath << std::endl;
            }

            if (!ok) {
                std::cerr << "[Decrypt] Failed to decrypt " << inputPath << std::endl;
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

bool m3u8Downloader::MergeToVideo(const std::filesystem::path& outputFile, std::function<void(int)> progressCallBack, m3u8Downloader::VideoFormat format) {
    std::ofstream ofs(outputFile, std::ios::binary);
    if (!ofs) {
        std::cerr << "[Merge] Cannot open output file: " << outputFile << std::endl;
        return false;
    }

    // 按照解密后的顺序合并，避免乱序
    // 合并占50%，转换占50%
    for (const auto& decryptedFile : decryptedFiles) {
        std::ifstream ifs(decryptedFile, std::ios::binary);
        if (!ifs) {
            std::cerr << "[Merge] Cannot open decrypted file: " << decryptedFile << std::endl;
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
        std::filesystem::remove(transformed);
        // 使用ffmpeg进行容器转换(允许路径中包含空白字符)
        // 可以使用caffeinate -i 命令来制定执行时避免休眠而中断
        char outputpath[2048] = {0};
        snprintf(outputpath, sizeof(outputpath), "ffmpeg -y -i \"%s\" -c copy \"%s\"", outputFile.c_str(), transformed.c_str());
        std::cout << "[FFmpeg] " << outputpath << std::endl;
        system(outputpath); // 同步执行
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

    for(auto item: decryptedFiles) {
        std::filesystem::remove(item);
    }
}
