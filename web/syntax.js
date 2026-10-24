/* syntax.js -- C* 语法小抄（数据 + 查表）
 *
 * 一份数据两处用：
 *   · 代码视图 -- 按分类上色 + 鼠标停在词上显示用法（editor.js）
 *   · 积木视图 -- 鼠标停在小格子上说明这一格填什么（blocks.js）
 *
 * 分类沿用积木箱那十类：out 输出输入 / var 变量数据 / math 运算 / if 判断 /
 * loop 循环 / fn 函数 / text 字符串与数组 / win 窗口程序 / misc 其它。
 */
var SYNTAX = (function () {

  var CAT = {
    out: 't-out', var: 't-var', math: 't-math', if: 't-if', loop: 't-loop',
    fn: 't-fn', text: 't-text', win: 't-win', css: 't-css',
    io: 't-io', os: 't-os', net: 't-net', sc: 't-sc',
    misc: 't-misc'
  };

  /* ---------------- 关键字 ---------------- */
  var KW = {};                       /* 名字 -> {cat, sig, note} */

  function k(cat, sig, note, names) {
    names.split(' ').forEach(function (n) {
      KW[n] = { cat: cat, sig: sig, note: note };
    });
  }

  k('out', 'print 值1, 值2 …;', '打印这些值，打印完换行。（say 是它的别名）', 'print say');
  k('out', 'show 值1, 值2 …;', '打印这些值，不换行。', 'show');
  k('out', 'input 变量;', '等用户输入一行，把读到的内容放进这个变量。别忘了最后的分号。', 'input');

  k('var', 'int 变量 = 0;', '整数类型。声明变量：类型 名字 = 初始值;（不给初始值就是 0）', 'int');
  k('var', 'float 变量 = 0.0;', '小数类型。', 'float');
  k('var', 'string 变量 = "";', '文字类型，值用双引号包起来。', 'string');
  k('var', "char 变量 = 'a';", '单个字符，值用单引号包起来。', 'char');
  k('var', 'bool 变量 = true;', '真假类型，只有 true 和 false 两个值。', 'bool');
  k('var', 'none 函数名() { }', '写在函数名前面，表示这个函数没有返回值（也可以写 function）。', 'none void');
  k('var', 'int 变量 = 0;', '旧的类型名，还能用但会有提示：现在写 int 更清楚。', 'number');
  k('var', 'float 变量 = 0.0;', '旧的类型名，还能用但会有提示：现在写 float 更清楚。', 'real');
  k('var', 'string 变量 = "";', '旧的类型名，还能用但会有提示：现在写 string 更清楚。', 'text');
  k('var', 'char 变量 = \'a\';', '旧的类型名，还能用但会有提示：现在写 char 更清楚。', 'letter');
  k('var', 'constant 名字 = 值;', '常量：值定下来就不能改，适合放不会变的数据。', 'constant const');

  k('if', 'if (条件) { … } else { … }', '条件成立走 if 的 { }，不成立走 else 的 { }（else 可以不要）。', 'if else');
  k('if', 'switch (值) { case 1: … default: … }', '多路分支：挨个比 case，都不像就走 default；每个 case 后面记得 break;', 'switch case default');
  k('if', 'true / false', '真假两个字面值。', 'true false');
  k('if', 'a and b / a or b / not a', '逻辑运算：与、或、非（也可以写 && || !）。', 'and or not');

  k('loop', 'while (条件) { … }', '条件成立就一直转圈，每圈开头检查一次条件。', 'while');
  k('loop', 'for (int i = 0; i < 10; i++) { … }', '计数循环：起点、条件、每圈结束做什么，三段用分号隔开。', 'for');
  k('loop', 'until (条件) { … }', '条件成立就停（和 while 相反）。', 'until');
  k('loop', 'do { … } while (条件);', '先做一遍，再判断要不要继续。', 'do');
  k('loop', 'foreach (名字 in 数组) { … }', '把数组里的元素一个一个拿出来。注意括号里只写名字，不要写类型。', 'foreach in');
  k('loop', 'break;', '跳出当前这个循环。（结束整个程序要另外想办法）', 'break');
  k('loop', 'continue;', '这一圈不做了，直接开始下一圈。', 'continue');

  k('fn', 'int 函数名(参数) { return 值; }', '定义函数：返回值类型写在函数名前面，return 把结果交回去。', 'function func');
  k('fn', 'start { … }', '程序入口，从这里开始跑（`function main()` 也行）。', 'start');
  k('fn', 'return 值;', '结束当前函数并把值交回调用它的地方；没有返回值就直接 return;', 'return');

  k('win', 'use "C_OPED.cash";', '放在程序最上面，引入窗口程序头：之后就能直接写 window、button、label 这些，编译器会自动链接窗口库。再写 use "C_IO.cash"; 还能读写文件、取程序目录、launchwait 等外部程序。', 'use');

  /* ---------------- 内置函数 ---------------- */
  var BI = {};                       /* 大写名 -> {cat, sig, note} */

  function b(cat, sig, note, names) {
    names.split(' ').forEach(function (n) {
      BI[n] = { cat: cat, sig: sig, note: note };
    });
  }

  /* Excel 风格（大写）与 C 风格（小写）两套名字，编译器都认，这里都收 */
  b('math', 'ABS(x)', '绝对值。', 'ABS');
  b('math', 'SIGN(x)', 'x 是正数给 1，负数给 -1，0 给 0。', 'SIGN');
  b('math', 'MOD(a, b)', '取余数：结果的符号跟着除数（Excel 的规矩），MOD(-3, 10) 是 7。', 'MOD');
  b('math', 'ROUND(x) / ROUND(x, 位数)', '四舍五入；写了位数就保留那么多位小数。', 'ROUND');
  b('math', 'INT(x)', '向下取整：INT(-2.7) 是 -3。', 'INT');
  b('math', 'SQRT(x)', '平方根。', 'SQRT');
  b('math', 'POWER(x, y)', 'x 的 y 次方。', 'POWER');
  b('math', 'EXP(x)', 'e 的 x 次方。', 'EXP');
  b('math', 'LN(x)', '自然对数（底是 e）。', 'LN');
  b('math', 'LOG(x) / LOG(x, 底)', '对数；不写底就是常用对数（底 10）。', 'LOG');
  b('math', 'PI()', '圆周率 3.14159…。', 'PI');
  b('math', 'RAND()', '0 到 1 之间的随机小数。', 'RAND');
  b('math', 'RANDBETWEEN(a, b)', 'a 到 b 之间的随机整数（两头都算）。', 'RANDBETWEEN');
  b('math', 'SUM(数组)', '把数组里的数加起来。', 'SUM');
  b('math', 'AVERAGE(数组)', '平均数（给出来是小数）。', 'AVERAGE');
  b('math', 'COUNT(数组)', '数组里有几个元素。', 'COUNT');
  b('math', 'MAX(a, b, …) / MAX(数组)', '最大的那个；MIN 是最小的。', 'MAX MIN');
  b('math', 'SIN(弧度)', '正弦（参数是弧度）。', 'SIN');
  b('math', 'COS(弧度)', '余弦（参数是弧度）。', 'COS');
  b('math', 'TAN(弧度)', '正切（参数是弧度）。', 'TAN');
  b('math', 'ASIN(x) / ACOS(x) / ATAN(x)', '反三角函数，返回弧度。', 'ASIN ACOS ATAN');
  b('math', 'LOG10(x)', '常用对数（底 10）。', 'LOG10');
  b('math', 'FLOOR(x)', '向下取整（和 floor 一样）。', 'FLOOR');
  b('math', 'CEILING(x)', '向上取整（和 ceil 一样）。', 'CEILING');
  b('math', 'TRUNC(x)', '截掉小数部分（向 0 取整）。', 'TRUNC');
  b('math', 'ATAN2(y, x)', '由 y、x 求角度（弧度），比如让角色朝一个方向走。', 'ATAN2');

  b('text', 'LEN(文字)', '文字长度。按字节算，一个汉字算 3。', 'LEN');
  b('text', 'LEFT(文字, 几个) / RIGHT(文字, 几个)', '从左边 / 右边取几个字符。', 'LEFT RIGHT');
  b('text', 'MID(文字, 第几位, 几个)', '从第几位开始取几个字符，位置从 1 开始。', 'MID');
  b('text', 'UPPER(文字) / LOWER(文字)', '全变大写 / 全变小写（对英文有效）。', 'UPPER LOWER');
  b('text', 'TRIM(文字)', '去掉两头的空格，中间连着的好几个空格并成一个。', 'TRIM');
  b('text', 'CONCAT(a, b, …)', '把几段文字接成一段（也可以用 + 号）。', 'CONCAT');
  b('text', 'VALUE(文字)', '把文字转成整数；开头不是数字就算 0。', 'VALUE');
  b('text', 'VAL(文字)', '把文字转成整数（VAL 是旧写法，现在写 VALUE）。', 'VAL');
  b('text', 'FIND(文字, 要找的, 从第几位开始)', '找到返回位置（从 1 数），找不到返回 0；区分大小写。', 'FIND');
  b('text', 'SUBSTITUTE(文字, 旧, 新)', '把文字里的“旧”全部换成“新”。', 'SUBSTITUTE');
  b('text', 'REPT(文字, 次数)', '把文字重复几遍接起来。', 'REPT');
  b('text', 'EXACT(a, b)', '两段文字完全一样吗（区分大小写）：一样是 true。', 'EXACT');
  b('text', 'CHAR(编号)', '编号（0..255）对应的字符。', 'CHAR');
  b('text', 'CODE(文字)', '第一个字符的编号（0..255）。', 'CODE');

  b('if', 'IF(条件, 成立时的值, 不成立时的值)', '二选一，代替别的语言里的 ? :；只会算用到的那一边。', 'IF');
  b('if', 'AND(a, b, …) / OR(a, b, …)', '都成立 / 有一个成立（也可以是 and、or）。', 'AND OR');
  b('if', 'NOT(x)', '反过来（条件必须是比较，比如 x > 3）。', 'NOT');
  b('if', 'TRUE() / FALSE()', '真假两个字面值。', 'TRUE FALSE');

  /* 小写是原来的名字（软弃用：还能用，但提示换大写的规范写法） */
  b('math', 'abs(x)', '绝对值（旧写法，现在写 ABS）。', 'abs');
  b('math', 'sqrt(x)', '平方根（旧写法，现在写 SQRT）。', 'sqrt');
  b('math', 'pow(x, y)', 'x 的 y 次方（旧写法，现在写 POWER）。', 'pow');
  b('math', 'sin(x) cos(x) tan(x)', '三角函数，参数是弧度（旧写法，现在写 SIN/COS/TAN）。', 'sin cos tan');
  b('math', 'asin(x) acos(x) atan(x)', '反三角函数（弧度）（旧写法，现在写 ASIN/ACOS/ATAN）。', 'asin acos atan');
  b('math', 'log(x)', '自然对数（旧写法。注意：log 是自然对数；常用对数请写 LOG，自然对数请写 LN）。', 'log');
  b('math', 'log10(x)', '常用对数（底 10）（旧写法，现在写 LOG10）。', 'log10');
  b('math', 'exp(x)', 'e 的 x 次方（旧写法，现在写 EXP）。', 'exp');
  b('math', 'floor(x) / ceil(x)', '向下取整 / 向上取整（旧写法，现在写 FLOOR/CEILING）。', 'floor ceil');
  b('math', 'round(x)', '四舍五入（旧写法，现在写 ROUND）。', 'round');
  b('math', 'random()', '0 到 1 之间的随机小数（旧写法，现在写 RAND）。', 'random');
  b('math', 'now()', '现在的时间戳，单位秒。', 'now');

  b('text', 'len(文字)', '文字长度（旧写法，现在写 LEN）。', 'len');
  b('text', 'at(文字, 第几位)', '取第几位上的字符，位置从 1 开始。', 'at');

  /* 彩蛋 */
  b('misc', 'rcl() / rcl(235) / rcl("rcl")',
    '彩蛋：rcl 是方程235的女朋友。rcl() 会替你表白；rcl(235)、rcl("rcl")、rcl("方程235") 也都有回应。',
    'rcl');

  /* ---------------- 窗口程序（C_OPED.cash） ---------------- */
  var GUI = {};                      /* 小写名 -> {cat, sig, note} */

  function g(sig, note, names) {
    names.split(' ').forEach(function (n) { GUI[n] = { cat: 'win', sig: sig, note: note }; });
  }

  g('window("标题", 宽, 高)', '开一个窗口，返回窗口号。', 'window');
  g('label("文字", x, y, 宽, 高)', '放一个标签（只显示、不能改），返回控件号。', 'label');
  g('button("文字", x, y, 宽, 高)', '放一个按钮，点它就会调用 on_click(控件号)。', 'button');
  g('textbox("初始文字", x, y, 宽, 高)', '单行输入框，读内容用 readtext(控件号)。', 'textbox');
  g('memo("初始文字", x, y, 宽, 高)', '多行文本区。', 'memo');
  g('checkbox("文字", x, y, 宽, 高)', '复选框，看有没有勾上用 checked(控件号)。', 'checkbox');
  g('listbox(x, y, 宽, 高)', '列表，往里面加条目用 additem(控件号, 文字)。', 'listbox');
  g('progress(x, y, 宽, 高)', '进度条，0 到 100 用 setvalue(控件号, 值) 设。', 'progress');
  g('settext(控件号, 文字)', '改标签或文本框里的文字。', 'settext');
  g('settitle(控件号, 文字)', '改窗口标题。', 'settitle');
  g('additem(控件号, 文字)', '往列表里加一条。', 'additem');
  g('checked(控件号)', '复选框勾上了没有：1 勾了，0 没勾。', 'checked');
  g('setchecked(控件号, 0 或 1)', '设置复选框的勾选状态。', 'setchecked');
  g('setvalue(控件号, 0–100)', '设置进度条的位置。', 'setvalue');
  g('getvalue(控件号)', '读进度条当前在哪（0–100）。', 'getvalue');
  g('readtext(控件号)', '读输入框/文本区里的文字。', 'readtext');
  g('msgbox("内容", "标题")', '弹一个提示框，带标题。', 'msgbox');
  g('alert("内容")', '弹一个提示框。', 'alert');
  g('ask("问题", "标题")', '弹是／否询问框：点"是"得 1，点"否"得 0。', 'ask');
  g('sleep(毫秒)', '等一会儿再往下走。', 'sleep');
  g('run()', '写完所有控件以后调用，进入事件循环，窗口才真的出现。', 'run');
  g('destroy(控件号)', '关掉一个控件或窗口；关掉主窗口会让 run() 结束、程序退出。', 'destroy');
  g('launch("命令行")', '启动一个外部程序（比如 java），启动就返回、不等它跑完：1 = 起来了。', 'launch');

  /* ---------------- 文件与系统（C_IO.cash） ---------------- */
  function gi(sig, note, names) {
    names.split(' ').forEach(function (n) { GUI[n] = { cat: 'io', sig: sig, note: note }; });
  }
  gi('readfile("路径")', '把整个文件读成一段文字；文件不存在给 ""。（上限 1MB）', 'readfile');
  gi('writefile("路径", 内容)', '把文字整体写进文件（覆盖原内容）：1 = 成功。', 'writefile');
  gi('appendfile("路径", 内容)', '把文字追加到文件末尾：1 = 成功。', 'appendfile');
  gi('fileexists("路径")', '文件（或目录）在不在：1 在，0 不在。', 'fileexists');
  gi('makedir("路径")', '建一个目录；已经有了也算成功（1）。', 'makedir');
  gi('appdir()', '本程序（exe）所在的完整目录，不带末尾分隔符。', 'appdir');
  gi('envget("名字")', '读环境变量，没有这个变量给 ""。', 'envget');
  gi('launchwait("命令行")', '启动外部程序并等它跑完，返回它的退出码（起不来 -1）。', 'launchwait');

  /* ---------------- CSS 美化（C_CSS.cash） ---------------- */
  var CSS = {};                      /* 小写名 -> {cat, sig, note} */
  function c(cat, sig, note, names) {
    names.split(' ').forEach(function (n) { CSS[n] = { cat: cat, sig: sig, note: note }; });
  }
  c('css', 'csswindow()', '开一个样式作用域（写一次就行，后面就能 cstext/csbg …）。', 'csswindow');
  c('css', 'cstext(控件号, 颜色)', '改文字颜色。颜色写 "#RRGGBB"、"RRGGBB" 或颜色名（"blue" "gold" …）。', 'cstext');
  c('css', 'csbg(控件号, 颜色)', '改背景颜色。', 'csbg');
  c('css', 'csfont(控件号, 字号, 加粗)', '改字号（磅）；加粗写 1，不粗写 0。', 'csfont');
  c('css', 'csradius(控件号, 像素)', '圆角（Linux 生效，Windows 忽略）。', 'csradius');
  c('css', 'csshadow(控件号, 0..3)', '投影强度 0~3（Linux 生效，Windows 忽略）。', 'csshadow');
  c('css', 'cshide(控件号) / csshow(控件号)', '把控件藏起来 / 再显示出来。', 'cshide csshow');
  c('css', 'cspass(控件号)', '把输入框变成密码框（打码）。', 'cspass');
  c('css', 'cscolor(名字, 颜色)', '给一个颜色起个名字，之后能用 csapply 涂到别的控件上。', 'cscolor');
  c('css', 'csapply(名字, 控件号)', '把起好名字的颜色涂到这个控件上。', 'csapply');

  /* ---------------- 操作系统（C_OS.cash） ---------------- */
  var OS = {};                       /* 小写名 -> {cat, sig, note} */
  function o(sig, note, names) {
    names.split(' ').forEach(function (n) { OS[n] = { cat: 'os', sig: sig, note: note }; });
  }
  o('os_pid()', '本程序的进程号（> 0）。', 'os_pid');
  o('os_cores()', '逻辑 CPU 核数（>= 1）。', 'os_cores');
  o('os_ram()', '物理内存大小，单位 GB（>= 0）。', 'os_ram');
  o('os_cpu()', 'CPU 型号文字（拿不到给 ""）。', 'os_cpu');
  o('os_hostname()', '本机主机名（拿不到给 ""）。', 'os_hostname');
  o('os_platform()', '"windows" / "linux" / "mac"。', 'os_platform');
  o('isadmin()', '以管理员 / root 身份运行 -> 1 / 0。', 'isadmin');
  o('uptime()', '开机到现在经过的毫秒数（>= 0）。', 'uptime');
  o('findproc("名字")', '有这个名字的进程在跑 -> 1 / 0。', 'findproc');

  /* ---------------- 网络与攻防（C_NET.cash） ---------------- */
  var NET = {};
  function nn(sig, note, names) {
    names.split(' ').forEach(function (n) { NET[n] = { cat: 'net', sig: sig, note: note }; });
  }
  nn('isurl(文字)', '是 http:// 或 https:// 开头 -> 1 / 0。', 'isurl');
  nn('isip(文字)', '是 IPv4 点分十进制地址 -> 1 / 0。', 'isip');
  nn('urlhost(网址)', '取网址里的主机名（"https://a.com/x" -> "a.com"）。', 'urlhost');
  nn('urlport(网址)', '取网址的端口（没写 -> 按 https=443 / http=80）。', 'urlport');
  nn('fnv1a(文字)', '字符串 32 位 FNV-1a 哈希（校验内容有没有变）。', 'fnv1a');
  nn('base64(文字)', '把文字编码成 base64（返回 base64 字符串）。', 'base64');
  nn('unbase64(文字)', '把 base64 解码回原文（解码失败给 ""）。', 'unbase64');
  nn('hostalive(主机, 端口)', '某主机的某端口 TCP 通不通 -> 1 / 0（会阻塞到超时）。', 'hostalive');

  /* ---------------- 便携化交互（C_SC.cash） ---------------- */
  var SC = {};
  function s(sig, note, names) {
    names.split(' ').forEach(function (n) { SC[n] = { cat: 'sc', sig: sig, note: note }; });
  }
  s('isportable()', '本程序所在目录可写（能就地放文件）-> 1 / 0。', 'isportable');
  s('copytoexe(名, 内容)', '把一个文本文件写到 exe 旁边，1 = 成功。', 'copytoexe');
  s('freebytes()', '空闲内存，单位字节（拿不到 0）。', 'freebytes');
  s('diskfree(路径)', '某盘符 / 目录所在分区的空闲字节（拿不到 0）。', 'diskfree');
  s('is64bit()', '本程序跑在 64 位环境 -> 1 / 0。', 'is64bit');
  s('portcheck(端口)', '127.0.0.1 上某 TCP 端口是否开着 -> 1 / 0。', 'portcheck');
  s('portscan(起, 止)', '这一段端口开着几个 -> 个数。', 'portscan');

  /* 合并进 GUI 命名空间，让 lookup() 一次查全 */
  var i;
  for (i in OS)  GUI[i] = OS[i];
  for (i in NET) GUI[i] = NET[i];
  for (i in SC)  GUI[i] = SC[i];

  /* ---------------- 每一格该填什么（积木视图的悬浮说明） ---------------- */
  var ARG = {
    /* 语句本身 */
    decl: ['变量名', '初始值'],
    print: ['要打印的内容'], say: ['要打印的内容'], show: ['要打印的内容'],
    input: ['变量名'],
    if: ['条件'], while: ['条件'], until: ['条件'],
    for: ['循环变量', '起点', '循环变量', '终点（不含）', '循环变量'],
    foreach: ['元素名字（不要写类型）', '数组'],
    switch: ['要判断的值'], case: ['匹配的值'],
    return: ['返回值'], function: ['函数名', '参数名'], use: ['头文件名（要带引号）'],
    /* 窗口函数 */
    window: ['窗口标题', '宽（像素）', '高（像素）'],
    label: ['显示的文字', '左边距 x', '上边距 y', '宽', '高'],
    button: ['按钮上的文字', '左边距 x', '上边距 y', '宽', '高'],
    textbox: ['一开始的文字（可留空）', '左边距 x', '上边距 y', '宽', '高'],
    memo: ['一开始的文字（可留空）', '左边距 x', '上边距 y', '宽', '高'],
    checkbox: ['复选框旁边的文字', '左边距 x', '上边距 y', '宽', '高'],
    listbox: ['左边距 x', '上边距 y', '宽', '高'],
    progress: ['左边距 x', '上边距 y', '宽', '高'],
    settext: ['控件号', '新的文字'],
    settitle: ['窗口号', '新的标题'],
    additem: ['列表的控件号', '要加的那条文字'],
    checked: ['复选框的控件号'],
    setchecked: ['复选框的控件号', '1 勾上，0 不勾'],
    setvalue: ['进度条的控件号', '0 到 100'],
    getvalue: ['进度条的控件号'],
    readtext: ['控件号'],
    msgbox: ['提示内容', '标题'],
    alert: ['提示内容'],
    ask: ['要问的话', '标题'],
    sleep: ['等多少毫秒（1000 = 1 秒）'],
    run: [],
    destroy: ['要关掉的控件号／窗口号'],
    launch: ['完整命令行（路径有空格要加引号）'],
    /* 文件与系统（C_IO.cash） */
    readfile: ['文件的完整路径'],
    writefile: ['文件的完整路径', '要写进去的文字'],
    appendfile: ['文件的完整路径', '要追加的文字'],
    fileexists: ['文件的完整路径'],
    makedir: ['目录的路径'],
    appdir: [],
    envget: ['环境变量的名字'],
    launchwait: ['完整命令行'],
    /* 操作系统（C_OS.cash） */
    os_pid: [], os_cores: [], os_ram: [], os_cpu: [],
    os_hostname: [], os_platform: [], isadmin: [], uptime: [],
    findproc: ['进程名（比如 notepad）'],
    /* 网络与攻防（C_NET.cash） */
    isurl: ['要判断的文字'], isip: ['要判断的文字'],
    urlhost: ['网址'], urlport: ['网址'], fnv1a: ['要哈希的文字'],
    base64: ['要编码的文字'], unbase64: ['要解码的 base64'],
    hostalive: ['主机名或 IP', '端口（如 80 / 443）'],
    /* 便携化交互（C_SC.cash） */
    isportable: [], copytoexe: ['要写的文件名', '文件内容'],
    freebytes: [], diskfree: ['盘符或目录（如 "C:"）'],
    is64bit: [], portcheck: ['端口号'], portscan: ['起始端口', '结束端口'],
    /* 逻辑（Excel 风格） */
    IF: ['条件（要写比较，比如 x > 3）', '成立时的值', '不成立时的值'],
    AND: ['第一个条件', '第二个条件', '…'], OR: ['第一个条件', '第二个条件', '…'],
    NOT: ['条件'],
    /* 数学（Excel 风格） */
    ABS: ['数值'], SIGN: ['数值'], INT: ['要向下取整的数'],
    ROUND: ['数值', '保留几位小数（可以不写）'],
    SQRT: ['数值'], POWER: ['底数', '指数'], EXP: ['指数'], LN: ['数值'],
    LOG: ['数值', '底数（可以不写）'],
    MOD: ['被除数', '除数'], RAND: [], RANDBETWEEN: ['最小值', '最大值'], PI: [],
    SUM: ['数组名（写整个数组，不用写 [ ]）'],
    AVERAGE: ['数组名（写整个数组，不用写 [ ]）'],
    COUNT: ['数组名（写整个数组，不用写 [ ]）'],
    MAX: ['比大小的数', '…'], MIN: ['比大小的数', '…'],
    SIN: ['弧度'], COS: ['弧度'], TAN: ['弧度'],
    ASIN: ['数值'], ACOS: ['数值'], ATAN: ['数值'],
    LOG10: ['数值'], FLOOR: ['要向下取整的数'], CEILING: ['要向上取整的数'],
    TRUNC: ['要截断的数'], ATAN2: ['y（竖直边）', 'x（水平边）'],
    /* 文本（Excel 风格） */
    LEN: ['文字'], LEFT: ['文字', '取几个'], RIGHT: ['文字', '取几个'],
    MID: ['文字', '从第几位开始（从 1 数）', '取几个'],
    UPPER: ['文字'], LOWER: ['文字'], TRIM: ['文字'],
    CONCAT: ['第一段', '第二段', '…'],
    VALUE: ['要转成整数的文字'], VAL: ['要转成整数的文字'],
    FIND: ['在哪儿找', '要找什么', '从第几位开始（可以不写）'],
    SUBSTITUTE: ['原来的文字', '要换掉的旧文字', '换上去的新文字'],
    REPT: ['要重复的文字', '重复几次'],
    EXACT: ['第一段文字', '第二段文字'],
    CHAR: ['字符编号（0..255）'], CODE: ['文字'],
    /* 小写（C 风格） */
    len: ['文字'], at: ['文字', '第几位（从 1 开始）'], rcl: ['（可以不填，或者 235 / "rcl"）'],
    abs: ['数值'], sqrt: ['数值'], pow: ['底数', '指数'], round: ['数值'],
    floor: ['数值'], ceil: ['数值'],
    sin: ['弧度'], cos: ['弧度'], tan: ['弧度'], asin: ['数值'], acos: ['数值'], atan: ['数值'],
    log: ['数值'], log10: ['数值'], exp: ['指数'], random: [], now: []
  };

  /* 按原样精确查：窗口函数是小写，Excel 类内置是大写；查不到就返回 null */
  function lookup(name) {
    if (!name) return null;
    if (KW[name]) return KW[name];
    if (BI[name]) return BI[name];
    if (GUI[name]) return GUI[name];
    return null;
  }

  function argLabels(name) {
    if (!name) return null;
    return ARG[name] || ARG[name.toUpperCase()] || null;
  }

  return {
    CAT: CAT,
    KW: KW, BI: BI, GUI: GUI, ARG: ARG,
    lookup: lookup,
    argLabels: argLabels
  };
})();

if (typeof module !== 'undefined' && module.exports) module.exports = SYNTAX;
