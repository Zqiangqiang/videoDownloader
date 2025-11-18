#include <iostream>

#include "downloader/thread_pool.h"
#include "downloader/http_client.h"
#include "downloader/ffmpeg_downloader.h"
#include "m3u8_downloader.h"

#include <QApplication>
#include <QtConcurrent>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>
#include <QFileDialog>
#include <QDesktopServices>
#include <QUrl>
#include <QPlainTextEdit>
#include <QStringList>
#include <QFontMetrics>

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QWidget window;
    window.setWindowTitle("Video Downloader");
    window.resize(500, 150);

    // 主布局
    QVBoxLayout *mainLayout = new QVBoxLayout(&window);

    // Label + 输入框
    QHBoxLayout *inputLayout = new QHBoxLayout();
    QLabel *label = new QLabel("请输入 URLS:");
    QPlainTextEdit *urlInput = new QPlainTextEdit();
    // 设置输入框高度为两行
    QFontMetrics fm(urlInput->font());
    int height = fm.lineSpacing() * 2 + 6;
    urlInput->setFixedHeight(height);
    // 将组件添加到横向栏中
    inputLayout->addWidget(label);
    inputLayout->addWidget(urlInput);

    // 下载按钮
    QPushButton *downloadBtn = new QPushButton("下载");
    // 显示下载视频的标题
    QLabel *titleLabel = new QLabel("当前视频标题：");
    titleLabel->setVisible(false);
    // 允许自动换行
    titleLabel->setWordWrap(false);
    // 超出 QLabel 的部分自动隐藏，不换行
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    // 设置 QLabel 的固定高度为一行字体高度
    QFontMetrics font(titleLabel->font());
    titleLabel->setFixedHeight(font.height());

    // 进度条
    QProgressBar *progress = new QProgressBar();
    progress->setRange(0, 100);   // 0 ~ 100%
    progress->setValue(0);
    progress->setVisible(false);
    // 百分比文字
    QLabel *percentLabel = new QLabel("0%");
    percentLabel->setVisible(false);
    // 水平布局：进度条 + 百分比文本
    QHBoxLayout *progressLayout = new QHBoxLayout();
    progressLayout->addWidget(progress);
    progressLayout->addWidget(percentLabel);

    // 多任务时总进度（单个任务时不显示）
    QProgressBar* totalProgress = new QProgressBar();
    totalProgress->setRange(0, 100);
    totalProgress->setValue(0);
    totalProgress->setVisible(false);
    QLabel* totalPercentLabel = new QLabel("0%");
    totalPercentLabel->setVisible(false);
    QHBoxLayout* totalProgressLayout = new QHBoxLayout();
    totalProgressLayout->addWidget(totalProgress);
    totalProgressLayout->addWidget(totalPercentLabel);

    // 下载路径提示 + 选择按钮
    QHBoxLayout *pathLayout = new QHBoxLayout();
    QLabel *tipLabel = new QLabel("（默认下载路径为桌面）");
    QPushButton *choosePathBtn = new QPushButton("选择路径");
    pathLayout->addWidget(tipLabel);
    pathLayout->addWidget(choosePathBtn);

    // 打开文件夹按钮 (与上述两个按钮放在同一布局中)
    QPushButton *openFolderBtn = new QPushButton("打开文件夹");
    pathLayout->addWidget(openFolderBtn);

    // 加入布局（输入框，下载按钮，标题框，进度条, 路径选择按钮）
    mainLayout->addLayout(inputLayout);
    mainLayout->addWidget(downloadBtn);
    // 在 URL 输入框和标题之间加 10 像素空白
    mainLayout->addSpacing(10);
    mainLayout->addWidget(titleLabel);
    mainLayout->addLayout(progressLayout);
    mainLayout->addLayout(totalProgressLayout);
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

    // 点击按钮打开下载目录
    QObject::connect(openFolderBtn, &QPushButton::clicked, [=]() {
        QString path = tipLabel->text();
        if (path.contains("下载路径：")) {
            // 如果 tipLabel 文字中包含 "下载路径：" 前缀，需要去掉
            path = path.replace("下载路径：", "");
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        } else if(path.contains("默认下载路径为桌面")) {
            QDesktopServices::openUrl((QUrl::fromLocalFile(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation))));
        }
    });

    // 处理downloadBtn的点击事件
    QObject::connect(downloadBtn, &QPushButton::clicked, [&]() {
        downloadBtn->setVisible(false);   // 隐藏下载按钮
        choosePathBtn->setVisible(false);  // 下载途中禁止修改路径
        titleLabel->setVisible(true);     // 显示标题
        progress->setVisible(true);       // 显示进度条
        percentLabel->setVisible(true);   // 显示百分比

        QString urlsInput = urlInput->toPlainText();
        if (urlsInput.isEmpty()) {
            titleLabel->setText("请输入URl...");
            downloadBtn->setVisible(true);
            choosePathBtn->setVisible(true);
            progress->setVisible(false);
            percentLabel->setVisible(false);
        }
        // 新增支持批量处理链接
        QStringList urlList; // 保存所有输入的url
        for (QString line : urlsInput.split('\n', Qt::SkipEmptyParts)) {
            line = line.trimmed();
            if (!line.isEmpty())
                urlList << line;
        }

        // 把currentTasks放在堆上，避免在异步更新时过早释放
        auto currentTasks = std::make_shared<std::atomic<int>>(0);
        const int totalTasks = urlList.size();
        // 多任务时显示总任务进度条
        if (totalTasks > 1) {
            totalProgress->setVisible(true);
            totalPercentLabel->setVisible(true);
            int initialProgress = 100.0 / totalTasks < 5.0 ? 100.0 / totalTasks : 5;
            totalProgress->setValue(initialProgress);
            totalPercentLabel->setText(QString::number(initialProgress) + "%");
        }

        // 设置下载路径
        QString downloadPath;
        if (tipLabel->text() == "（默认下载路径为桌面）") {
            downloadPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        } else {
            downloadPath = tipLabel->text().split("：")[1];
        }

        for (auto& url : urlList) {
            // 在QT项目中UI界面处于住进程，不能让住进程阻塞，否则UI界面卡住
            // QtConcurrent异步执行，如果采用&捕获，会造成捕获到已释放对象
            QtConcurrent::run([=]() {
                HttpClient hClient(url.toStdString());
                std::string html = hClient.GetHtmlFromUrl();
                if (html.empty()) {
                    // 重新在请求一次
                    html = hClient.GetHtmlFromUrl();
                }
                std::string title = HttpClient::ExtractTitle(html);
                std::cout << "[Title] " << title << std::endl;

                // UI 修改需要切回主线程
                QMetaObject::invokeMethod(titleLabel, [titleLabel, title]() {
                    titleLabel->setText("当前视频标题：" + QString::fromStdString(title));
                    // 鼠标悬浮显示完整标题
                    titleLabel->setToolTip(QString::fromStdString(title));
                });

                std::string basePath = downloadPath.toStdString();

                if (html.empty()) {
                    std::cerr << "Failed to fetch HTML." << std::endl;
                    return 1;
                }

                // 方案二: 通过m3u8文件下载分片后合成完整视频
                std::cout << "[QConcurrent] Extracting m3u8 links..." << std::endl;
                auto m3u8Urls = hClient.ExtractLinkOfM3U8(html);

                if (m3u8Urls.empty()) {
                    std::cerr << "[Parse m3u8] No m3u8 links found." << std::endl;
                    return -1;
                } else {
                    std::cout << "[Parse m3u8] Found m3u8 URLs:\n";
                    for (const auto& url : m3u8Urls)
                        std::cout << " - " << url << std::endl;
                }

                // 进度回调函数（回调当前视频进度）
                std::function<void(int)> updateProgress = [&](int value){
                    QMetaObject::invokeMethod(progress, [=](){
                        progress->setValue(value);
                        percentLabel->setText(QString::number(value) + "%");
                        titleLabel->setText("当前视频标题：" + QString::fromStdString(title));
                    });
                };

                // 解析m3u8文件占比20%，下载所有分片占比40%，合并所有分片占比30%，格式转换占比10%
                for(auto item: m3u8Urls) {
                    bool success = true;
                    // 解析m3u8文件
                    m3u8Downloader m3u8_downloader(item);
                    updateProgress(10);
                    m3u8_downloader.parseM3U8();
                    //m3u8_downloader.printInfo();
                    updateProgress(20);
                    // 目录名为video的证明title没有获取到
                    basePath = basePath.back() == '/' ? basePath : basePath + '/';
                    std::string dirPath = title.empty() ? basePath + "video" : basePath + title;

                    // 下载所有ts分片
                    success = m3u8_downloader.DownloadAllSegments(dirPath, updateProgress);
                    if (!success) {
                        //这里可以做重新下载的操作
                        std::cerr << "当前线路失效，选择其他线路" << std::endl;
                        updateProgress(0);
                        continue;
                    }

                    // 将下载好的所有ts分片进行解密
                    success = m3u8_downloader.DecryptAllTs(updateProgress);
                    if(!success) {
                        std::cerr << "Decrypt TS failed" << std::endl;
                        continue;
                    }

                    // 将所有分片和并为完整视频，如需转换格式，则需要使用ffmpeg
                    // 默认格式是将合并后的TS转换为MP4，如有需要可传参
                    success = m3u8_downloader.MergeToVideo(basePath + title + "/" + title + ".ts", updateProgress, m3u8Downloader::VideoFormat::MP4);
                    m3u8_downloader.DeleteTemplateFile();

                    if (success) break;
                    updateProgress(0); // 下载失败进度归零
                }

                // 下载完成后，更新总进度
                currentTasks->fetch_add(1);
                // 当前任务下载完成后更新 UI（必须用主线程以确保线程安全）
                QMetaObject::invokeMethod(totalProgress, [=]() {
                    titleLabel->setText("下载完成：" + titleLabel->text().split("：").back());
                    totalProgress->setValue(static_cast<int>(currentTasks->load() * 100.0 / totalTasks));
                    totalPercentLabel->setText(QString::number(static_cast<int>(currentTasks->load() * 100.0 / totalTasks)) + "%");
                    // 所有任务下载结束后隐藏进度条
                    if (currentTasks->load() == totalTasks) {
                        progress->setValue(0);
                        progress->setVisible(false);
                        percentLabel->setVisible(false);
                        percentLabel->setText(QString::number(0) + "%");
                        totalProgress->setVisible(false);
                        totalProgress->setValue(0);
                        totalPercentLabel->setVisible(false);
                        totalPercentLabel->setText(QString::number(0) + "%");
                        // 修改titleLabel
                        titleLabel->setText("所有任务下载完成");
                        downloadBtn->setVisible(true);
                        choosePathBtn->setVisible(true);
                        // 清空输入框中内容
                        urlInput->setPlainText("");
                    }
                });
            });
        }
    });

    return app.exec();
}


// 方案1:调用ffmpeg 下载到指定目录
//return FFmpegDownloader::download(m3u8Urls.front(), outputPath) ? 0 : -1;
