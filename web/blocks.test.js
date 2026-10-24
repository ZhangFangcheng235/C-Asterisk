/* blocks.test.js -- web/blocks.js 的纯逻辑测试（node web/blocks.test.js）
 *
 * 覆盖：
 *   1. examples/*.cask 与 tests/*.cask 每个文件：
 *      parse -> 语句区间检查（去首尾空白后非空、兄弟区间不重叠且升序、
 *                子语句都在父体内）-> serialize === 原文（逐字节）
 *   2. 槽位编辑：number 分数 = 90;  的「分数」改成「成绩」
 *   3. 语句移动：剪切一条语句插到另一位置，重新 parse 语句数/行数正确
 *   4. 调色板模板插入：插入 if 模板后能再解析出 if 语句
 *   5. 语法容错：缺分号不崩
 *   6. 新关键字：switch/case/default、do…while、foreach…in、until
 */
'use strict';

const fs = require('fs');
const path = require('path');

/* 让相对路径以仓库根为准（无论从哪里运行） */
const ROOT = path.resolve(__dirname, '..');
process.chdir(ROOT);

/* 悬浮说明的语法表：浏览器里由 <script src="syntax.js"> 提供，node 里手动挂上 */
global.SYNTAX = require(path.join(__dirname, 'syntax.js'));

const Blocks = require(path.join(__dirname, 'blocks.js'));
const I = Blocks._internals;

let okCount = 0, badCount = 0;
const failures = [];

function ok(name, cond, extra) {
  if (cond) { okCount++; console.log('ok   ' + name); }
  else {
    badCount++;
    failures.push(name + (extra ? ' -- ' + extra : ''));
    console.log('bad  ' + name + (extra ? '  [' + extra + ']' : ''));
  }
}

function eq(name, got, want) {
  ok(name, got === want, 'got=' + JSON.stringify(got) + ' want=' + JSON.stringify(want));
}

/* ---------- 语句树遍历工具 ---------- */
function allStmts(list, out) {
  out = out || [];
  for (const s of list) {
    out.push(s);
    for (const p of s.parts) if (p.children) allStmts(p.children, out);
  }
  return out;
}

/* ---------- 1. 语料文件：区间 + 序列化 ---------- */
function checkIntervals(name, text, stmts) {
  const isWs = (c) => c === ' ' || c === '\t' || c === '\r' || c === '\n';
  const nonEmpty = (a, b) => {
    let i = a, j = b;
    while (i < j && isWs(text[i])) i++;
    while (j > i && isWs(text[j - 1])) j--;
    return j > i;
  };
  let prevEnd = -1, prevStart = -1;
  let good = true, why = '';
  for (const s of stmts) {
    if (!(s.start >= 0 && s.end <= text.length && s.start < s.end)) {
      good = false; why = '区间越界/为空 [' + s.start + ',' + s.end + ') type=' + s.type; break;
    }
    if (!nonEmpty(s.start, s.end)) { good = false; why = '区间全是空白 [' + s.start + ',' + s.end + ')'; break; }
    if (s.start < prevEnd) { good = false; why = '兄弟区间重叠 @' + s.start + ' type=' + s.type; break; }
    if (s.start < prevStart) { good = false; why = '兄弟区间未按 start 升序'; break; }
    prevEnd = s.end; prevStart = s.start;
    for (const p of s.parts) {
      if (!p.children) continue;
      for (const c of p.children) {
        if (p.body && (c.start < p.body.start + 1 || c.end > p.body.end - 1)) {
          good = false; why = '子语句超出父体 [' + c.start + ',' + c.end + ') vs body [' + p.body.start + ',' + p.body.end + ')'; break;
        }
      }
      if (!good) break;
      if (!checkIntervals(name, text, p.children).good) { good = false; why = '子级区间检查失败'; break; }
    }
    if (!good) break;
  }
  return { good, why };
}

function listCask(dir) {
  return fs.readdirSync(dir).filter((f) => f.endsWith('.cask')).sort().map((f) => path.join(dir, f));
}

const corpus = listCask(path.join(ROOT, 'examples')).concat(listCask(path.join(ROOT, 'tests')));
ok('语料非空（examples + tests）', corpus.length >= 30, 'found ' + corpus.length);

