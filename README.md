# fcitx5-yuntype（云打）

Fcitx 5 后台常驻的大模型整句拼音转换模块（`Category=Module`）。**不占用输入法名额**，无论你当前使用的是英文键盘（`keyboard-us`）、原版 `WowType`、`Rime` 还是官方拼音，都可以随时使用。

## 核心特性

1. **两种无缝触发方式（共用同一个快捷键，默认 `Alt+U`）**
   - **方式一（推荐）：先打拼音 → 一键退格替换（终端与 GUI 通用）**
     - 直接在终端或任意输入框里敲一整句拼音（如 `ni mingtian you shijian ma`，或者在命令行引号内 `git commit -m "xiufu zhengju shuru`），敲完按一次 `Alt+U`：
     - 模块会自动计算刚才连续输入的拼音长度 $N$，给终端/应用连发 $N$ 个 `BackSpace` 删掉拼音，并在原地替换为大模型转换好的中文整句（如 `你明天有时间吗？`）。
     - 若在前台中文输入法的预编辑区（Preedit）已经敲了一串拼音尚未上屏，按一次 `Alt+U` 也会直接清空前台预编辑并交由大模型整句转换上屏。
     - 若在支持 `SurroundingText` 的 GUI 应用中用鼠标/键盘选中了一段拼音，按 `Alt+U` 会直接将选区原地替换为转换结果。
   - **方式二：先按 `Alt+U` 进入独立整句草稿框**
     - 当光标前没有刚打的拼音时，按 `Alt+U` 进入下划线整句草稿框，输入整句拼音后按 `回车` 或再按 `Alt+U` 提交转换；等待期间可连续输入下一句，按顺序自动上屏；按 `Esc` 可取消或还原。
2. **短输入多候选（原生 Fcitx 5 候选框皮肤）**
   - 当提交的拼音长度（去首尾空格）不超过设定阈值（界面可配置 `0–200`，默认 `50`，`0` 为关闭）时，自动切换至专用短输入候选提示词并通过 Fcitx 5 原生 `CommonCandidateList` 弹出候选框（样式完全跟随当前 Fcitx 5 皮肤）；短输入请求不附带大段历史正文与多轮记录，普通短打不写入正文历史，仅在原地修正上一句或选择非首选项时记录「历史修正」，让长句多轮前缀缓存保持纯净高效。
   - 支持配置「候选框单页显示上限」（`2–10`，默认 `5`，满 10 项时第 10 项用 `0` 选词），首屏直接按单页条数生成以保证极速响应；支持数字键/鼠标点选、`Space`/`Enter` 确认、`-`/`=` 与 `PageUp`/`PageDown` 翻页、方向键跨页选词；**翻到末页再按 `=` 或任意页按 `+`** 可通过临时多轮续问（复用本次 KV 前缀缓存、不污染历史）继续让模型补充下一页新候选；按 `Esc` 取消并还原拼音、`BackSpace` 退回草稿框、直接继续输入自动上屏高亮项。
3. **跨窗口历史上下文、人工改错记忆、自动落盘与后台摘要压缩**
   - 可选开启「跨窗口记住近期输入」，在 GUI 应用中自动通过 `SurroundingText`、在终端等不支持 `SurroundingText` 的应用中自动通过「影子缓冲区 + 虚拟光标」识别用户对上屏结果的修改（包括退格/方向键改字、选中覆盖，以及退格后再次 `Alt+U` 替换，如 `「王小明」→「王晓明」`），在后续请求中采用多轮对话前缀重放（兼容 DeepSeek / OpenAI 前缀缓存）以及后台滚动摘要压缩。
   - 采用「脏标记 + 可配置闲置落盘冷却（默认 30 秒）+ 最大强制落盘时长（默认 300 秒）+ 退出 `save()` 兜底」策略自动原子落盘至 `~/.local/share/fcitx5-yuntype/history.json`（`0600` 权限），重启 `fcitx5` 后自动恢复；点击设置页「清空」可同步清除内存与磁盘记录。
4. **多模型配置、思考模式控制与一键切换**
   - 设置界面支持保存多套大模型配置（各自独立名称、接口地址、API Key、模型名、思考模式/自定义额外请求 JSON 与备注，以 `0600` 权限保存于 `~/.config/fcitx5-yuntype/models.json`），支持点击条目回填编辑、实时测试、保存写回以及一键点击「使用」切换生效；新建模型默认不传思考控制参数，可按需选择常见关闭思考预设或自定义 JSON 合并至请求体顶层。
5. **极简轻量**
   - 无需绑定几十 MB 的本地词库，C++ 模块 + GTK4/libadwaita 独立设置界面（`fcitx5-yuntype-config`）打包后体积轻巧。

## 构建与打包（Arch Linux）

```bash
./scripts/package-arch.sh 0.1.16-1 dist
sudo pacman -U dist/fcitx5-yuntype-0.1.16-1-x86_64.pkg.tar.zst
```

## 构建与运行（Windows 10 / 11 伴随工具壳）

本仓库采用跨平台同仓分层结构：`core/` 为 Linux / Windows 共用 C++20 核心，`windows/` 为零第三方运行库依赖的单文件伴随工具（WinHTTP 流式 SSE + 全局钩子 + 4 级 UIA/MSAA/Win32 光标跟随 + 贴边无焦点候选框 + 本地设置页）。

在 Windows 上使用 Visual Studio 2022（含 C++ 桌面开发工作负载）和 CMake 编译：

```powershell
cmake -S . -B build -A x64 -DBUILD_TESTING=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

编译完成后直接运行 `build\Release\yuntype.exe`（绿色单文件，配置文件位于 `%APPDATA%\yuntype\`，历史位于 `%LOCALAPPDATA%\yuntype\history.json`）；也可直接通过 `.github/workflows/windows-build.yml` 在 GitHub Actions 自动产出 `yuntype-windows-x64.zip`。

## 打开设置界面

安装后可在应用菜单打开 **「云打（YunType）整句输入设置」**，或在终端运行：

```bash
fcitx5-yuntype-config
```
