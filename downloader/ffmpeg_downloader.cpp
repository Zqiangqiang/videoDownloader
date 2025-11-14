//
// Created by 翔 on 25-11-13.
//

#include "ffmpeg_downloader.h"
#include <cstdlib>
#include <iostream>
#include <sstream>

bool FFmpegDownloader::download(const std::string& m3u8Url, const std::string& outputFile)
{
    std::ostringstream cmd;
    cmd << "ffmpeg -y -i \"" << m3u8Url
        << "\" -c copy -bsf:a aac_adtstoasc \"" << outputFile << "\"";

    std::cout << "[INFO] Executing: " << cmd.str() << std::endl;

    int ret = std::system(cmd.str().c_str());
    if (ret != 0)
    {
        std::cerr << "[ERROR] ffmpeg execution failed with code: " << ret << std::endl;
        return false;
    }

    std::cout << "[SUCCESS] Download completed: " << outputFile << std::endl;
    return true;
}