for (const file of corpus) {
  const rel = path.relative(ROOT, file).replace(/\\/g, '/');
  const text = fs.readFileSync(file, 'utf8');          // UTF-8 读入
  let ast, threw = null;
  try { ast = I.parse(text); } catch (e) { threw = e; }
  if (threw) { ok(rel + ' parse 不抛异常', false, threw.message); continue; }
  ok(rel + ' parse 不抛异常', true);

  const iv = checkIntervals(rel, text, ast.stmts);
  ok(rel + ' 语句区间（非空/不重叠/升序/子在父内）', iv.good, iv.why);

  const ser = I.serialize(ast);
  ok(rel + ' serialize === 原文', ser === text,
    ser === text ? '' : 'len ' + ser.length + ' vs ' + text.length +
      ' firstDiff@' + firstDiff(ser, text));
}

function firstDiff(a, b) {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) if (a[i] !== b[i]) return i;
  return n;
}

/* ---------- 2. 槽位编辑 ---------- */
{
  const code = 'number 分数 = 90;\nprint 分数;\n';
  const ast = I.parse(code);
  const decl = ast.stmts[0];
  eq('槽位编辑: 声明语句类型', decl.type, 'decl');
  const slot = decl.slots.find((s) => s.text === '分数');
  ok('槽位编辑: 找到「分数」标识符槽位', !!slot);
  const edited = I.editSlot(code, slot, '成绩');
  ok('槽位编辑: 结果正确', edited === 'number 成绩 = 90;\nprint 分数;\n',
    JSON.stringify(edited));
  /* 再解析，槽位同步更新 */
  const ast2 = I.parse(edited);
  ok('槽位编辑: 再 parse 仍正常', ast2.stmts.length === 2 &&
    ast2.stmts[0].slots.some((s) => s.text === '成绩'));
}

/* 字符串槽位编辑（自动带引号与转义） */
{
  const code = 'print "hello";\n';
  const ast = I.parse(code);
  const slot = ast.stmts[0].slots.find((s) => s.kind === 'str');
  ok('字符串槽位: 带引号区间', !!slot && code.slice(slot.start, slot.end) === '"hello"');
  const edited = I.editSlot(code, slot, 'hi "x"');
  eq('字符串槽位: 转义正确', edited, 'print "hi \\"x\\"";\n');
}

/* ---------- 3. 语句移动 ---------- */
{
  const code = 'print 1;\nprint 2;\nprint 3;\n';
  const ast = I.parse(code);
  const first = ast.stmts[0];
  const last = ast.stmts[2];
  const pos = I.insertPosAfterStmt(code, last);
  const moved = I.moveRange(code, first.start, first.end, pos);
  const ast2 = I.parse(moved);
  eq('语句移动: 行数不变', moved.split('\n').filter((l) => l.trim()).length, 3);
  eq('语句移动: 语句数不变', allStmts(ast2.stmts).length, 3);
  ok('语句移动: 顺序变为 2,3,1',
    ast2.stmts.map((s) => moved.slice(s.start, s.end)).join(',') === 'print 2;,print 3;,print 1;',
    JSON.stringify(moved));
  ok('语句移动: serialize 一致', I.serialize(ast2) === moved);
}

/* 带嵌套体的移动 */
{
  const code = 'print a;\nif (x > 0) {\n    print b;\n}\nprint c;\n';
  const ast = I.parse(code);
  const ifStmt = ast.stmts[1];
  const pos = I.insertPosAfterStmt(code, ast.stmts[2]);
  const moved = I.moveRange(code, ifStmt.start, ifStmt.end, pos);
  const ast2 = I.parse(moved);
  eq('嵌套体移动: 语句数', ast2.stmts.length, 3);
  eq('嵌套体移动: if 挪到最后', ast2.stmts[2].type, 'if');
  eq('嵌套体移动: 子语句保留', ast2.stmts[2].children.length, 1);
}

