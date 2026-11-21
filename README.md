# fcitx5-llm-sentence

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
2. **跨窗口历史上下文、人工改错记忆与后台自动摘要压缩**
   - 可选开启「跨窗口记住近期输入」，自动通过 `SurroundingText` 识别用户对上屏结果的修改（如 `「王小明」→「王晓明」`），并在后续请求中采用多轮对话前缀重放（兼容 DeepSeek / OpenAI 前缀缓存）以及后台滚动摘要压缩。
3. **极简轻量**
   - 无需绑定几十 MB 的本地词库，C++ 模块 + GTK4/libadwaita 独立设置界面（`fcitx5-llm-sentence-config`）打包后总大小仅 **约 50 KB**。

## 构建与打包（Arch Linux）

```bash
./scripts/package-arch.sh 0.1.0 dist
sudo pacman -U dist/fcitx5-llm-sentence-0.1.0-1-x86_64.pkg.tar.zst
```

## 打开设置界面

安装后可在应用菜单打开 **「大模型整句输入设置」**，或在终端运行：

```bash
fcitx5-llm-sentence-config
```
