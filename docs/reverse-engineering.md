# 逆向笔记（5dchesswithmultiversetimetravel.exe）

目标：x64 PE，imagebase `0x140000000`，引擎为 SDL2 + OpenGL + 自研 UI
（Dear ImGui 仅少量使用），文本**全部硬编码**在二进制中，无本地化系统。

> 以下地址均为 RVA（VA - 0x140000000），针对 Steam v1.0
> （exe SHA256 `087429A0...747B5815`，1,363,456 字节）。exe 不同全部失效。

## 关键 RVA

| RVA | 符号（IDA 自命名） | 说明 |
|---|---|---|
| `0x26640` | sub_140026640 | **文本测量**（509B）：遍历字节累加字距，输出 `[minx,miny,w,h]` |
| `0xC45D0` | sub_1400C45D0 | **文本绘制**：逐字生成四边形（36B/顶点 ×4 + 索引 ×6） |
| `0x25B20` | sub_140025B20 | **字体样式烘焙**（2.8KB）：主 95 字形 + extras 列表 + 图集打包 |
| `0x25A40` | — | 字体文件加载（fopen 整读 → stb_truetype，支持 CFF/OTF，**不支持 TTC**） |
| `0x25070` | — | stbtt_InitFont 等价物（table directory 解析） |
| `0x22550` | — | 单字形光栅化 + 图集装箱 |
| `0x25780` | — | 位于字体加载器之前的辅助函数（内含 exit(1)） |
| `0xE3D78` | j__malloc_base | **游戏静态 CRT 堆**（代理里分配游戏缓冲必须用它） |
| `0xE6A64` | j__realloc_base | 同上 |
| `0x89CB0` | — | 主循环（SDL_PollEvent 轮询 + 分发） |
| `0x13ABB0` | __imp_SDL_PollEvent | IAT 槽 |

detour：三个目标函数序言相同（8 个 push + `sub rsp, imm32` = 19 字节），
补丁 = 14 字节 `FF 25 00 00 00 00 + abs64`（**地址在指令后 p+6，不是 p+5**）。

## 文本样式（style）结构

烘焙器写入、测量/绘制读取，176B（0xB0）：

```
+0   u32 base_codepoint (=32)      +4   u32 main_count (=95)
+8   Glyph[95]  (28B/条)           +16  u32 extra_count
+20  u32 extra_cap                 +24  Glyph[] extras (32B/条)
+40  u32 atlas_w                   +44  u32 atlas_h
+48  f32 pixel_size                +52/+56/+60  f32 行距度量
```

### Glyph 记录（主数组 28B / extras 32B）

```
+0  u16 atlas_x0   +2  u16 atlas_y0   +4  u16 atlas_x1   +6  u16 atlas_y1
+8  f32 bearing_x  +12 f32 bearing_y  +16 f32 advance
+20 f32 width      +24 f32 height
extras 额外：+28 u16 codepoint   （步长32，勿用 28 字节指针算术！）
```

## 字形烘焙（sub_140025B20）

- 参数：`(out, font, pixel_size, sample_text, extra_u16*, extra_count)`
- 主数组：对 sample_text 中 ASCII(32..126) 逐字置 flag → 烘焙 95 槽
- extras：flag 之外追加任意 BMP 码点（UTF-16 数组）
- **图集重试上限 2048×2048**；extras 循环遇首个烘焙失败（字体无该字形）
  即 **break**，后续全部留空 —— 注入列表必须只含字体实有码点
- 容量模型（补丁内 inject_allowed 与绘制端必须同一公式）：
  `count * (size+6)^2 <= 4194304 * 0.85`

## 原版文本管线的缺陷（补丁重写的原因）

1. 测量/绘制均为 `index = (u8_byte - base) * 28` **只读主数组**，
   不解码 UTF-8、不查 extras → 中文高位字节产生负索引 → 花屏
2. 绘制循环按 **字节** 发射四边形（strlen 预留也是字节）
3. 单条调用还存在 `(start, end)` 分段测量语义（控件分词排版）

补丁对两函数做了整体替换实现（UTF-8 解码 + 主数组/extras 双查找）。

## 输入事件（SDL2）

主循环 `0x89CB0` 内轮询；SDL_Event 关键偏移（含 `which` 的布局）：

- `type @0`（QUIT=0x100, WINDOW=0x200, KEYDOWN=0x300, MOTION=0x400, BTN=0x401/402）
- 按钮：`button @16, x @20, y @24`；运动：`x @20, y @24`
- 处理：MOTION 写光标 `cursor = (x * drawable_w/win_w, drawable_h - y*drawable_h/win_h)`
  （GL 底部原点 + HiDPI 缩放）；BTN_DOWN 置 `leftDown @ RVA 0x147FC8`

## DPI

游戏窗口尺寸（逻辑）与 drawable（物理）在 125% 缩放下不一致，
补丁在 DllMain 最早调用 `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)`
统一为物理坐标。

## 其它坑

- 代理必须用**游戏的堆**分配它要释放的缓冲（`j__malloc_base/realloc`）
- `exit(1)` 调用点：`0x25B16`（字体加载失败）、`0x2662C`（烘焙图集失败，位于
  baker 函数体内）等 —— 字体缺字形最坏会导致 UAF/退出，务必保证码点可烘焙
- MSVC 的 `.def` 不识别 `a=b.c` 转发语法，转发用
  `#pragma comment(linker, "/export:name=SDL2_orig.name")` 生成
- 系统级“找不到入口点”错误框归 csrss 所有，低权限进程点不掉（UIPI）
