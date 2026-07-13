# DiskDelta - NTFS MFT 扫描与差异比较工具

![License](https://img.shields.io/badge/license-MIT-blue)
![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B%2017-green)

## 📋 概述

**DiskDelta** 是一款高性能 Windows 桌面工具，能够直接读取 NTFS 卷的 **$MFT（主文件表）**，快速扫描指定盘符下所有文件/目录的元数据，并支持两轮扫描的差异比较。适用于磁盘使用分析、变更追踪等场景。

### 核心特性

- 🚀 **直接读取 MFT** — 绕过文件系统遍历 API，性能提升 10-100 倍
- 📸 **快照管理** — 将扫描结果保存为 JSON 快照，支持历史回溯
- 🔀 **差异比较** — 对比两次快照，按变化量排序，识别新增/删除路径
- 🖥 **现代 Web UI** — 基于 HTML/CSS/JS 的深色主题界面
- 📊 **数据导出** — 支持导出对比结果为 CSV 文件
- 🔒 **安全设计** — 通过 HTTP REST API 与后端通信，仅监听本地回环地址

## 🏗 项目结构

```
DiskDelta/
├── CMakeLists.txt           # CMake 构建配置
├── README.md                # 本文档
├── src/
│   ├── main.cpp             # 入口点 + HTTP 服务 + REST API
│   ├── mft_reader.h         # MFT 读取引擎头文件
│   ├── mft_reader.cpp       # MFT 直接读取实现
│   ├── diff_engine.h        # 差异比较引擎头文件
│   ├── diff_engine.cpp      # 差异比较实现
│   ├── http_server.h        # 嵌入式 HTTP 服务器头文件
│   └── http_server.cpp      # HTTP 服务器实现
├── web/
│   └── index.html           # 前端 Web UI（含完整 CSS 和 JS）
├── data/                    # 扫描快照存储目录（运行时生成）
└── scripts/
    ├── build.bat            # CMake 构建脚本
    └── build_simple.bat     # 简易 MSVC 编译脚本
```

## 🛠 编译方法

### 前置条件

- **Windows 10/11** 64 位
- **Visual Studio 2022**（或 2019）含 C++ 桌面开发组件
- **CMake** 3.16+（可选，用于 CMake 构建方式）

### 方式一：简易 MSVC 编译（推荐）

1. 打开 **"Developer Command Prompt for VS 2022"**
2. 进入项目目录：
   ```cmd
   cd e:\EE\DiskDelta
   ```
3. 运行编译脚本：
   ```cmd
   scripts\build_simple.bat
   ```
4. 编译产物在 `build\DiskDelta.exe`

### 方式二：CMake 构建

1. 打开 **"Developer Command Prompt for VS 2022"**
2. 进入项目目录：
   ```cmd
   cd e:\EE\DiskDelta
   ```
3. 运行 CMake 构建脚本：
   ```cmd
   scripts\build.bat
   ```
4. 或手动执行：
   ```cmd
   mkdir build && cd build
   cmake .. -G "Visual Studio 17 2022" -A x64
   cmake --build . --config Release
   ```

## 🚀 运行方式

### 1. 以管理员身份运行（必须）

> **⚠️ 重要**：MFT 直接读取需要管理员权限（UAC）。请务必以管理员身份运行。

```cmd
# 方法一：右键 → "以管理员身份运行"
build\DiskDelta.exe

# 方法二：通过 runas
runas /user:Administrator "build\DiskDelta.exe"
```

### 2. 启动参数

```cmd
DiskDelta.exe [options]

Options:
  --port <number>     HTTP 服务端口（默认: 45678）
  --datadir <path>    快照存储目录（默认: data）
  --webdir <path>     Web UI 目录（默认: web）
  --help              显示帮助信息
```

### 3. 访问界面

启动后自动打开浏览器：**http://127.0.0.1:45678/**

### 4. 使用流程

```
1. 在左侧选择盘符（如 C:）
2. 点击「开始扫描」— 等待扫描完成（大型盘符可能需要几分钟）
3. 扫描完成后自动生成快照，显示在快照列表中
4. 勾选两个快照，点击「开始对比」
5. 查看差异结果表格，支持排序、筛选、搜索
6. 点击「导出 CSV」下载对比结果
```

## 🧬 技术架构

```
┌─────────────────────────────────────────────────┐
│                 浏览器 (Web UI)                   │
│         HTML + CSS + JavaScript                 │
└──────────────────┬──────────────────────────────┘
                   │ HTTP REST API (JSON)
                   │ http://127.0.0.1:45678
┌──────────────────▼──────────────────────────────┐
│           C++ 后端 (DiskDelta.exe)               │
│  ┌──────────┐  ┌──────────┐  ┌──────────────┐  │
│  │HTTP Server│  │MFT Reader│  │ Diff Engine  │  │
│  │ (Winsock) │  │(Raw MFT) │  │(Comparison)  │  │
│  └──────────┘  └────┬─────┘  └──────┬───────┘  │
│                     │               │          │
│              ┌──────▼───────┐       │          │
│              │JSON Snapshots│       │          │
│              │  (data/*.json)       │          │
│              └──────────────┘       │          │
└─────────────────────────────────────────────────┘
```

## 📡 API 文档

后端提供以下 REST API：

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/api/status` | 服务状态检查 |
| GET | `/api/snapshots` | 获取快照列表 |
| GET | `/api/snapshot/<index>` | 获取指定快照详情 |
| POST | `/api/scan` | 开始扫描（body: `{"drive":"C"}`） |
| POST | `/api/diff` | 差异比较（body: `{"snapshot1":0,"snapshot2":1}`） |
| GET | `/api/export/<idx1>/<idx2>` | 导出 CSV（返回 CSV 文件） |
| GET | `/api/lastscan` | 获取最近一次扫描结果 |

## 🔬 MFT 读取原理

本工具直接读取 NTFS 卷的 **$MFT** 文件，核心技术点：

1. **打开卷设备**：使用 `\\.\C:` 语法以原始模式打开卷
2. **解析引导扇区**：读取偏移 0x28 处获取 $MFT 起始簇号（LCN）
3. **读取 MFT 记录**：定位到每条 MFT 记录（通常 1024 字节）
4. **USA 修复**：应用 Update Sequence Array 修复记录完整性
5. **属性解析**：
   - `$FILE_NAME` 属性（0x30）：提取文件名、父目录引用、时间戳
   - `$DATA` 属性（0x80）：提取文件大小
6. **路径树重建**：通过父目录引用构建完整路径，递归计算目录大小
7. **快照存储**：将结果序列化为 JSON 格式保存

### 性能特性

- 单线程扫描 100 万文件约需 30-120 秒（取决于磁盘速度）
- 内存占用约 200-500MB（100 万文件规模）
- 全程无文件系统遍历，绕过 `FindFirstFile`/`FindNextFile`

## ❓ 常见问题

**Q: 为什么扫描速度很慢？**
A: 首次扫描时程序需要读取整个 $MFT，机械硬盘可能较慢。SSD 通常 30-60 秒可扫描 50 万文件。

**Q: 为什么不使用 FindFirstFile/Walk 方式？**
A: 直接读取 MFT 性能更高（10-100 倍），且能捕获所有文件元数据，包括系统保护文件和隐藏文件。

**Q: 能扫描网络驱动器吗？**
A: 不支持。本工具仅对本地 NTFS 卷有效。

**Q: 文件路径乱码怎么办？**
A: MFT 中的文件名使用 UTF-16LE 编码，程序已正确处理。如果控制台显示乱码，请使用 `chcp 65001` 切换到 UTF-8。

## 📄 许可证

MIT License

## 🙏 致谢

- Microsoft NTFS 官方文档
- 开源 NTFS 解析相关项目参考资料