/* ---------- 4. 调色板模板插入 ---------- */
{
  const code = 'function main() {\n    print 1;\n}\n';
  const ifTpl = I.PALETTE.find((p) => p.code.indexOf('if (') === 0);
  ok('调色板: 有 if 模板', !!ifTpl);
  const ast = I.parse(code);
  const pr = ast.stmts[0].children[0];
  const pos = I.insertPosAfterStmt(code, pr);
  const inserted = I.insertTemplate(code, pos, ifTpl.code, '    ');
  const ast2 = I.parse(inserted);
  const ifs = allStmts(ast2.stmts).filter((s) => s.type === 'if');
  eq('调色板插入 if: 解析出一条 if', ifs.length, 1);
  ok('调色板插入 if: 有函数体', ifs[0].hasBody && ifs[0].children.length === 0);
  ok('调色板插入 if: 原内容还在', inserted.indexOf('print 1;') >= 0);
}

/* 每个调色板模板插入后都能再 parse、serialize 稳定 */
{
  let allGood = true, badName = '';
  for (const item of I.PALETTE) {
    const wrapped = 'function main() {\n' + item.code + '}\n';
    try {
      const a = I.parse(wrapped);
      if (I.serialize(a) !== wrapped) { allGood = false; badName = item.name + ' serialize'; }
    } catch (e) { allGood = false; badName = item.name + ' ' + e.message; }
  }
  ok('调色板全部 ' + I.PALETTE.length + ' 个模板可解析可还原', allGood, badName);
}
ok('调色板条目 >= 70', I.PALETTE.length >= 70, 'count=' + I.PALETTE.length);
ok('调色板 14 个分类', I.CATEGORIES.length === 14);

/* 每个分类一种颜色：颜色互不相同（dot 色块 + cls 颜色类都要唯一） */
(function () {
  var dots = {}, clss = {}, missing = [], dup = [];
  I.CATEGORIES.forEach(function (c) {
    var cc = I.CAT_COLOR[c.id];
    if (!cc || !cc.dot || !cc.cls) { missing.push(c.id); return; }
    if (dots[cc.dot]) dup.push(c.id + ' / ' + dots[cc.dot] + ' 同为 ' + cc.dot);
    if (clss[cc.cls]) dup.push(c.id + ' / ' + clss[cc.cls] + ' 同为 ' + cc.cls);
    dots[cc.dot] = c.id; clss[cc.cls] = c.id;
  });
  ok('颜色表覆盖全部 14 个分类', missing.length === 0, 'missing=' + missing.join(','));
  ok('14 个分类颜色各不相同', dup.length === 0, dup.join('; '));
  ok('14 个分类颜色类各不相同', Object.keys(clss).length === 14,
     'classes=' + Object.keys(clss).join(','));
})();


/* ---------- 4c. 画布积木按分类上色：不同分类不同色 ---------- */
(function () {
  var code = [
    'use "C_OPED.cash";',
    'function main() {',
    '    print "你好";',
    '    float price = 0.0;',
    '    int t = a + b > c;',
    '    string 名字 = "小明";',
    '    int n = LEN(s);',
    '    int 总 = SUM(a, b);',
    '    int 按钮 = button("点我", 20, 60, 120, 32);',
    '    settext(n, "y");',
    '    x = 5;',
    '    if (t > 1) { print 1; }',
    '    for (int i = 0; i < 3; i++) { }',
    '    return;',
    '}'
  ].join('\n');
  var ast = I.parse(code);
  var list = allStmts(ast.stmts);
  var want = {
    'use': 'win',
    'print': 'out',
    'float price = 0.0;': 'var',
    'int t = a + b > c;': 'math',
    'string 名字 = "小明";': 'text',
    'int n = LEN(s);': 'text',
    'int 总 = SUM(a, b);': 'math',
    'int 按钮 = button("点我", 20, 60, 120, 32);': 'win',
    'settext(n, "y");': 'win',
    'x = 5;': 'var',
    'return;': 'fn'
  };
  var bad = [];
  list.forEach(function (st) {
    var head = code.slice(st.parts[0].headStart, st.parts[0].headEnd).trim();
    if (want[head] && I.stmtCat(st) !== want[head]) {
      bad.push(head + ' -> ' + I.stmtCat(st) + ' (want ' + want[head] + ')');
    }
  });
  ok('画布上色: 各分类判断正确', bad.length === 0, bad.join('; '));

  var cls = {};
  list.forEach(function (st) {
    var cat = I.stmtCat(st);
    cls[cat] = I.blockClassOf(st);
  });
  var seen = {}, dup = [];
  Object.keys(cls).forEach(function (cat) {
    if (seen[cls[cat]]) dup.push(cat + ' / ' + seen[cls[cat]] + ' 同为 ' + cls[cat]);
    seen[cls[cat]] = cat;
  });
  ok('画布上色: 不同分类颜色类不同', dup.length === 0, dup.join('; '));
  ok('画布上色: 用到了 var/math/text/win 四种颜色', !!cls['var'] && !!cls['math'] &&
     !!cls['text'] && !!cls['win'],
     JSON.stringify(cls));
  ok('画布上色: 声明块不再一律同色',
     cls['var'] !== cls['math'] && cls['math'] !== cls['text'] && cls['text'] !== cls['win'],
     JSON.stringify(cls));
})();

