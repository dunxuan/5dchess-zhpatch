# 5D Chess 简体中文汉化补丁

《5D Chess With Multiverse Time Travel》的简体中文界面补丁。
游戏文本全部硬编码在 exe 中、无任何本地化系统，本补丁通过 **SDL2 代理 DLL +
运行时挂钩** 完成文本替换，并注入汉字字形实现中文渲染。

```
对战 / 谜题 / 指南 / 设置 / 制作人员 / 退出     ← 主菜单（实机截图效果）
```

## 特性

- **全量翻译**：464 条 UI / 教程 / 关卡名 / FAQ 文本（`translation.tsv`）
- **绿色免安装**：解压覆盖游戏目录即用，可反复覆盖，无注册表、无脚本
- **不修改 exe**：原版 exe 字节不变（SHA256 可校验），卸载 = 删代理 + Steam 校验完整性
- **高 DPI 修复**：125%/150% 缩放下坐标统一，避免输入错位
- 渲染字体：Noto Serif/Sans CJK SC（SIL OFL）

## 安装（玩家）

见 **[安装说明.txt](安装说明.txt)**：解压到游戏目录 → 运行 `安装.bat` → 启动即中文。
脚本会自动把原版 `SDL2.dll` 重命名备份为 `SDL2_orig.dll` 并覆盖字体（备份在
`assets\fonts\orig\`），包内**不附带任何游戏文件**；`还原.bat` 可一键还原。

## 目录结构

```
├── src/dllmain.cpp          补丁核心：SDL2 代理 + 四个函数挂钩 + 翻译/字形注入
├── build.bat                MSVC 构建（生成 out\SDL2.dll）
├── package.ps1              组装发布 zip（安装.bat 负责重命名原版，不附带游戏文件）
├── 安装.bat / 还原.bat       玩家侧一键安装/还原
├── translation.tsv          翻译表（英文<TAB>中文，\n 为换行转义）
├── config.example.ini       运行配置样例（debug / 字形注入阈值）
├── tools/
│   ├── extract_strings.py   从 exe 提取“被界面引用”的字符串 → ui_strings.tsv
│   ├── gen_def.py           生成 SDL2 转发表 src/forward.cpp + 导入检查
│   ├── validate_translation.py  翻译表逐字节校验（结构/转义/缩进）
│   └── ui_strings.tsv       提取产物（校验输入）
├── docs/reverse-engineering.md  逆向笔记：RVA、结构体、坑位
├── THIRD_PARTY.md           第三方版权（游戏/SDL2/Noto）
└── LICENSE                  本仓库代码 MIT
```

## 从源码构建

前置：Visual Studio Build Tools（含 C++ 工具集）、Python 3。

```powershell
# 1) 生成 SDL 转发代码（指向你的游戏目录）
python tools\gen_def.py --game-dir "C:\Path\To\5dchesswithmultiversetimetravel"

# 2) 编译代理 DLL（vcvars 路径可用环境变量 VCVARS 覆盖）
.\build.bat

# 3) 校验翻译表
python tools\validate_translation.py

# 4) 组装发布包（会自动下载 Noto 字体；包内不含游戏文件）
powershell -ExecutionPolicy Bypass -File package.ps1
```

产物 zip 内含：`安装.bat`（重命名原版 SDL2 + 覆盖补丁）、`还原.bat`、
`patch\`（代理 DLL、翻译表+配置、CJK 字体）、`安装说明.txt`。

## 工作原理（简）

1. **代理**：`SDL2.dll` 用链接器 pragma 把 678 个 SDL 导出全部转发给
   `SDL2_orig.dll`（原版字节），进程加载时在 `DllMain` 安装挂钩。
2. **挂钩**（14 字节绝对跳，序言 19 字节）：
   - 文本测量 `0x26640` / 文本绘制 `0xC45D0` → **整体替换**为
     UTF-8 解码 + 主字形数组/extras 双查找的实现（原版不解码 UTF-8，
     中文会花屏）；替换内容按 `translation.tsv` 内容匹配。
   - 字体烘焙 `0x25B20` → 向 extras 注入 ASCII+汉字码点，使字形图集包含中文。
3. **字形容量**：图集上限 2048²，注入集合按
   `count*(size+6)² ≤ 4194304*0.85` 判定，绘制端用同一公式决定是否替换，
   容量不足的大字号样式自动保留英文（不会空白/崩溃）。

细节见 [docs/reverse-engineering.md](docs/reverse-engineering.md)。

## 已知限制

- 大字号样式（81/190/512px，如 "Available Now"、主标题）容量不够，
  保留英文——这是图集物理上限，标题类英文保留也可接受。
- 仅支持 Steam v1.0 exe（见安装说明中的 SHA256）。
- 中文译文由 AI 初译 + 人工校对完成，欢迎提 issue/PR 修正措辞。

## 翻译贡献

编辑 `translation.tsv`（UTF-8，每行 `英文<TAB>中文`，保持行数与转义一致），
然后运行 `python tools/validate_translation.py` 校验，提交 PR 即可。
教程文本里的 `\n` 必须与英文侧数量一致（渲染按它换行）。

## 免责声明

- 需要**合法拥有**游戏副本；本仓库不含任何游戏文件。
- 与 Thunkspace / Conor Petersen 无关联。
- 逆向与挂钩仅供个人学习研究，请勿商用分发。
- 字体与 SDL2 许可见 [THIRD_PARTY.md](THIRD_PARTY.md)。
