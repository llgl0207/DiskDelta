# DiskDelta — NTFS MFT 扫描与差异比较工具

![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B%2017-green)
![UI](https://img.shields.io/badge/ui-Web-darkgreen)

作者寄语：
> 直接读取 NTFS 卷的 `$MFT`（主文件表），快速扫描盘符下所有文件/目录元数据，支持双快照差异比较。
> 此项目使用DeepSeek V4 Flash编写，也能有很多bug，欢迎反馈。另外由于基于MFT，仅支持 NTFS 分区。

- 可以下载Release，也可以自己编译。
---

## ✨ 核心特性

- 🚀 **直接读取 MFT** — 绕过 `FindFirstFile`/`FindNextFile`，性能提升 10~100 倍
- 📸 **快照管理** — 扫描结果保存为 JSON 快照，支持历史回溯
- 🔀 **差异比较** — 对比两次快照，变化量排序，识别新增/删除路径
- 🌳 **可展开目录树** — 父子层级可视化，递归计算目录大小与变化量
- 📋 **一键复制路径** — 定位到文件/文件夹后点击 📋 复制完整路径
- 📊 **CSV 导出** — 导出对比结果
- 🔒 **本地安全** — HTTP 服务仅监听 `127.0.0.1`

---

## 🖥 界面预览

```
┌─────────────────────────────────────────────────────────────┐
│ 📁 Windows                   32.5 GB                        │
│ ├ 📁 Program Files           15.2 GB    +2.1 GB     增大    │
│ │ ├ 📁 Common Files           8.5 GB    +1.2 GB     增大    │
│ │ └ 📁 Windows Defender       1.2 GB     0  B       不变    │
│ ├ 📁 Users                    8.7 GB    -0.5 GB     减小    │
│ │ └ 📁 llgl0207               6.2 GB    -0.3 GB     减小    │
│ └ 📁 System32                 1.5 GB    +0.1 GB     增大    │
└─────────────────────────────────────────────────────────────┘
```

---

## 🏗 项目结构

```
DiskDelta/
├── CMakeLists.txt            # CMake 构建配置
├── README.md
├── src/
│   ├── main.cpp              # 入口 + HTTP 服务 + REST API
│   ├── mft_reader.h/.cpp     # MFT 直接读取引擎
│   ├── diff_engine.h/.cpp    # 差异比较引擎
│   └── http_server.h/.cpp    # 嵌入式 HTTP 服务器 (Winsock)
├── web/
│   └── index.html            # 前端 Web UI（CSS + JS 内嵌）
├── data/                     # 扫描快照目录（运行时生成）
└── scripts/
    └── build_mingw.bat        # MinGW 一键编译脚本
```

---

## 🔧 编译

### 环境要求

- **MinGW-w64** (g++ 12+) — 可从 [MSYS2](https://www.msys2.org/) 安装
- **Windows 10/11** 64 位

### 一键编译

```bash
scripts\build_mingw.bat
```

### 手动编译

```bash
# 1. 编译所有源文件
g++ -std=c++17 -O2 -I src -D_WIN32_WINNT=0x0A00 ^
    -c src/main.cpp -o build_mingw/main.o ^
    -c src/mft_reader.cpp -o build_mingw/mft_reader.o ^
    -c src/diff_engine.cpp -o build_mingw/diff_engine.o ^
    -c src/http_server.cpp -o build_mingw/http_server.o

# 2. 链接
g++ -std=c++17 -O2 build_mingw/*.o ^
    -o build_mingw/DiskDelta.exe ^
    -lws2_32 -lshell32 -static

# 3. 复制前端文件
xcopy /E /Y /I web build_mingw\web\
```

> 💡 输出文件：`build_mingw\DiskDelta.exe`（约 3.5 MB，静态链接无外部依赖）

---

## 🚀 使用

### 启动

```bash
# ⚠ 必须「以管理员身份运行」
build_mingw\DiskDelta.exe
```

启动后自动弹出浏览器 → **http://127.0.0.1:45678/**

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `--port` | `45678` | HTTP 端口 |
| `--datadir` | `data` | 快照存储目录 |
| `--webdir` | `web` | 前端文件目录 |
| `--help` | — | 显示帮助 |

### 工作流程

```
1. 选择盘符 → 点击「开始扫描」
   └── 控制台显示进度，等待扫描完成

2. 快照管理
   ├── 勾选 1 个 → 点击「📂 浏览快照」
   │    └── 展开目录树，按大小排序查看
   └── 勾选 2 个 → 点击「🔄 开始对比」
        └── 对比两轮扫描，按变化量排序

3. 点击 📋 复制路径 → 粘贴到资源管理器处理
4. 点击 📥 导出 CSV → 下载对比结果
```

---

## 📡 REST API

| 方法 | 路径 | 说明 |
|------|------|------|
| `GET` | `/api/status` | 服务健康检查 |
| `GET` | `/api/snapshots` | 获取快照列表 |
| `GET` | `/api/snapshot/<idx>?sort=size` | 快照详情（支持按大小排序） |
| `POST` | `/api/scan` | 开始扫描 `{"drive":"C"}` |
| `POST` | `/api/diff` | 差异比较 `{"snapshot1":0,"snapshot2":1}` |
| `GET` | `/api/export/0/1` | 导出 CSV |
| `GET` | `/api/open?path=\Users\...` | 在资源管理器中打开路径 |

---

## 🧬 技术架构

```
┌─────────────────────────────────────────────┐
│              浏览器 (Web UI)                  │
│       HTML + CSS + JavaScript               │
└──────────────────┬──────────────────────────┘
                   │ HTTP REST API (JSON)
                   │ http://127.0.0.1:45678
┌──────────────────▼──────────────────────────┐
│           C++ 后端 (DiskDelta.exe)           │
│  ┌──────────┐  ┌──────────┐  ┌───────────┐ │
│  │HTTP Server│  │MFT Reader│  │Diff Engine│ │
│  │ (Winsock) │  │(Raw MFT) │  │(Two-pass) │ │
│  └──────────┘  └────┬─────┘  └─────┬─────┘ │
│                     │               │       │
│              ┌──────▼───────┐       │       │
│              │JSON Snapshots│       │       │
│              │ (data/*.json)│       │       │
│              └──────────────┘       │       │
└─────────────────────────────────────────────┘
```

### MFT 读取原理

1. **打开卷设备** — `\\.\C:` 管理员权限
2. **获取 MFT 参数** — `FSCTL_GET_NTFS_VOLUME_DATA`（Windows 官方 API）
3. **大块读取** — 8MB 批量读取 `$MFT` 文件
4. **解析记录** — 提取 `$FILE_NAME`（文件名/父目录/时间戳）和 `$DATA`（大小）
5. **路径树重建** — 通过父目录 Record ID 构建完整路径，递归计算目录大小
6. **快照存储** — 序列化为 JSON

---

## ❓ FAQ

**Q: 为什么必须管理员运行？**
A: 直接打开 `\\.\C:` 卷设备需要 `SE_BACKUP_NAME` 权限，只有管理员拥有。

**Q: 能扫描网络驱动器吗？**
A: 不能。仅对本地 NTFS 卷有效。

**Q: 回收站（`$Recycle.Bin`）为什么不在扫描结果中？**
A: `$Recycle.Bin` 的 MFT 记录使用特殊标志位（POSIX 命名空间），是 NTFS 设计特性，非 Bug。

**Q: 扫描速度如何？**
A: 大块读取模式下，100 万文件约 30~60 秒（取决于磁盘速度）。

**Q: 端口被占用怎么办？**
A: 程序会**自动杀掉旧进程**释放端口。也可用 `--port 8080` 指定其他端口。

---

## 📄 License

MIT