/* ---------- 5. 语法容错 ---------- */
{
  let threw = null, ast = null;
  const bad = 'function main() {\n  print 1\n}\n';
  try { ast = I.parse(bad); } catch (e) { threw = e; }
  ok('容错: 缺分号不崩', !threw, threw && threw.message);
  if (ast) {
    eq('容错: 顶层 1 条语句', ast.stmts.length, 1);
    eq('容错: function 类型', ast.stmts[0].type, 'function');
    eq('容错: 体内 1 条 print', ast.stmts[0].children.length, 1);
    eq('容错: print 类型', ast.stmts[0].children[0].type, 'print');
    ok('容错: serialize 仍逐字节一致', I.serialize(ast) === bad);
  }
}
{
  /* 更凶残的残缺代码 */
  let threw = null;
  const samples = ['', '   \n\n', ';;;', 'if (x {', '} else {', 'switch (', 'do {',
    '/* unterminated block comment', 'print "unterminated', 'function f( { }', '{ { }'];
  for (const s of samples) {
    try {
      const a = I.parse(s);
      if (I.serialize(a) !== s) { threw = new Error('serialize mismatch for ' + JSON.stringify(s)); }
    } catch (e) { threw = e; }
  }
  ok('容错: 11 段残缺代码均不崩且可还原', !threw, threw && threw.message);
}

/* ---------- 6. 新关键字 ---------- */
{
  const code = [
    'function main() {',
    '    text t = "a";',
    '    switch (t) {',
    '        case 1: print 1; break;',
    '        default: print 9;',
    '    }',
    '}'
  ].join('\n');
  const ast = I.parse(code);
  const sw = allStmts(ast.stmts).find((s) => s.type === 'switch');
  ok('switch: 解析出 switch 且有体', !!sw && sw.hasBody);
  const kids = sw.children.map((c) => c.type);
  eq('switch: 子句序列 case/default', kids.join(','), 'case,print,break,default,print');
  eq('case: 类型', sw.children[0].type, 'case');
  ok('case: 有常量槽位', sw.children[0].slots.some((s) => s.kind === 'num' && s.text === '1'));
}
{
  const code = 'function main() {\n    number n = 0;\n    do {\n        n = n + 1;\n    } while (n < 3);\n}\n';
  const ast = I.parse(code);
  const d = allStmts(ast.stmts).find((s) => s.type === 'do');
  ok('do…while: 类型 + 体 + while 尾', !!d && d.hasBody && d.children.length === 1 &&
    d.parts.length === 2 && d.parts[1].kind === 'while-tail');
  ok('do…while: 整条覆盖到分号', code.slice(d.start, d.end).endsWith('while (n < 3);'));
}
{
  const code = 'function main() {\n    number a[3];\n    foreach (s in a) {\n        print s;\n    }\n}\n';
  const ast = I.parse(code);
  const fe = allStmts(ast.stmts).find((s) => s.type === 'foreach');
  ok('foreach: 类型 + 体', !!fe && fe.hasBody && fe.children.length === 1);
  ok('foreach: 元素与数组都是槽位',
    fe.slots.some((s) => s.text === 's') && fe.slots.some((s) => s.text === 'a'));
}
{
  const code = 'function main() {\n    number n = 0;\n    until (n >= 3) {\n        n = n + 1;\n    }\n}\n';
  const ast = I.parse(code);
  const u = allStmts(ast.stmts).find((s) => s.type === 'until');
  ok('until: 类型 + 体', !!u && u.hasBody && u.children.length === 1);
}

