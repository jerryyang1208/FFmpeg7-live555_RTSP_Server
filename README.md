# FFmpeg7 + Live555 RTSP Server

一个基于FFmpeg 7和Live555的高性能RTSP流媒体服务器，支持H.264/H.265视频文件的实时流式传输。

## ✨ 特性

- 🎥 **多格式支持**：支持MP4、FLV、H.264裸流等视频格式
- 🚀 **高性能传输**：优化的缓冲区管理，支持高码率视频流
- 🔧 **自动格式转换**：自动将MP4容器中的H.264/H.265转换为AnnexB格式
- ⏱️ **精准时间同步**：基于PTS的精确时间戳计算，确保播放流畅
- 📡 **多路并发**：支持多个客户端同时连接不同视频流
- 🛡️ **防丢包设计**：增强的Socket缓冲区，减少网络丢包

## 🏗️ 项目结构

<pre>
RTSP_server/
├── rtsp_server.cpp          # 主程序源代码
├── CMakeLists.txt           # CMake构建配置
├── .gitignore               # Git忽略文件
├── build/                   # 编译输出目录
│   ├── RTSP_server          # 可执行文件
│   └── *.mp4/*.h264         # 视频文件（用户放置）
└── README.md                # 项目文档
</pre>


## 📋 环境要求

- **操作系统**：Linux (支持WSL)
- **编译器**：GCC 7+ 或 Clang
- **CMake**：3.10+
- **FFmpeg**：7.0+ (libavcodec, libavformat, libavutil)
- **Live555**：最新版
- **OpenSSL**：(Live555依赖)

## 🔧 安装步骤

### 1. 安装依赖

```bash
# Ubuntu/Debian
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    libavcodec-dev \
    libavformat-dev \
    libavutil-dev \
    libswscale-dev \
    libswresample-dev \
    libssl-dev

# 安装Live555
cd ~/project
git clone https://github.com/xanview/live555/
cd live555
./genMakefiles linux-64bit
make -j$(nproc)
```

### 2. 编译项目

```bash
# 克隆项目
git clone https://github.com/jerryyang1208/FFmpeg7-live555_RTSP_Server.git
cd FFmpeg7-live555_RTSP_Server

# 创建build目录
mkdir build && cd build

# 配置并编译
cmake ..
make -j$(nproc)
```

### 3. 使用方法
```bash
# 进入项目目录
cd ~/project/RTSP_server/build

# 复制视频文件到运行目录
cp /path/to/your/video.mp4 ~/project/RTSP_server/build/

# 运行服务器，默认在8554端口启动RTSP服务
./RTSP_server

# 服务器输出示例
--- FFmpeg + Live555 稳定性强化版 (Anti-Artifacts) ---
[Published] rtsp://192.168.1.100:8554/sample.mp4
[Published] rtsp://192.168.1.100:8554/test.h264

# 另开终端使用FFplay
ffplay rtsp://your-server-ip:8554/video.mp4

# 另开终端使用VLC（命令行）
vlc rtsp://your-server-ip:8554/video.mp4
```
