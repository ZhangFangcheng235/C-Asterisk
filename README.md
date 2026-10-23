# C*（C Asterisk）语言工具链 · 0.5.0

一门**语法像 C、关键字用初中英语词汇**的入门编程语言，**Windows 和 Linux 通用**。

源文件后缀 `.cask`，编译链：`.cask` → x86-64 汇编 → `as` → `ld` → 原生可执行文件（Windows 上 `exe`，Linux 上无后缀）。
编译 `.cask` 程序**全程不调用 C 编译器**：链接预编译好的运行时库 `libcastrt.a`（`build.bat` 构建工具链时编好一次），
所以用户机器上只需要 binutils（`as` / `ld`）。

```cask
function main() {
    print "Hello, world!";
}
```

## 0.5.0 新变化

- **新增 `C_IO.cash`**：`readfile / writefile / appendfile / fileexists / makedir /
  appdir / envget / launchwait`，控制台程序也能用，不必开窗口。
- **窗口美化真正落地**：Windows 上按钮改为 **owner-draw** 自绘，`csbg / cstext / csfont`
  对按钮生效；`csbg(窗口, 色)` 换窗口底色；输入框背景刷与文字底色一致；
  `csfont` 按 DPI 把"磅"正确换算成像素（不再出现巨字）。
- **新增完整示例 `examples\mc_launcher.cask`**：自动找 Java、自动定位 jar、
  记住上次配置、拼命令行并启动 Minecraft，演示 C_IO + C_CSS 的组合用法。
- **汇编后端修正**：`TRUE()/FALSE()` 参与运算、`char` 符号扩展、数组初始化超出长度、
  `INT64_MIN % -1` 崩溃、中文函数嵌套调用、`--fast` 下字符自增检查等问题全部修复。
- **教材全面修订到 0.5.0**：小学/初中/普高必修/普高选修/高职五本教材新增
  文件读写与界面美化章节，并改正"先翻译成 C 再交给 gcc"的旧描述。

## 特点（0.5.0）

- **像 C**：大括号、分号、运算符、优先级都与 C 相同，学了 C* 再看 C 几乎无障碍。

- **关键字看得懂**：`print` 打印、`show` 不换行、`input` 输入、`int` 整数、`string` 字符串、

  `function main()` 主程序、`constant` 常量，`if/else/while/for/return` 与 C 相同，

  `and or not` 等于 `&& || !`；还有 `do…while`、`until`、`foreach…in`、`switch/case/default`。

  （老写法 `say` / `start { }` / `func` / `const` / `void`，以及 0.3 的类型名

  `number` / `real` / `text` / `letter` 全部保留为**软弃用别名**，旧程序照样能编译；

  带 `--diag` 检查时会给一条不计错的"提示"建议换新写法。）

- **一行打开窗口世界（0.4.2 新增）**：文件最前面写 `use "C_OPED.cash";`，就打开了

  C / C++ / 原生 GUI——系统头文件已经包含、窗口库自动链接、头文件里的函数**不用声明**就能调，

  另外带一整套内建 GUI：`window` `label` `button` `textbox` `memo` `checkbox` `listbox` `progress`、

  事件 `on_click` / `on_change` / `on_close`、事件循环 `run()`、`alert` / `msgbox` / `ask` / `sleep`、

  还有 `launch(命令行)`——启动一个外部程序（比如 java）并且不等它退出，返回 1 表示启动成功。

  同一份程序 Windows 上生成**窗口式 exe**（Win32，默认没有控制台，调试时加 `--console`），
  Linux 上生成 **GTK3 窗口程序**。C# 没有 C 头文件，不支持。

- **再一行打开 CSS 美化（0.4.2 新增）**：紧跟 `use "C_OPED.cash";` 再写 `use "C_CSS.cash";`，
  就能给窗口控件上 CSS 式的美化：`cstext / csbg`（文字、背景色，写 `"#RRGGBB"` 或颜色名）、
  `csfont`（字号粗细）、`csradius`（圆角）、`csshadow`（投影）、`cspass`（密码打码）、
  `cshide / csshow`（隐藏显示）、`cscolor / csapply`（给样式起个名再涂到别的控件上）。
  颜色写法和 CSS 完全一样；`examples\calc.cask` 是一个窗口计算器。