/* ---------- 7. 其它头部类型识别 ---------- */
{
  const code = [
    'constant number LIMIT = 100;',
    'start {',
    '    number a[5] = {10, 20, 30, 40, 50};',
    '    input age;',
    '    show "n="; show a[0];',
    '    while (a[0] < 10) { a[0] += 1; }',
    '    for (number i = 0; i < 5; i++) { if (i) { continue; } else if (i > 2) { break; } else { continue; } }',
    '    { number inner = 1; }',
    '    ;',
    '}'
  ].join('\n');
  const ast = I.parse(code);
  const types = ast.stmts.map((s) => s.type);
  eq('类型识别: constant 声明', types[0], 'decl');
  eq('类型识别: start 入口', types[1], 'start');
  const inner = ast.stmts[1].children;
  eq('数组整表初始化是 decl', inner[0].type, 'decl');
  eq('input', inner[1].type, 'input');
  eq('show', inner[2].type, 'show');            // show "n=";
  eq('show 二连', inner[3].type, 'show');       // show a[0];
  eq('while 有体', inner[4].type, 'while');
  eq('for 有体', inner[5].type, 'for');
  eq('裸块', inner[6].type, 'block');
  eq('空语句', inner[7].type, 'empty');
  ok('if/else 链并成一条', inner[5].children[0].parts.length === 3);
  ok('if/else 链覆盖整段', code.slice(inner[5].children[0].start, inner[5].children[0].end)
    .indexOf('else { continue; }') >= 0);
  ok('= {…} 初始化列表不被当成函数体', !inner[0].hasBody);
}

/* 无花括号的 if / else */
{
  const code = 'start {\n    if (x) print 1; else print 2;\n}\n';
  const ast = I.parse(code);
  const ifStmt = ast.stmts[0].children[0];
  eq('裸 if: 类型', ifStmt.type, 'if');
  ok('裸 if: else 与之并成一条', ifStmt.parts.length === 2 &&
    code.slice(ifStmt.start, ifStmt.end) === 'if (x) print 1; else print 2;',
    JSON.stringify(code.slice(ifStmt.start, ifStmt.end)));
  const code2 = 'start {\n    if (x) print 1; else { print 2; }\n}\n';
  const ast2 = I.parse(code2);
  const f2 = ast2.stmts[0].children[0];
  ok('裸 if + 花括号 else: 并成一条且 else 有体', f2.parts.length === 2 && f2.parts[1].body !== null);
}

/* ---------- 8. 中文标识符 / 注释 / 转义 ---------- */
{
  const code = '// 注释\nnumber 苹果 = 3; /* 块注释 */\ntext 名字 = "小明说\\"你好\\"";\nprint 苹果, 名字;\n';
  const ast = I.parse(code);
  const comments = ast.stmts.filter((s) => s.type === 'comment');
  const real = ast.stmts.filter((s) => s.type !== 'comment');
  eq('中文: 非注释语句数', real.length, 3);
  eq('注释: 两条注释各成一块', comments.length, 2);
  ok('注释: 行注释原文保留', code.slice(comments[0].start, comments[0].end) === '// 注释');
  ok('注释: 块注释原文保留', code.slice(comments[1].start, comments[1].end) === '/* 块注释 */');
  ok('中文: 槽位识别苹果', ast.stmts.some((s) => (s.slots || []).some((x) => x.text === '苹果')));
  const strSlot = ast.stmts.flatMap((s) => s.slots || []).find((s) => s.kind === 'str');
  ok('转义: 字符串槽位到闭合引号', !!strSlot && code.slice(strSlot.start, strSlot.end).endsWith('"'));
  ok('注释+中文: serialize 逐字节一致', I.serialize(ast) === code);
}

/* ---------- 9. 删除 ---------- */
{
  const code = 'print a;\nif (x) {\n    print b;\n}\nprint c;\n';
  const ast = I.parse(code);
  const del = I.deleteRange(code, ast.stmts[1].start, ast.stmts[1].end);
  eq('删除: 连同嵌套体整段删', del, 'print a;\nprint c;\n');
  eq('删除: 重新 parse 语句数', I.parse(del).stmts.length, 2);
}
{
  const code = 'print a;\nprint b;\n';
  const ast = I.parse(code);
  const del = I.deleteRange(code, ast.stmts[0].start, ast.stmts[0].end);
  eq('删除: 整行删除不留空行', del, 'print b;\n');
}

