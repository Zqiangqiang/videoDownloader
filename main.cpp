#include <iostream>

#include "downloader/thread_pool.h"
#include "downloader/http_client.h"
#include "downloader/ffmpeg_downloader.h"
#include "m3u8_downloader.h"

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QFileDialog>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QWidget window;
    window.setWindowTitle("Video Downloader");
    window.resize(400, 150);

    // 主布局
    QVBoxLayout *mainLayout = new QVBoxLayout(&window);

    // Label + 输入框
    QHBoxLayout *inputLayout = new QHBoxLayout();
    QLabel *label = new QLabel("请输入 URL:");
    QLineEdit *urlInput = new QLineEdit();
    inputLayout->addWidget(label);
    inputLayout->addWidget(urlInput);

    // 下载按钮
    QPushButton *downloadBtn = new QPushButton("下载");
    // 显示下载视频的标题
    QLabel *titleLabel = new QLabel("视频标题：");
    titleLabel->setVisible(false);

    // 进度条
    QProgressBar *progress = new QProgressBar();
    progress->setRange(0, 100);   // 0 ~ 100%
    progress->setValue(0);
    progress->setVisible(false);

    // 下载路径提示 + 选择按钮
    QHBoxLayout *pathLayout = new QHBoxLayout();
    QLabel *tipLabel = new QLabel("（默认下载路径为桌面）");
    QPushButton *choosePathBtn = new QPushButton("选择路径");
    pathLayout->addWidget(tipLabel);
    pathLayout->addWidget(choosePathBtn);

    // 加入布局（输入框，下载按钮，标题框，进度条）
    mainLayout->addLayout(inputLayout);
    mainLayout->addWidget(downloadBtn);
    mainLayout->addWidget(titleLabel);
    mainLayout->addWidget(progress);
    mainLayout->addWidget(tipLabel);
    mainLayout->addLayout(pathLayout);

    window.show();

    // 选择路径按钮逻辑
    QObject::connect(choosePathBtn, &QPushButton::clicked, [&]() {
        QString dir = QFileDialog::getExistingDirectory(&window, "选择下载路径",
            QStandardPaths::writableLocation(QStandardPaths::DesktopLocation));
        if (!dir.isEmpty()) {
            tipLabel->setText("下载路径：" + dir);
        }
    });

    // 处理downloadBtn的点击事件
    QObject::connect(downloadBtn, &QPushButton::clicked, [&]() {
        downloadBtn->setVisible(false);   // 隐藏下载按钮
        choosePathBtn->setVisible(false);  // 下载途中禁止修改路径
        titleLabel->setVisible(true);     // 显示标题
        progress->setVisible(true);       // 显示进度条

        // 从输入窗口中获取输入的url
        QString url = urlInput->text();
        QString downloadPath;
        if (tipLabel->text() == "（默认下载路径为桌面）") {
            downloadPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        } else {
            downloadPath = tipLabel->text().split("：")[1];
        }

        // 在QT项目中UI界面处于住进程，不能让住进程阻塞，否则UI界面卡住
        // QtConcurrent异步执行，如果采用&捕获，会造成捕获到已释放对象
        QtConcurrent::run([=]() {
            HttpClient hClient(url.toStdString());
            std::string html = hClient.GetHtmlFromUrl();
            std::string title = HttpClient::ExtractTitle(html);

            // UI 修改需要切回主线程
            QMetaObject::invokeMethod(titleLabel, [titleLabel, title]() {
                titleLabel->setText("视频标题：" + QString::fromStdString(title));
            });

            std::string basePath = downloadPath.toStdString();

            if (html.empty()) {
                std::cerr << "Failed to fetch HTML." << std::endl;
                return 1;
            }

            // 方案二: 通过m3u8文件下载分片后合成完整视频
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

                QMetaObject::invokeMethod(progress, [=]() {
                    progress->setValue(20);
                });

                basePath = basePath.back() == '/' ? basePath : basePath + '/';
                std::string dirPath = title.empty() ? basePath + "video" : basePath + title;

                // 下载所有ts分片
                m3u8_downloader.DownloadAllSegments(dirPath);
                QMetaObject::invokeMethod(progress, [=]() {
                    progress->setValue(60);
                });

                // 将下载好的所有ts分片进行解密
                m3u8_downloader.DecryptAllTs();
                QMetaObject::invokeMethod(progress, [=]() {
                    progress->setValue(90);
                });
                // 将所有分片和并为完整视频，如需转换格式，则需要使用ffmpeg
                bool ret = m3u8_downloader.MergeToVideo(basePath + title + "/" + title + ".ts");
                m3u8_downloader.DeleteTemplateFile();

                if (ret) break;
                QMetaObject::invokeMethod(progress, [=]() {
                    progress->setValue(0);
                });
            }

            // 下载完成后更新 UI（必须用主线程）
            QMetaObject::invokeMethod(progress, [=]() {
                progress->setValue(100);
                downloadBtn->setVisible(true);
                std::this_thread::sleep_for(std::chrono::seconds(2));
                progress->setVisible(false);
            });
        });
    });

    return app.exec();
}


// 方案1:调用ffmpeg 下载到指定目录
//return FFmpegDownloader::download(m3u8Urls.front(), outputPath) ? 0 : -1;