- **再一行打开文件与系统（0.5.0 新增）**：`use "C_IO.cash";` 打开 `readfile / writefile /
  appendfile`（整文件读 / 写 / 追加）、`fileexists`（在不在）、`makedir`（建目录）、

  `appdir()`（本程序所在目录）、`envget`（环境变量）、`launchwait`（启动外部程序并等它

  跑完拿退出码；`launch()` 则是启动就返回）。控制台程序也能用，不必配 `C_OPED.cash`。

  `examples\mc_launcher.cask`（MC 启动器）就用它把上次填写记在 exe 旁边。

- **两种视图，同一个程序**：C* Studio 里点工具栏「代码 / 积木」随时切换。

  积木视图左挑右拖、点黄色格子改内容、Delete 删除、拖拽换位置（带影子和插入线）——

  像 Scratch 一样上手，但每次操作都会**真正改写代码文本**，代码始终是唯一的真身，两个视图永远不会不一致。

  积木箱还带**头文件门禁**：`窗口程序` 和 `CSS 美化` 两组积木，只有程序里 `use` 了对应头文件才显示；

  没 use 就收着、点它会自动补一行 `use`。`use "C_OPED.cash";` 那条积木旁边会拉一条线、连出一个

  **头文件概述小卡片**，写清这个头是干嘛的（比如 `C_OPED.cash` 是"万能头文件"）。

- **代码按分类上色、悬浮看用法**：代码视图的颜色和积木箱的十类一一对应
  （输出输入 / 变量数据 / 运算 / 判断 / 循环 / 函数 / 字符串与数组 / 窗口程序 / CSS 美化 / 其它），
  **鼠标停在任意一个词上**就会弹出它的写法和用法小卡片（忘了哪个函数怎么用，停一下就行）；
  选中的文字是半透明蓝底，底下的彩色字照样看得清。

- **问题分三级**：**严重**（跑不起来）/ **语法**（写法不对，能一次报多条并自动跳过错处）/

  **拼写**（名字打错了，附"是不是想写 ×××"），另有不计错的**提示**；状态栏四个计数实时显示。

  报错信息全是中文：直接说**第几行、哪里不对、该怎么改**（例如 `S|3|5|这里要写 ';'，它给这条声明收尾，但是碰到了 'print'`）。

- **名字可以用中文**：`int 苹果 = 3;`、`function 打招呼(string 谁) { … }`（文件存为 UTF-8）。

- **会 Excel 就会写**：`IF / AND / OR / MOD / ROUND / SUM / AVERAGE / MAX / MIN /

  LEFT / MID / UPPER / TRIM / CONCAT / VALUE …` 一整套与 Excel **同名同义**的大写函数，

  表格公式能直接搬进程序；三目 `? :` 用 `IF(条件, 真值, 假值)` 代替（见说明书 4.10）。

  **0.4.2 补齐**：`ATAN2` `TRUNC` `LOG10` `FIND` `SUBSTITUTE` `REPT` `EXACT` `CHAR` `CODE`，

  以及 Excel 大写数学名 `SIN` `COS` `TAN` `ASIN` `ACOS` `ATAN` `FLOOR` `CEILING`；

  小写老写法（`abs`、`len`、`sin`…）变成**软弃用别名**，照常能编译，检查时提示换新名。

- **跑得快**：直接生成 x86-64 机器码（不经过 C 编译器），热点循环里整数加减乘、除模、下标访问的
  溢出/越界检查都内联成几条指令，常量以立即数形式出现，热的整型局部变量直接放进
  `rbx`/`r12`-`r15` 寄存器缓存（tcc 式），十亿次空循环约半秒量级；

  编译一个小程序只需 **0.1–0.3 秒**（一次 `as` + 一次 `ld`，比调 gcc 快，生成的 exe 也不包含
  `windows.h` 那堆东西，自动精简）；

  `print` 输出走行缓冲，大量输出时比逐段写快约一倍。