/* ---------- 10. 悬浮说明与括号里的加号 ---------- */
{
  const code = 'function add(int a, int b) {\n    int n = LEN("abc");\n    button("确定", 10, 10, 60, 24);\n}\n';
  const ast = I.parse(code);
  const fn = ast.stmts[0];
  const head = fn.parts[0];
  const txt = (s, p, t) => p.slots.findIndex((x) => x.text === t);

  /* 函数定义：名字那格、参数那格 */
  eq('悬浮: 函数定义的名字', I.slotTip(fn, head, txt(fn, head, 'add')).title, '函数名');
  ok('悬浮: 参数说明里提到类型', /类型/.test(I.slotTip(fn, head, txt(fn, head, 'a')).title),
    I.slotTip(fn, head, txt(fn, head, 'a')).title);

  /* 内置函数：LEN 的参数 */
  const kid1 = fn.children[0], p1 = kid1.parts[0];
  const t1 = I.slotTip(kid1, p1, txt(kid1, p1, '"abc"'));
  eq('悬浮: LEN 参数名', t1.title, '文字');
  ok('悬浮: LEN 的说明带用法', /长度/.test(t1.note), t1.note);

  /* 窗口函数：按调用 + 逗号序号给说明 */
  const kid2 = fn.children[1], p2 = kid2.parts[0];
  eq('悬浮: button 第 1 个参数', I.slotTip(kid2, p2, txt(kid2, p2, '"确定"')).title, '按钮上的文字');
  eq('悬浮: button 第 4 个参数是宽', I.slotTip(kid2, p2, txt(kid2, p2, '60')).title, '宽');
  eq('悬浮: 函数名那格说"要用的函数"', I.slotTip(kid2, p2, txt(kid2, p2, 'button')).title, '要用的函数');

  /* 普通声明：变量名 / 初始值 */
  const decl = I.parse('int count = 0;\n').stmts[0], dp = decl.parts[0];
  eq('悬浮: 声明第 1 格是变量名', I.slotTip(decl, dp, txt(decl, dp, 'count')).title, '变量名');
  eq('悬浮: 声明第 2 格是初始值', I.slotTip(decl, dp, txt(decl, dp, '0')).title, '初始值');

  /* 条件语句没有函数名，按语句类型给说明 */
  const w = I.parse('while (x < 3) {\n}\n').stmts[0], wp = w.parts[0];
  eq('悬浮: while 的条件', I.slotTip(w, wp, txt(w, wp, 'x')).title, '条件');

  /* 括号：空括号的洞 / 末尾续参数 */
  const closeIdx = (part) => part.toks.findIndex((t) => t.kind === 'op' && t.text === ')');
  const ci0 = closeIdx(head);
  eq('括号: 定义函数的括号里数得清参数', I.parenInfo(head.toks, ci0).arg, 1);

  const eAst = I.parse('function main() {\n}\n');
  const eHead = eAst.stmts[0].parts[0];
  const ec = closeIdx(eHead);
  const pe = I.parenInfo(eHead.toks, ec);
  ok('括号: 空括号认得前面的函数名', !!pe && pe.name === 'main' && pe.arg === 0, JSON.stringify(pe));
  ok('括号: 空括号说明是加参数', /括号里加参数/.test(I.holeTip(eAst.stmts[0], eHead, ec, 'empty').title),
    I.holeTip(eAst.stmts[0], eHead, ec, 'empty').title);
  /* 空括号里填上内容，就等于加了参数（演示 splice 插入位置） */
  {
    const src = 'function main() {\n}\n';
    const at = src.indexOf(')');
    eq('括号: 往空括号里插参数', I.splice(src, at, at, 'int n'), 'function main(int n) {\n}\n');
  }

  const ci1 = closeIdx(p2);
  eq('括号: 数得清括号里已有几个参数', I.parenInfo(p2.toks, ci1).arg, 4);
  ok('括号: 末尾续参数的说明',
    /后面还能再加一个/.test(I.holeTip(kid2, p2, ci1, 'add').title),
    I.holeTip(kid2, p2, ci1, 'add').title);
  ok('括号: 超过程度说明会提示参数太多',
    /只用 5 个参数/.test(I.holeTip(kid2, p2, ci1, 'add').note),
    I.holeTip(kid2, p2, ci1, 'add').note);

  /* 非函数调用的括号（if / while / switch）不长加号 */
  const iw = I.parse('if (x > 0) {\n}\n').stmts[0], iwp = iw.parts[0];
  ok('括号: if 的括号不算函数调用', I.parenInfo(iwp.toks, closeIdx(iwp)).name === '',
    JSON.stringify(I.parenInfo(iwp.toks, closeIdx(iwp))));

  /* 语法表自身要自洽：每个关键字/内置函数都得有用法说明 */
  const SYN = global.SYNTAX;
  let miss = [];
  Object.keys(SYN.KW).forEach((k) => { if (!SYN.KW[k].sig || !SYN.KW[k].note) miss.push('KW:' + k); });
  Object.keys(SYN.BI).forEach((k) => { if (!SYN.BI[k].sig || !SYN.BI[k].note) miss.push('BI:' + k); });
  Object.keys(SYN.GUI).forEach((k) => { if (!SYN.GUI[k].sig || !SYN.GUI[k].note) miss.push('GUI:' + k); });
  ok('语法表: 关键字/内置函数都有用法说明', miss.length === 0, miss.join(','));

  /* 编译器 reg_builtins() 里注册的全部内置函数名（大写的 Excel 风格 + 小写的 C 风格），
     悬浮说明和高亮都得认得它们；rcl 是彩蛋，故意不收。 */
  const BUILTINS = ('IF AND OR NOT TRUE FALSE ABS SIGN MOD ROUND INT POWER SQRT EXP LN LOG PI ' +
    'RAND RANDBETWEEN SUM AVERAGE COUNT MAX MIN LEN LEFT RIGHT MID UPPER LOWER TRIM CONCAT ' +
    'VALUE VAL len at abs sqrt pow sin cos tan asin acos atan log log10 exp floor ceil round ' +
    'random now').split(' ').filter((s) => s.length);
  const noTip = BUILTINS.filter((n) => { const e = SYN.lookup(n); return !e || !SYN.CAT[e.cat]; });
  ok('语法表: 认得出编译器注册的全部内置函数', noTip.length === 0, noTip.join(','));
  const noCat = BUILTINS.filter((n) => !I.CALL_CAT[n]);
  ok('积木分类: 内置函数都有所属分类', noCat.length === 0, noCat.join(','));

  /* firstCallName 现在返回原样大小写：小写的窗口函数也得认出来 */
  const wCall = I.parse('function main() {\n    settext(标签, "你好");\n}\n').stmts[0];
  eq('语句分类: 小写窗口函数识别为 win', I.stmtCat(wCall.children[0]), 'win');
  const sumCall = I.parse('function main() {\n    int 总分 = SUM(成绩);\n}\n').stmts[0];
  eq('语句分类: SUM(...) 识别为 math', I.stmtCat(sumCall.children[0]), 'math');
  const sinCall = I.parse('function main() {\n    float y = sin(0.5);\n}\n').stmts[0];
  eq('语句分类: sin(...) 识别为 math', I.stmtCat(sinCall.children[0]), 'math');
  eq('语句分类: firstCallName 保留大小写', I.firstCallName(sinCall.children[0].parts[0].toks), 'sin');
}

/* ---------- 11. 对外 API 与 node 守卫 ---------- */
ok('API: init/refresh/showError 齐全',
  typeof Blocks.init === 'function' &&
  typeof Blocks.refresh === 'function' &&
  typeof Blocks.showError === 'function');
ok('API: 无 window 环境加载不报错（DOM 守卫）', true);   // require 本文件本身即证明
ok('_internals: 暴露 parse/serialize/editSlot/moveRange/insertSnippet',
  typeof I.parse === 'function' && typeof I.serialize === 'function' &&
  typeof I.editSlot === 'function' && typeof I.moveRange === 'function' &&
  typeof I.insertSnippet === 'function');
ok('_internals: 暴露悬浮说明用的 parenInfo/slotTip/holeTip',
  typeof I.parenInfo === 'function' && typeof I.slotTip === 'function' &&
  typeof I.holeTip === 'function');

/* ---------- 汇总 ---------- */
console.log('');
console.log('ok: ' + okCount + '  bad: ' + badCount);
if (badCount) {
  console.log('failed:');
  for (const f of failures) console.log('  - ' + f);
  process.exit(1);
}
process.exit(0);
