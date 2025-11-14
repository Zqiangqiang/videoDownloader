//
// Created by 翔 on 25-11-13.
//

#ifndef FFMPEG_DOWNLOADER_H
#define FFMPEG_DOWNLOADER_H

#pragma once
#include <string>

class FFmpegDownloader {
public:
    static bool download(const std::string& m3u8Url, const std::string& outputFile);
};

#endif //FFMPEG_DOWNLOADER_H
