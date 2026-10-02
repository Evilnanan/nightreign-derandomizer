# Nightreign Derandomizer

[English](README.md) | **中文**

这个工程包含以下 Windows x64 产物：

- `derandomizer.dll`：mod 本体。
- `Seed Reroller.exe`：查找/预测种子，并生成 `derandomizer.ini` 的配套工具。
- `derandomizer.ini`：mod 配置文件，各项位于 `[settings]` 节：

| 配置项 | 说明 |
| --- | --- |
| `nightlord` | 夜王 ID（`0`–`9`，对应关系见 ini 注释）；`-1` 表示不固定夜王。 |
| `everdark` | `0` 固定普通形态，`1` 固定永夜形态；`-1` 表示不固定形态。 |
| `seed` | 指定种子（32 位整数，支持十进制或十六进制）；`-1` 表示不固定种子。 |
| `log` | `0` 关闭日志，`1` 开启日志。 |

配置支持热重载：修改并保存 `derandomizer.ini` 后自动生效，无需重启游戏。

## 工程结构

```text
src/
├── app/                 # Seed Reroller
│   ├── main.cpp         # Win32 界面与程序入口
│   ├── cli.*            # 命令行解析与输出
│   ├── model.*          # 种子 / pattern 预测模型
│   └── config.*         # mod 配置文件生成
└── mod/
    ├── derandomizer.cpp      # DLL 入口、状态观察与运行时调度
    ├── boss_hooks.*          # Nightlord / Everdark hook
    ├── seed_hook.*           # 种子注入与快速字段校正
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
├── derandomizer.ini
└── Seed Reroller.exe
```

Debug 构建可使用 `cmake --build --preset debug`，产物位于
`build/dist/Debug/`。

## 命令行调用

不带参数运行时仍会打开图形界面。命令行提供两个子命令：

```powershell
# 为 pattern 搜索一个种子
& '.\Seed Reroller.exe' find 1042

# 根据种子和 Nightlord 预测 pattern
& '.\Seed Reroller.exe' predict 0x066BB95B --nightlord 7
```

两者都支持以下选项：

```text
--mode deep|normal                 模式，缺省为 deep
--variant auto|normal|everdark     写入配置的形态，缺省为 auto
--config <path>                    同时生成指定的 derandomizer.ini
--json                             以单行 JSON 输出结果
```

例如，为 pattern 1042 搜索种子并直接生成配置：

```powershell
& '.\Seed Reroller.exe' find 1042 --variant everdark --config .\derandomizer.ini
```

相对的 `--config` 路径以当前工作目录为基准。完整帮助可通过
`& '.\Seed Reroller.exe' --help` 查看；参数错误时进程返回非零退出码。
