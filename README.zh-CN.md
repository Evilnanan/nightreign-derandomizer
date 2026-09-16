# Nightreign Derandomizer

[English](README.md) | **中文**

这个工程包含两个 Windows x64 产物：

- `derandomizer.dll`：由 mod loader 加载的游戏 mod。
- `Seed Reroller.exe`：查找/预测 seed，并生成 `derandomizer.ini` 的配套程序。

## 工程结构

```text
src/
├── app/                 # Seed Reroller
│   ├── main.cpp         # Win32 界面与程序入口
│   ├── cli.*            # 命令行解析与输出
│   ├── model.*          # seed / pattern 预测模型
│   └── config.*         # mod 配置文件生成
└── mod/
    ├── derandomizer.cpp      # DLL 入口、状态观察与运行时调度
    ├── boss_hooks.*          # Nightlord / Everdark hook
    ├── seed_hook.*           # seed 注入与快速字段校正
    ├── range_diagnostics.*   # 可选的 Range() 逆向诊断
    ├── game_module.*         # PE 版本识别与特征扫描
    ├── memory_patch.*        # 内存补丁与 stub 分配
    ├── config.*              # derandomizer.ini 读取和校验
    └── logging.*             # 运行日志
```

## 构建

需要 Visual Studio 2026 的“使用 C++ 的桌面开发”组件（v145 工具集），以及
CMake 4.2 或更高版本（VS 2026 生成器要求）：

```powershell
cmake --preset vs2026-x64
cmake --build --preset release
```

Release 产物会放在同一个目录：

```text
build/dist/Release/
├── derandomizer.dll
└── Seed Reroller.exe
```

Debug 构建可使用 `cmake --build --preset debug`，产物位于
`build/dist/Debug/`。

## 命令行调用

不带参数运行时仍会打开图形界面。命令行提供两个子命令：

```powershell
# 为 pattern 搜索一个 seed
& '.\Seed Reroller.exe' find 1042

# 根据 seed 和 Nightlord 预测 pattern
& '.\Seed Reroller.exe' predict 0x066BB95B --nightlord 7
```

两者都支持以下选项：

```text
--mode deep|normal                 模式，缺省为 deep
--variant auto|normal|everdark     写入配置的形态，缺省为 auto
--config <path>                    同时生成指定的 derandomizer.ini
--json                             以单行 JSON 输出结果
```

例如，为 pattern 1042 搜索 seed 并直接生成配置：

```powershell
& '.\Seed Reroller.exe' find 1042 --variant everdark --config .\derandomizer.ini
```

相对的 `--config` 路径以当前工作目录为基准。完整帮助可通过
`& '.\Seed Reroller.exe' --help` 查看；参数错误时进程返回非零退出码。