- **不乱算**：整数溢出、除零、数组越界、非法移位都会**立刻停止并指出 `.cask` 的出错行**；

  局部变量自动初始化为 0；忘写 `return` 时补安全的 `return 0`。

- **整洁**：五种基本类型 `int / float / string / char / bool`（外加 `none` 表示无返回值），

  常量在编译期求值，数组维度必须是常量。

- **VS Code 里也能写**：`vscode-ext\` 是 `.cask` 的 VS Code 扩展（语法高亮 + 代码片段）。

## 目录

```
compiler/   caskc.c（编译器）、cask_rt.h（运行时）、C_OPED.cash（窗口程序头，Win32/GTK3 跨平台）、
            C_CSS.cash（美化）、C_IO.cash（文件与系统）、cask_gui.c / cask_io.c（预编译进 libcastrt.a）
editor/     webide.c（C* Studio 宿主：原生窗口 + 本机网页服务，Windows 专用）、wv2.cpp（WebView2 接入）
caskide.py  Linux/macOS 编辑器后端（Python 小服务器，同一套 web/ 页面）
web/        编辑器界面网页（index.html / style.css / editor.js / app.js / blocks.js / syntax.js）
docs/       教材与手册：CStar手册.md / 小学版教程.md / 初中版教程.md /
            普高教材必修一.md / 普高选修一.md / 高职教程CStar语言基础.md /
            md2html.py / check_doc_code.py / *.html / images/
examples/   hello / calc（窗口计算器，C_OPED + CSS）/
            mc_launcher（MC 启动器：拼命令行 + launch() 启动外部程序）
tests/      41 个测试 + 期望输出（含 tests/native/ 窗口程序头测试、io 文件与系统头测试），run_tests.bat 一键回归
vscode-ext/ VS Code 扩展源码（打包产物在 dist/）
dist/       打包好的发行文件：CStar-0.5.0.zip、c-asterisk-0.5.0.vsix
bin/        build.bat / build.sh 生成：caskc、cask_rt.h、C_OPED.cash、web/（Linux 下 caskide.sh）
build.bat   Windows 构建整个工具链     build.sh   Linux/macOS 构建
run_tests.bat Windows 跑全部测试        run_tests.sh Linux/macOS 跑全部测试
```

## 快速开始（Windows）

```bat
:: 1) 构建工具链（需要 PATH 里有 gcc 和 g++）
build.bat

:: 2) 编译并运行一个程序
bin\caskc.exe examples\hello.cask --run

:: 3) 跑回归测试（41 项）
run_tests.bat

:: 4) 打开编辑器（F5 运行 / Esc 停止 / 工具栏「代码 积木」切换 / F1 帮助）
bin\caskide.exe
```

## 快速开始（Linux / macOS）

```bash
# 1) 构建工具链（需要 PATH 里有 gcc；窗口程序还需要 libgtk-3-dev）
#    Ubuntu/Debian:  sudo apt install build-essential pkg-config libgtk-3-dev
./build.sh

# 2) 编译并运行一个程序
./bin/caskc examples/hello.cask --run

# 3) 跑回归测试（41 项）
./run_tests.sh

# 4) 打开编辑器（同一套网页，用 Python 小服务器 + 系统浏览器；需要 python3）
./bin/caskide.sh
```

编辑器在 Linux/macOS 上打开的是**和 Windows 完全相同的网页界面**（代码 / 积木 / 格式化 /
外观 / 运行），只是宿主从 WebView2 原生窗口换成了「本机 Python 服务器 + 浏览器」，
编译、运行、文件读写全部走同一套 API，功能不变。

## 编辑器（C* Studio）

双击 `bin\caskide.exe` 即可（后面跟文件名可直接打开那个程序）。
界面上是 **Visual Studio 2026 深色风格**：菜单栏（文件 / 编辑 / 视图 / 生成 / 调试 / 帮助）、
工具栏、文档标签、停靠的「输出」「问题」工具窗口、底部蓝色状态栏。
界面是一页网页，但装在**真正的原生窗口**里：exe 自带一个小服务（只认 127.0.0.1，不连外网）

把 `bin\web\` 的页面喂给窗口；Windows 缺 WebView2 组件时会提示并改用默认浏览器打开同一界面。

- **菜单栏 + 工具栏**：文件（新建 / 打开 / 保存 / 另存为）、编辑（格式化 / 转成 UTF-8 / 转成 GBK）、

  视图（代码 / 积木 / 输出 / 问题）、生成、调试（运行 F5 / 停止 Esc）、帮助（F1）；

  工具栏上同样有 ▶运行、■停止、代码·积木、格式化、编码 ▾、帮助

- **边写边查**：停顿半秒自动跑一遍 `--check`；「问题」面板列出每条（点一条即跳到出错行），

  行号栏标红出错行，状态栏四个计数：**严重 / 语法 / 拼写 / 提示**

- **运行面板**：程序输出直接显示在下方；`input` 时出现输入框，打字回车喂给程序；

  结束显示 `[结束，退出码 0]`；随时 Esc 停止

- **积木视图**：左侧积木箱 10 类（输出输入 / 变量数据 / 运算 / 判断 / 循环 / 函数 / 字符串与数组 /

  **窗口程序** / **CSS 美化** / 其它），

  其中「窗口程序」「CSS 美化」是头文件插件——程序里 `use` 了对应的 `.cash` 才出现，没 `use` 就不显示；

  注释行画成能点改的绿色积木，`use` 积木拉一条线连到右侧的头文件概述卡片；

  含 `if` `while` `for` `until` `do…while` `foreach` `switch/case/default` 全套积木，

  以及 `use "C_OPED.cash";`、创建窗口 / 按钮 / 文本框、`on_click`、`run()` 等窗口积木；

  点击加入、拖到画布指定位置（影子 + 黄色插入线 + 落位动画）、点黄色格子就地改名字和数值
  （鼠标停在小格子上会说明这一格填什么）、括号里的 **＋** 用来加参数、

  选中按 Delete 删除

- **状态栏**：文件名（改过亮 ●）、编码、当前行列、四类问题计数、版本

- **编码**（编辑菜单，或工具栏「编码 ▾」）：`转成 UTF-8（推荐）` / `转成 GBK（给老环境）` 立刻换编码保存（把 GBK 文件转成

  UTF-8，中文变量名就能被编译器接受）；GBK 和 UTF-8 都能直接打开

- **代码按分类上色**：和积木箱一一对应的十类十色（输出输入亮蓝 / 变量数据绿 / 运算青 /
  判断橙 / 循环紫 / 函数粉红 / 字符串与数组黄绿 / 窗口程序天蓝 / CSS 美化紫 / 其它灰），注释绿色斜体；
  **鼠标停在词上**会弹出它的写法和用法小卡片；选中的文字是半透明蓝底，不挡字；行号栏滚动同步；

  Tab 缩进四格、回车自动对齐空位（上一行有 `{` 自动多缩一层）

## 编译器选项

| 选项 | 作用 |
|------|------|
| `-o 文件名` | 指定输出 exe |
| `--run` | 编译完立刻运行 |
| `--console` | 窗口程序里保留控制台（`print` 的输出看得见） |
| `--emit-c 文件名` | 只写出生成的 C 代码（调试用；正常编译不产生 C，也不调用 C 编译器） |
| `--keep` / `-g` | 保留生成的 `.s` 汇编文件 |
| `--fast` | 关闭溢出/除零/越界检查（更快，出错行为不再保证） |
| `--check` | 只检查不编译（不留下任何文件），有错退出码非 0 |
| `--check --diag` | 再按 `E|行|列|信息`（严重）、`S|…`（语法）、`P|…`（拼写）、`H|…`（提示）逐条输出 |
| `-O0..3` | 保留兼容（优化已内置在汇编后端里） |
| `-v` / `-h` | 显示 as/ld 命令行 / 帮助 |
| `--version` | 显示版本号（0.5.0） |

## 学习与使用

- **第一次学编程**：看 `docs\小学版教程.md`——从"让电脑说话"讲起，每课都有例子和练习。

- **进阶**：`docs\初中版教程.md`——从语法到函数、数组、常用算法，例子最多。

- **普高 / 高职**：`docs\普高教材必修一.md`（必修）、`docs\普高选修一.md`（选修：算法与窗口进阶）、

  `docs\高职教程CStar语言基础.md`（高职）。

- **速查手册**：`docs\CStar手册.md`（浏览器打开 `docs\manual.html`，编辑器里按 `F1`）。

- **网页版**：`python docs\md2html.py` 会把 `docs\` 下的每本 `.md` 都生成同名 `.html`。

- **示例**：`examples\` 每个文件都短小并带注释，适合照着改（`win_hello.cask` 是窗口程序）。

- **测试**：`tests\` 里 `rtcheck_*` 是"应当报运行时错误"的负向测试，

  `cerr_*` 是"编译器应当拒绝"的负向测试，其余比对 `*.expected` 输出。

- **教材里的例子会被真的编译一遍**：`python docs\check_doc_code.py docs\小学版教程.md`
  （窗口程序只编译、不运行，不会弹窗）。

## VS Code 扩展

`vscode-ext\` 是纯声明式的扩展（无需 Node 依赖即可安装 .vsix）：

1. VS Code → `Ctrl+Shift+P` → `Extensions: Install from VSIX...` → 选 `dist\c-asterisk-0.5.0.vsix`
2. 打开任意 `.cask` 文件即高亮；输入 `main` / `print` / `for` 等按 Tab 展开片段

自己重新打包（需要 Node/npm）：

```bat
cd vscode-ext
npx --yes @vscode/vsce package --allow-missing-repository --out ..\dist\c-asterisk-0.5.0.vsix
```

发布到 VS Code 应用市场需要你自己的 publisher 账号与 Azure DevOps PAT：

```bat
npx @vscode/vsce login <publisher>
npx @vscode/vsce publish -p <PAT>
```

## 环境要求

- **Windows 10/11**：**编译 `.cask` 程序**只需要 binutils（`as` / `ld`，MinGW-w64 系自带：
  Dev-C++ / MSYS2 / w64devkit / Strawberry Perl 的都行）外加工具链目录里的 `bin\libcastrt.a` 和
  `bin\libgcc.a`；**构建工具链本身**（`build.bat`）另外需要 gcc 和 g++。
  编辑器需要 WebView2 运行时（Windows 10/11 一般自带；没有会提示并降级用默认浏览器）。

- **Linux / macOS**：编译 `.cask` 需要 `as` / `ld`（binutils）与 glibc 启动文件（`libc6-dev`）；
  窗口程序（`use "C_OPED.cash";`）额外需要 GTK3 开发包（`pkg-config libgtk-3-dev`）。
  构建工具链本身（`build.sh`）需要 gcc（Ubuntu/Debian 装 `build-essential`）。
  编辑器需要 python3（同一套网页 + 本机小服务器）。

- **Python 3**（可选）：用于重新生成网页说明书与检查教程代码（`python docs/md2html.py`、`python docs/check_doc_code.py`）。

## 说明

- C\* 0.5.0 教学原型，接口与实现会随教学反馈调整；
  问题与建议请附上最小复现 `.cask` 文件。
- **彩蛋 `rcl()`**：`rcl` 是方程235 的女朋友，方程235 永远爱 rcl。
  `rcl();` 会替你说一句"方程235永远爱rcl！"然后结束；`rcl(235)`、`rcl("rcl")`、
  `rcl("方程235")` 也各有一句回应；写别的值就当没看见（程序照常往下跑）。
  测试 `tests\rcl.cask` 覆盖这个彩蛋。
