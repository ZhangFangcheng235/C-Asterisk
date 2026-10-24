/* blocks.js -- C* (C Asterisk) 积木视图（span 视图）
 *
 * 代码文本是唯一真相：积木只是它的视图。
 *   parse(code)      把代码切成带绝对区间的语句树
 *   serialize(ast)   未编辑时逐字节还原原文
 *   编辑 = 文本区间splice，之后一律 re-parse。
 *
 * 浏览器里通过 <script src="blocks.js"> 引入，挂 window.Blocks；
 * node 里可以 require 本文件，用 Blocks._internals 做纯逻辑测试。
 */
'use strict';

/* ================================================================== *
 * 1. 词法：跳过注释/空白的 token 扫描器
 * ================================================================== */

var KW = {};
['print','say','show','input','start','if','else','while','for','break','continue',
 'return','constant','use','and','or','not','true','false','number','real','text',
 'letter','bool','none','function','switch','case','default','do','foreach','in',
 'until','func','int','float','string','char','const','void'].forEach(function (k) { KW[k] = true; });

var TYPE_WORDS = { int:1, float:1, string:1, char:1, bool:1, none:1, void:1,
                   number:1, real:1, text:1, letter:1 };

/* 语法小抄（syntax.js）：悬浮说明用；没加载上也能照常跑 */
var SYN = (typeof SYNTAX !== 'undefined') ? SYNTAX : null;

function isIdentStart(c) {
  if (c === undefined) return false;
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_' || c === '$' ||
         c.charCodeAt(0) >= 0x80;               // 中文等非 ASCII 标识符
}
function isIdentPart(c) {
  return isIdentStart(c) || (c >= '0' && c <= '9');
}

/* 跳过空白与 //、/* *​/ 注释，返回新位置 */
function skipWs(text, i) {
  for (;;) {
    var c = text[i];
    if (c === ' ' || c === '\t' || c === '\n' || c === '\r' || c === '\f' || c === '\v' || c === '\uFEFF') { i++; continue; }
    if (c === '/' && text[i + 1] === '/') {
      i += 2;
      while (i < text.length && text[i] !== '\n') i++;
      continue;
    }
    if (c === '/' && text[i + 1] === '*') {
      i += 2;
      while (i < text.length && !(text[i] === '*' && text[i + 1] === '/')) i++;
      i = Math.min(i + 2, text.length);
      continue;
    }
    return i;
  }
}

var OPS2 = ['==','!=','<=','>=','&&','||','++','--','+=','-=','*=','/=','%=','&=','|=','^=','<<','>>','**'];

/* 读一个 token：{kind:'str'|'chr'|'ident'|'num'|'op', start, end, text} */
function readToken(text, i) {
  i = skipWs(text, i);
  if (i >= text.length) return null;
  var c = text[i];
  if (c === '"' || c === "'") {
    var q = c, j = i + 1;
    while (j < text.length) {
      if (text[j] === '\\') { j += 2; continue; }
      if (text[j] === q) { j++; break; }
      if (text[j] === '\n') break;             // 未闭合字符串到行为止
      j++;
    }
    return { kind: q === '"' ? 'str' : 'chr', start: i, end: j, text: text.slice(i, j) };
  }
  if (isIdentStart(c)) {
    var a = i;
    while (a < text.length && isIdentPart(text[a])) a++;
    return { kind: 'ident', start: i, end: a, text: text.slice(i, a) };
  }
  if (c >= '0' && c <= '9') {
    var b = i;
    while (b < text.length && text[b] >= '0' && text[b] <= '9') b++;
    if (text[b] === '.' && text[b + 1] >= '0' && text[b + 1] <= '9') {
      b++;
      while (b < text.length && text[b] >= '0' && text[b] <= '9') b++;
    }
    if (text[b] === 'e' || text[b] === 'E') {
      var k = b + 1;
      if (text[k] === '+' || text[k] === '-') k++;
      if (text[k] >= '0' && text[k] <= '9') {
        k++;
        while (k < text.length && text[k] >= '0' && text[k] <= '9') k++;
        b = k;
      }
    }
    return { kind: 'num', start: i, end: b, text: text.slice(i, b) };
  }
  var two = text.substr(i, 2);
  if (OPS2.indexOf(two) >= 0) return { kind: 'op', start: i, end: i + 2, text: two };
  return { kind: 'op', start: i, end: i + 1, text: c };
}

/* ================================================================== *
 * 2. 语句解析（容错：能切多少算多少）
 * ================================================================== */

/* 从 start 扫描一条语句的头部 token，直到深度 0 的 ; { }（case/default 还认 :）。
 * 声明里的 “= { … }” 初始化大括号会被当作表达式的一部分，不当成函数体。 */
function lexHead(text, start, to) {
  var toks = [], i = start, depth = 0, sawAssign = false, term = null;
  for (;;) {
    i = skipWs(text, i);
    if (i >= to) { term = { kind: 'eof', at: to }; break; }
    var c = text[i];
    if (depth === 0) {
      if (c === ';') { term = { kind: 'semi', at: i }; break; }
      if (c === '}') { term = { kind: 'rbrace', at: i }; break; }
      if (c === '{') {
        if (!sawAssign) { term = { kind: 'brace', at: i }; break; }
        /* “= {1,2,3};” 初始化列表：整体当 token 吃掉 */
        var d2 = 0;
        for (;;) {
          var t2 = readToken(text, i);
          if (!t2 || i >= to) break;
          toks.push(t2); i = t2.end;
          if (t2.kind === 'op') {
            if (t2.text === '{') d2++;
            else if (t2.text === '}') { d2--; if (d2 === 0) break; }
          }
          if (i >= to) break;
        }
        continue;
      }
      if (c === ':' && toks.length && toks[0].kind === 'ident' &&
          (toks[0].text === 'case' || toks[0].text === 'default')) {
        term = { kind: 'colon', at: i }; break;
      }
    }
    var t = readToken(text, i);
    if (!t) { term = { kind: 'eof', at: to }; break; }
    if (t.kind === 'op') {
      if (t.text === '(' || t.text === '[') depth++;
      else if (t.text === ')' || t.text === ']') { if (depth > 0) depth--; }
      else if (depth === 0 && t.text === '=') sawAssign = true;   // 注意 == 不算赋值
    }
    toks.push(t);
    i = t.end;
  }
  return { toks: toks, term: term };
}

/* 找与 openAt 处 '{' 匹配的 '}'；找不到时假装到 to 结束 */
function findCloseBrace(text, openAt, to) {
  var depth = 0, i = openAt;
  while (i < to) {
    i = skipWs(text, i);
    if (i >= to) return { end: to, found: false };
    var t = readToken(text, i);
    if (!t) return { end: to, found: false };
    if (t.kind === 'op') {
      if (t.text === '{') depth++;
      else if (t.text === '}') { depth--; if (depth === 0) return { end: t.end, found: true }; }
    }
    i = t.end;
  }
  return { end: to, found: false };
}

function classifyHead(toks) {
  if (!toks.length) return 'block';                    // 裸 { } 块
  var t0 = toks[0];
  if (t0.kind !== 'ident') return 'expr';
  var w = t0.text;
  switch (w) {
    case 'print': case 'say': return 'print';
    case 'show':  return 'show';
    case 'input': return 'input';
    case 'if':    return 'if';
    case 'while': return 'while';
    case 'for':   return 'for';
    case 'until': return 'until';
    case 'do':    return 'do';
    case 'foreach': return 'foreach';
    case 'switch': return 'switch';
    case 'case':  return 'case';
    case 'default': return 'default';
    case 'else':  return 'else';
    case 'return': return 'return';
    case 'break': return 'break';
    case 'continue': return 'continue';
    case 'start': return 'start';
    case 'use':   return 'use';
    case 'constant': case 'const': return 'decl';
    case 'function': case 'func': return 'function';
    default:
      if (TYPE_WORDS[w]) {
        /* `number add(number a) {` 是函数定义，`number x = 1;` 是声明 */
        if (toks[1] && toks[1].kind === 'ident' &&
            toks[2] && toks[2].kind === 'op' && toks[2].text === '(') return 'function';
        return 'decl';
      }
      return 'expr';
  }
}

/* 头部可编辑槽位：标识符（非关键字）、数字、字符串/字符字面量 */
function slotsFromToks(toks) {
  var slots = [];
  for (var i = 0; i < toks.length; i++) {
    var t = toks[i];
    if (t.kind === 'num') slots.push({ kind: 'num', start: t.start, end: t.end, text: t.text });
    else if (t.kind === 'str') slots.push({ kind: 'str', start: t.start, end: t.end, text: t.text });
    else if (t.kind === 'chr') slots.push({ kind: 'chr', start: t.start, end: t.end, text: t.text });
    else if (t.kind === 'ident' && !KW[t.text]) slots.push({ kind: 'ident', start: t.start, end: t.end, text: t.text });
    else if (t.kind === 'comment') slots.push({ kind: 'comment', start: t.start, end: t.end, text: t.text });
  }
  return slots;
}

function mkPart(kind, headStart, headEnd, toks, body, children) {
  return {
    kind: kind,
    headStart: headStart, headEnd: headEnd,
    toks: toks,
    slots: slotsFromToks(toks),
    body: body || null,                   // {start,end} —— 含花括号
    children: children || null
  };
}

/* 只跳过空白（不跳注释）：注释行在积木视图里是可见的"注释积木" */
function skipSpaces(text, i, to) {
  for (; i < to; i++) {
    var c = text[i];
    if (c !== ' ' && c !== '\t' && c !== '\n' && c !== '\r' && c !== '\f' && c !== '\v' && c.charCodeAt(0) !== 0xFEFF) break;
  }
  return i;
}

/* 解析 [from,to) 里的语句列表（调用方保证 to 处是 '}' 或边界） */
function parseStmts(text, from, to) {
  var stmts = [], i = from;
  for (;;) {
    i = skipSpaces(text, i, to);
    if (i >= to) break;
    /* 行注释：一条注释 = 一块注释积木（不含行尾换行，换行留在间隙里，序列化逐字节不变） */
    if (text[i] === '/' && text[i + 1] === '/') {
      var cj = i + 2;
      while (cj < to && text[cj] !== '\n') cj++;
      /* 整段注释是一个可编辑槽位：点黄色格子就能改注释文字 */
      stmts.push(mkSimple('comment', i, cj, [{ kind: 'comment', start: i, end: cj, text: text.slice(i, cj) }], i, cj, null));
      i = cj; continue;
    }
    /* 块注释：/* ... *​/ 也是一块 */
    if (text[i] === '/' && text[i + 1] === '*') {
      var ck = i + 2;
      while (ck < to && !(text[ck] === '*' && text[ck + 1] === '/')) ck++;
      ck = Math.min(ck + 2, to);
      stmts.push(mkSimple('comment', i, ck, [{ kind: 'comment', start: i, end: ck, text: text.slice(i, ck) }], i, ck, null));
      i = ck; continue;
    }
    if (text[i] === '}') break;
    if (text[i] === ';') { stmts.push(mkSimple('empty', i, i + 1, [], i, i + 1, null)); i++; continue; }
    var before = i;
    var st = parseOneStmt(text, i, to);
    if (!st || st.end <= before) {          // 安全网：绝不死循环
      if (!st) st = mkSimple('other', before, Math.min(before + 1, to), [], before, Math.min(before + 1, to), null);
      stmts.push(st);
      i = Math.max(st.end, before + 1);
      continue;
    }
    stmts.push(st);
    i = st.end;
  }
  return stmts;
}

function mkSimple(type, start, end, toks, headStart, headEnd, body) {
  var children = body ? body.stmts : null;
  var part = mkPart(type, headStart, headEnd, toks, body, children);
  return {
    type: type,
    start: start, end: end,
    headStart: headStart, headEnd: headEnd,
    slots: part.slots,
    hasBody: !!body,
    body: body,
    children: children,
    parts: [part]
  };
}

/* 扫描 if 后面的 else / else if 链（i 为 if 主体结束处） */
function scanElseChain(text, i, to) {
  var chain = [];
  for (;;) {
    var nt = readToken(text, i);
    if (!nt || nt.kind !== 'ident' || nt.text !== 'else') break;
    var after = readToken(text, nt.end);
    if (after && after.kind === 'ident' && after.text === 'if') {
      var lx2 = lexHead(text, nt.start, to);
      if (lx2.term.kind !== 'brace') break;
      var cl2 = findCloseBrace(text, lx2.term.at, to);
      chain.push(mkPart('elseif', nt.start, lx2.term.at, lx2.toks,
        { start: lx2.term.at, end: cl2.end },
        parseStmts(text, lx2.term.at + 1, cl2.found ? cl2.end - 1 : to)));
      i = cl2.end;
    } else if (after && after.kind === 'op' && after.text === '{') {
      var cl3 = findCloseBrace(text, after.start, to);
      chain.push(mkPart('else', nt.start, after.start, [],
        { start: after.start, end: cl3.end },
        parseStmts(text, after.start + 1, cl3.found ? cl3.end - 1 : to)));
      i = cl3.end;
      break;                                       // else 收尾
    } else {
      /* `else print 2;` 无花括号：把后面一条语句并进来当它的身体 */
      var inline = parseStmts(text, nt.end, to);
      if (!inline.length) break;
      chain.push(mkPart('else', nt.start, nt.end, [], null, [inline[0]]));
      i = inline[0].end;
      break;
    }
  }
  return { chain: chain, end: i };
}

function parseOneStmt(text, start, to) {
  var lex = lexHead(text, start, to);
  var toks = lex.toks, term = lex.term;
  var type = classifyHead(toks);

  /* ---- 普通语句：分号 / 冒号（case）结尾 ---- */
  if (term.kind === 'semi' || term.kind === 'colon') {
    var st = mkSimple(type, start, term.at + 1, toks, start, term.at + 1, null);
    if (type === 'if') {                          // `if (x) print 1; else { }`
      var ch0 = scanElseChain(text, term.at + 1, to);
      if (ch0.chain.length) { st.parts = st.parts.concat(ch0.chain); st.end = ch0.end; }
    }
    return st;
  }
  /* ---- 残缺语句：撞到 } 或文件尾（容错，不当错误） ---- */
  if (term.kind === 'rbrace' || term.kind === 'eof') {
    return mkSimple(type, start, term.at, toks, start, term.at, null);
  }

  /* ---- 带 { } 体的语句 ---- */
  var close = findCloseBrace(text, term.at, to);
  var body = { start: term.at, end: close.end, stmts: parseStmts(text, term.at + 1, close.found ? close.end - 1 : to) };
  var s = mkSimple(type, start, close.end, toks, start, term.at, body);
  var i = close.end, end = close.end;

  if (type === 'if') {
    /* else / else if 链：都并进同一条语句（移动/删除时整体走） */
    var ch = scanElseChain(text, i, to);
    if (ch.chain.length) { s.parts = s.parts.concat(ch.chain); end = ch.end; }
  } else if (type === 'do') {
    /* do { } while (条件); */
    var wt = readToken(text, i);
    if (wt && wt.kind === 'ident' && wt.text === 'while') {
      var lx3 = lexHead(text, i, to);
      if (lx3.term.kind === 'semi') {
        s.parts.push(mkPart('while-tail', i, lx3.term.at + 1, lx3.toks, null, null));
        end = lx3.term.at + 1;
      }
    }
  }
  s.end = end;
  return s;
}

/* 顶层入口：parse(code) -> {text, stmts} */
function parse(code) {
  var text = String(code == null ? '' : code);
  return { text: text, stmts: parseStmts(text, 0, text.length) };
}

/* ================================================================== *
 * 3. 序列化（未编辑时逐字节 = 原文）
 * ================================================================== */

function serialize(ast) {
  var text = ast.text, out = '', pos = 0, stmts = ast.stmts;
  for (var i = 0; i < stmts.length; i++) {
    var s = stmts[i];
    if (s.start < pos) continue;               // 理论上不会发生
    out += text.slice(pos, s.start);
    out += text.slice(s.start, s.end);
    pos = s.end;
  }
  out += text.slice(pos);
  return out;
}

/* ================================================================== *
 * 4. 纯文本编辑（积木编辑全部落到这里）
 * ================================================================== */

function splice(text, start, end, rep) {
  return text.slice(0, start) + rep + text.slice(end);
}

function escapeStr(v) {
  return '"' + String(v).replace(/\\/g, '\\\\').replace(/"/g, '\\"')
    .replace(/\n/g, '\\n').replace(/\t/g, '\\t') + '"';
}
function escapeChr(v) {
  return "'" + String(v).replace(/\\/g, '\\\\').replace(/'/g, "\\'")
    .replace(/\n/g, '\\n').replace(/\t/g, '\\t') + "'";
}

/* 改一个槽位：str/chr 自动补引号与转义，ident/num 原样（去首尾空白） */
function editSlot(text, slot, value) {
  var v = String(value);
  if (slot.kind === 'str') return splice(text, slot.start, slot.end, escapeStr(v));
  if (slot.kind === 'chr') return splice(text, slot.start, slot.end, escapeChr(v.charAt(0) === '' ? ' ' : v));
  if (slot.kind === 'comment') {
    /* 注释槽位：允许用户只改正文；没写 "//" 或 "/*" 前缀就自动补行注释 */
    var cv = v.replace(/\s+$/g, '');
    if (cv && cv.charAt(0) !== '/') cv = '// ' + cv;
    return splice(text, slot.start, slot.end, cv);
  }
  return splice(text, slot.start, slot.end, v.replace(/^\s+|\s+$/g, ''));
}

/* 删除一条语句（连同嵌套体）：能整行删就连缩进和换行一起删 */
function deleteRange(text, start, end) {
  var s = start, e = end;
  var ls = text.lastIndexOf('\n', s - 1) + 1;
  var wholeLine = /^[ \t]*$/.test(text.slice(ls, s));
  if (wholeLine && ls > 0) {
    s = ls - 1;                                              // 连前面的换行一起删（尾部保留）
    return text.slice(0, s) + text.slice(e);
  }
  var q = e;
  while (q < text.length && (text[q] === ' ' || text[q] === '\t')) q++;
  if (q >= text.length) return text.slice(0, s) + text.slice(q);
  if (text[q] === '\r' || text[q] === '\n') {
    e = (text[q] === '\r' && text[q + 1] === '\n') ? q + 2 : q + 1;
  } else if (text[q] === '/' && text[q + 1] === '/') {
    while (q < text.length && text[q] !== '\n') q++;        // 行尾注释随语句一起删
    e = (q < text.length) ? q + 1 : q;
  }
  return text.slice(0, s) + text.slice(e);
}

/* 剪切 [start,end) 插到 target（都是绝对偏移；字节守恒，只重排） */
function moveRange(text, start, end, target) {
  if (target >= start && target <= end) return text;
  var cut = text.slice(start, end);
  if (target < start) {
    return text.slice(0, target) + cut + text.slice(target, start) + text.slice(end);
  }
  return text.slice(0, start) + text.slice(end, target) + cut + text.slice(target);
}

/* 在 pos 处插入一段模板，必要时补换行把它隔成独立语句 */
function insertSnippet(text, pos, snippet) {
  var ins = snippet;
  if (pos > 0) {
    var before = text[pos - 1];
    if (!/\s/.test(before) && before !== ';' && before !== '{' && before !== '}') ins = '\n' + ins;
  }
  var after = text[pos] || '';
  if (after !== '' && !/[\s]/.test(after)) {
    if (!/\n$/.test(ins)) ins += '\n';
  }
  return text.slice(0, pos) + ins + text.slice(pos);
}

/* ---- 位置/行工具 ---- */
function lineOf(text, offset) {
  var n = 1;
  for (var i = 0; i < offset && i < text.length; i++) {
    if (text[i] === '\n') n++;
    else if (text[i] === '\r') { if (text[i + 1] !== '\n') n++; }
  }
  return n;
}

/* pos 所在行的缩进（该行 pos 之前必须全是空白才返回） */
function lineIndentAt(text, pos) {
  var ls = text.lastIndexOf('\n', pos - 1) + 1;
  var pre = text.slice(ls, pos);
  return /^[ \t]*$/.test(pre) ? pre : '';
}

/* 语句后面最顺眼的插入点：行尾空白/注释之后、下一行行首 */
function insertPosAfterStmt(text, stmt) {
  var q = stmt.end;
  while (q < text.length && (text[q] === ' ' || text[q] === '\t')) q++;
  if (q >= text.length) return text.length;
  if (text[q] === '\r' || text[q] === '\n') return (text[q] === '\r' && text[q + 1] === '\n') ? q + 2 : q + 1;
  if (text[q] === '/' && text[q + 1] === '/') {
    while (q < text.length && text[q] !== '\n') q++;
    return q < text.length ? q + 1 : q;
  }
  return stmt.end;
}

/* 语句前面最顺眼的插入点：整行只有缩进时用行首 */
function insertPosBeforeStmt(text, stmt) {
  var ls = text.lastIndexOf('\n', stmt.start - 1) + 1;
  return /^[ \t]*$/.test(text.slice(ls, stmt.start)) ? ls : stmt.start;
}

/* 把多行模板按缩进对齐（首行由调用方决定是否加缩进） */
function indentSnippet(snippet, indent) {
  var lines = snippet.replace(/\r\n/g, '\n').replace(/\n+$/, '').split('\n');
  var out = [];
  for (var i = 0; i < lines.length; i++) out.push(lines[i] ? indent + lines[i] : '');
  return out.join('\n') + '\n';
}

/* 在 pos 处按行缩进插入模板（UI 用） */
function insertTemplate(text, pos, snippet, indent) {
  var ins = indentSnippet(snippet, indent || '');
  if (pos > 0 && text[pos - 1] !== '\n' && pos === text.length) ins = '\n' + ins;
  return insertSnippet(text, pos, ins);
}

/* ================================================================== *
 * 5. 积木箱：8 个分类、70+ 条目（新写法，无弃用关键字）
 * ================================================================== */

var CATEGORIES = [
  { id: 'out',  name: '输出输入' },
  { id: 'var',  name: '变量数据' },
  { id: 'math', name: '运算' },
  { id: 'if',   name: '判断' },
  { id: 'loop', name: '循环' },
  { id: 'fn',   name: '函数' },
  { id: 'text', name: '字符串与数组' },
  { id: 'win',  name: '窗口程序' },
  { id: 'css',  name: 'CSS 美化' },
  { id: 'io',   name: '文件与系统' },
  { id: 'os',   name: '操作系统' },
  { id: 'net',  name: '网络与攻防' },
  { id: 'sc',   name: '便携化交互' },
  { id: 'misc', name: '其它' }
];

/* 头文件门禁：某分类的积木只有程序里 use 了对应头文件（扫描成功）才显示。
 * 不 use 就根本不出现 —— "不use就用不了，不显示"。 */
var HEADER_GATED = {
  win: 'C_OPED.cash',
  css: 'C_CSS.cash',
  io:  'C_IO.cash',
  os:  'C_OS.cash',
  net: 'C_NET.cash',
  sc:  'C_SC.cash'
};

/* 扫描代码里 use 了哪些头文件（.cask 里写 use "X.cash"; 就算 use 了） */
function scanHeaders(code) {
  var names = [], re = /\buse\s*"([^"]+)"/g, m;
  while ((m = re.exec(code)) !== null) names.push(m[1]);
  return names;
}
function isCatUnlocked(cat, headers) {
  var h = HEADER_GATED[cat];
  if (!h) return true;
  for (var i = 0; i < headers.length; i++) if (headers[i] === h) return true;
  return false;
}

/* 每个分类一种颜色（互不相同）；注释单独用绿色。
 * 0.4.2 起与编辑器语法高亮的令牌色保持一致（VS 2026 深色）：
 *   dot = 积木箱里分类条目前面的小方块
 *   cls = 积木箱条目 / 画布积木用的颜色类（对应上面 CSS 里的 .c-xxx） */
var CAT_COLOR = {
  out:  { dot: '#4ec9b0', cls: 'c-out',  name: '青' },
  var:  { dot: '#569cd6', cls: 'c-var',  name: '蓝' },
  math: { dot: '#b5cea8', cls: 'c-math', name: '浅绿' },
  if:   { dot: '#c586c0', cls: 'c-if',   name: '紫粉' },
  loop: { dot: '#d7ba7d', cls: 'c-loop', name: '黄褐' },
  fn:   { dot: '#dcdcaa', cls: 'c-fn',   name: '黄' },
  text: { dot: '#ce9178', cls: 'c-text', name: '橙' },
  win:  { dot: '#4fc1ff', cls: 'c-win',  name: '天蓝' },
  css:  { dot: '#ab7dff', cls: 'c-css',  name: '紫' },
  io:   { dot: '#dcaa5a', cls: 'c-io',   name: '沙金' },
  os:   { dot: '#5fc9b8', cls: 'c-os',   name: '青绿' },
  net:  { dot: '#ff7f6e', cls: 'c-net',  name: '珊瑚红' },
  sc:   { dot: '#e2b743', cls: 'c-sc',   name: '琥珀' },
  misc: { dot: '#9aa3b5', cls: 'c-misc', name: '灰' },
  comment: { dot: '#6a9955', cls: 'c-comment', name: '注释绿' }
};

/* 语句 -> 分类：画布上的积木按"它属于积木箱哪个分类"上色。
 * 所以 float price = 0.0 是变量绿、int t = a + b > c 是运算青、
 * int n = LEN(s) 是字符串黄绿、int 按钮 = button(...) 是窗口品红。 */
var CALL_CAT = {
  /* 运算：Excel 风格大写 + C 风格小写，两套都认 */
  ABS: 'math', SIGN: 'math', MOD: 'math', ROUND: 'math', INT: 'math',
  POWER: 'math', SQRT: 'math', EXP: 'math', LN: 'math', LOG: 'math', PI: 'math',
  RAND: 'math', RANDBETWEEN: 'math', SUM: 'math', AVERAGE: 'math', COUNT: 'math',
  MAX: 'math', MIN: 'math',
  abs: 'math', sqrt: 'math', pow: 'math', sin: 'math', cos: 'math', tan: 'math',
  asin: 'math', acos: 'math', atan: 'math', log: 'math', log10: 'math', exp: 'math',
  floor: 'math', ceil: 'math', round: 'math', random: 'math', now: 'math',
  /* 0.4.2 新增：Excel 数学大写名 + 反正切二 / 截断 */
  SIN: 'math', COS: 'math', TAN: 'math', ASIN: 'math', ACOS: 'math', ATAN: 'math',
  ATAN2: 'math', LOG10: 'math', FLOOR: 'math', CEILING: 'math', TRUNC: 'math',
  /* 字符串与数组 */
  LEN: 'text', LEFT: 'text', RIGHT: 'text', MID: 'text',
  UPPER: 'text', LOWER: 'text', TRIM: 'text', CONCAT: 'text', VALUE: 'text', VAL: 'text',
  len: 'text', at: 'text',
  FIND: 'text', SUBSTITUTE: 'text', REPT: 'text', EXACT: 'text', CHAR: 'text', CODE: 'text',
  rcl: 'misc',
  /* 判断（逻辑） */
  IF: 'if', AND: 'if', OR: 'if', NOT: 'if', TRUE: 'if', FALSE: 'if',
  /* 窗口程序（C_OPED.cash 里都是小写） */
  window: 'win', label: 'win', button: 'win', textbox: 'win', memo: 'win',
  checkbox: 'win', listbox: 'win', progress: 'win', settext: 'win', settitle: 'win',
  additem: 'win', setchecked: 'win', setvalue: 'win', readtext: 'win', checked: 'win',
  getvalue: 'win', msgbox: 'win', ask: 'win', alert: 'win', destroy: 'win',
  sleep: 'win', run: 'win', launch: 'win',
  /* CSS 美化工具箱（C_CSS.cash） */
  csswindow: 'css', cstext: 'css', csbg: 'css', csfont: 'css', csradius: 'css',
  csshadow: 'css', cshide: 'css', csshow: 'css', cspass: 'css',
  cscolor: 'css', csapply: 'css',
  /* 文件与系统（C_IO.cash） */
  readfile: 'io', writefile: 'io', appendfile: 'io', fileexists: 'io',
  makedir: 'io', appdir: 'io', envget: 'io', launchwait: 'io',
  /* 操作系统（C_OS.cash） */
  os_pid: 'os', os_cores: 'os', os_ram: 'os', os_cpu: 'os',
  os_hostname: 'os', os_platform: 'os', isadmin: 'os', uptime: 'os',
  findproc: 'os',
  /* 网络与攻防（C_NET.cash） */
  isurl: 'net', isip: 'net', urlhost: 'net', urlport: 'net',
  fnv1a: 'net', base64: 'net', unbase64: 'net', hostalive: 'net',
  bannerget: 'net', httpcode: 'net', dnsresolve: 'net',
  /* 便携化交互（C_SC.cash） */
  isportable: 'sc', copytoexe: 'sc', freebytes: 'sc', diskfree: 'sc',
  is64bit: 'sc', portcheck: 'sc', portscan: 'sc'
};

/* 语句里第一个被调用的函数名（大小写原样），比如 'button'、'SUM' */
function firstCallName(toks) {
  for (var i = 0; i + 1 < toks.length; i++) {
    if (toks[i].kind === 'ident' && toks[i + 1].kind === 'op' && toks[i + 1].text === '(') {
      return toks[i].text;
    }
  }
  return '';
}

function stmtHasOp(toks, ops) {
  for (var i = 0; i < toks.length; i++) {
    if (toks[i].kind === 'op' && ops.indexOf(toks[i].text) >= 0) return true;
  }
  return false;
}

/* ================================================================== *
 * 5.5 悬浮说明：这一格该填什么
 * ================================================================== */

/* 从 toks[ti] 往回找最近的、还没配对的 '(' —— 知道括号里是第几个参数 */
function parenInfo(toks, ti) {
  var depth = 0;
  for (var i = ti - 1; i >= 0; i--) {
    var t = toks[i];
    if (t.kind !== 'op') continue;
    if (t.text === ')') depth++;
    else if (t.text === '(') {
      if (depth) { depth--; continue; }
      var name = '';
      if (i - 1 >= 0 && toks[i - 1].kind === 'ident' && !KW[toks[i - 1].text]) {
        name = toks[i - 1].text;             /* 括号前面那个名字 = 被调用的函数 */
      }
      var arg = 0, d = 0;
      for (var j = i + 1; j < ti && j < toks.length; j++) {
        var u = toks[j];
        if (u.kind !== 'op') continue;
        if (u.text === '(' || u.text === '[') d++;
        else if (u.text === ')' || u.text === ']') d--;
        else if (u.text === ',' && !d) arg++;
      }
      return { open: i, name: name, arg: arg };
    }
  }
  return null;
}

/* 这个槽位（黄色小格子）该填什么 */
function slotTip(s, part, slotIndex) {
  var toks = (part && part.toks) || [];
  var slot = part && part.slots ? part.slots[slotIndex] : null;
  if (!slot) return null;
  if (slot.kind === 'comment') {
    return { title: '注释', sig: '// …… 或 /* …… */',
             note: '这是注释，不会参与运行。点它把文字改掉；写的是 "// 开头" 或 "/* 开头 */"。' };
  }
  var ti = -1;
  for (var i = 0; i < toks.length; i++) {
    if (toks[i].start === slot.start) { ti = i; break; }
  }
  var kindWord = slot.kind === 'str' ? '一段文字（要带双引号，比如 "你好"）'
               : slot.kind === 'chr' ? "一个字符（要带单引号，比如 'a'）"
               : slot.kind === 'num' ? '一个数字'
               : '一个名字（变量名、算式都行）';
  var isDef = (s.type === 'function');
  var nameSlot = ti >= 0 && toks[ti + 1] && toks[ti + 1].kind === 'op' && toks[ti + 1].text === '(';
  var fnSig = SYN && SYN.KW['function'] ? SYN.KW['function'].sig : 'function 名字(参数) { }';

  if (isDef && nameSlot) {
    return { title: '函数名', sig: fnSig,
             note: '给这个函数起个名字（名字随便取，别和内置函数重名）。' };
  }
  if (isDef && parenInfo(toks, ti)) {
    return { title: '参数：类型 + 名字',
             sig: fnSig,
             note: '比如 int n，或 string 名字；多个参数用逗号隔开，没有参数就把括号留空。' };
  }
  var info = nameSlot ? { open: ti + 1, name: slot.text, arg: 0 } : (ti >= 0 ? parenInfo(toks, ti) : null);
  if (info && info.name) {
    var e = SYN ? SYN.lookup(info.name) : null;
    var labels = SYN ? SYN.argLabels(info.name) : null;
    var label = (labels && labels[info.arg]) || ('第 ' + (info.arg + 1) + ' 个参数');
    if (nameSlot) {                          /* 悬停的是函数名本身 */
      return { title: '要用的函数', sig: e ? e.sig : (info.name + '(…)'),
               note: e ? e.note
                       : ('程序里自己写的函数；得先有 "function ' + info.name + '(参数)" 那样的定义。') };
    }
    if (e) {
      return { title: label, sig: e.sig, note: e.note + '　这一格要填：' + kindWord + '。' };
    }
    return { title: label, sig: info.name + '(…)',
             note: '按你定义 ' + info.name + ' 时参数的顺序填。这一格要填：' + kindWord + '。' };
  }
  var lab = SYN ? SYN.argLabels(s.type) : null;
  var head = toks[0] && toks[0].kind === 'ident' ? toks[0].text : '';
  var he = head && SYN ? SYN.lookup(head) : null;
  var title = (lab && (lab[slotIndex] || lab[lab.length - 1])) || '这一格可以改';
  return { title: title, sig: he ? he.sig : '', note: '这一格要填：' + kindWord + '。' };
}

/* 空括号 / 括号末尾的小加号：这里能加什么 */
function holeTip(s, part, ti, mode) {
  var toks = (part && part.toks) || [];
  var info = ti >= 0 ? parenInfo(toks, ti) : null;
  var name = (info && info.name) || '';
  var e = name && SYN ? SYN.lookup(name) : null;
  var labels = name && SYN ? SYN.argLabels(name) : null;
  if (!labels && SYN) labels = SYN.argLabels(s.type);
  if (s.type === 'function') {
    var ke = SYN && SYN.KW['function'] ? SYN.KW['function'] : null;
    return { title: mode === 'add' ? '再加一个参数' : '括号里加参数',
             sig: ke ? ke.sig : 'function 名字(参数) { }',
             note: '参数的写法是「类型 名字」，比如 int n；多个参数用逗号隔开。' };
  }
  var idx = (mode === 'add' && info) ? info.arg + 1 : 0;
  var what = labels ? (labels[idx] || '') : '';
  var note;
  if (!labels) {
    note = '括号里可以填内容，多个用逗号隔开。';
  } else if (what === '…' || (what === '' && labels[labels.length - 1] === '…')) {
    what = '';
    note = '还能接着加，用逗号隔开，个数不限。';
  } else if (!what) {
    note = '说明里这个函数只用 ' + labels.length + ' 个参数，再多写一个就要报错了。';
  } else {
    note = '这一格要填：' + what + (e ? '。' + e.note : '。');
  }
  return {
    title: mode === 'add' ? '后面还能再加一个' : '点这里往括号里填内容',
    sig: e ? e.sig : (name ? name + '(…)' : ''),
    note: note
  };
}

function stmtCat(s) {
  if (!s || s.type === 'empty') return 'misc';
  if (s.type === 'comment') return 'comment';
  var toks = (s.parts && s.parts[0] && s.parts[0].toks) || [];
  switch (s.type) {
    case 'print': case 'show': case 'input': return 'out';
    case 'if': case 'switch': case 'case': case 'default': case 'else': return 'if';
    case 'while': case 'for': case 'until': case 'do': case 'foreach':
    case 'break': case 'continue': return 'loop';
    case 'function': case 'return': return 'fn';
    case 'use': {
      /* use "某.cash" 归到对应分组；别的头文件归 misc */
      for (var ui = 0; ui < toks.length; ui++)
        if (toks[ui].kind === 'str') {
          var hh = toks[ui].text;
          if (hh.indexOf('C_CSS') >= 0) return 'css';
          if (hh.indexOf('C_OPED') >= 0) return 'win';
          if (hh.indexOf('C_IO') >= 0) return 'io';
          if (hh.indexOf('C_OS') >= 0) return 'os';
          if (hh.indexOf('C_NET') >= 0) return 'net';
          if (hh.indexOf('C_SC') >= 0) return 'sc';
          return 'misc';
        }
      return 'win';
    }
    case 'start': return 'entry';
    case 'block': return 'block';
    case 'decl': {
      var c = firstCallName(toks);
      if (c && CALL_CAT[c]) return CALL_CAT[c];
      var head = toks[0] ? toks[0].text : '';
      if (head === 'string' || head === 'char' || head === 'text' || head === 'letter') return 'text';
      if (stmtHasOp(toks, ['+', '-', '*', '/', '%'])) return 'math';
      if (stmtHasOp(toks, ['>', '<', '>=', '<=', '==', '!=', '&&', '||'])) return 'if';
      return 'var';
    }
    case 'expr': {
      var c2 = firstCallName(toks);
      if (c2 && CALL_CAT[c2]) return CALL_CAT[c2];
      if (stmtHasOp(toks, ['=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^='])) return 'var';
      if (stmtHasOp(toks, ['++', '--'])) return 'var';
      if (c2) return 'fn';
      return 'misc';
    }
  }
  return 'misc';
}

/* 分类 -> 画布积木的 CSS 颜色类 */
function blockClassOf(s) {
  var cat = stmtCat(s);
  if (cat === 'entry') return 'c-entry';
  if (cat === 'block') return 'c-block';
  if (cat === 'other') return 'c-other';
  return (CAT_COLOR[cat] || CAT_COLOR.misc).cls;
}

/* cat: out 蓝 / var 绿 / if 橙 / loop 紫 / fn 红 / data 青 / misc 灰 */
var PALETTE = [
  /* ---- 输出输入 ---- */
  { cat: 'out', name: '打印一行',        hint: 'print "你好";',            code: 'print "你好";\n' },
  { cat: 'out', name: '打印（不换行）',  hint: 'show "...";',              code: 'show "...";\n' },
  { cat: 'out', name: '打印多个值',      hint: 'print "得分是 ", score;',  code: 'print "得分是 ", score;\n' },
  { cat: 'out', name: '打印一个变量',    hint: 'print value;',             code: 'print value;\n' },
  { cat: 'out', name: '打印算式',        hint: 'print a + b;',             code: 'print a + b;\n' },
  { cat: 'out', name: '打印一个空行',    hint: 'print "";',                code: 'print "";\n' },
  { cat: 'out', name: '读入一个值',      hint: 'input value;',             code: 'input value;\n' },
  { cat: 'out', name: '提示并输入',      hint: '先显示提示，再读入',        code: 'print "请输入：";\ninput value;\n' },

  /* ---- 变量数据 ---- */
  { cat: 'var', name: '整数变量',        hint: 'int count = 0;',        code: 'int count = 0;\n' },
  { cat: 'var', name: '小数变量',        hint: 'float price = 0.0;',        code: 'float price = 0.0;\n' },
  { cat: 'var', name: '文字变量',        hint: 'string name = "";',          code: 'string name = "";\n' },
  { cat: 'var', name: '真假变量',        hint: 'bool ok = true;',          code: 'bool ok = true;\n' },
  { cat: 'var', name: '字符变量',        hint: "char ch = 'a';",         code: "char ch = 'a';\n" },
  { cat: 'var', name: '只声明先不赋值',  hint: 'int total;',            code: 'int total;\n' },
  { cat: 'var', name: '赋值',            hint: 'count = 1;',               code: 'count = 1;\n' },
  { cat: 'var', name: '变量加 1',        hint: 'count = count + 1;',       code: 'count = count + 1;\n' },
  { cat: 'var', name: '变量减 1',        hint: 'count = count - 1;',       code: 'count = count - 1;\n' },
  { cat: 'var', name: '累加计分 +=',     hint: 'count += a;',              code: 'count += a;\n' },
  { cat: 'var', name: '常量（定死的数）', hint: 'constant int N = 5;',   code: 'constant int N = 5;\n' },

  /* ---- 运算 ---- */
  { cat: 'math', name: '四则运算',       hint: 'int t = a + b * c;',    code: 'int t = a + b * c;\n' },
  { cat: 'math', name: '取余数 MOD',     hint: 'int r = MOD(a, 10);',   code: 'int r = MOD(a, 10);\n' },
  { cat: 'math', name: '四舍五入 ROUND', hint: 'int n = ROUND(x, 2);',  code: 'int n = ROUND(x, 2);\n' },
  { cat: 'math', name: '向下取整 INT',   hint: 'int n = INT(x);',       code: 'int n = INT(x);\n' },
  { cat: 'math', name: '绝对值 ABS',     hint: 'int n = ABS(x);',       code: 'int n = ABS(x);\n' },
  { cat: 'math', name: '乘方 POWER',     hint: 'float n = POWER(x, 2);',    code: 'float n = POWER(x, 2);\n' },
  { cat: 'math', name: '开平方 SQRT',    hint: 'float y = SQRT(x);',        code: 'float y = SQRT(x);\n' },
  { cat: 'math', name: '最大值 MAX',     hint: 'int m = MAX(a, b);',    code: 'int m = MAX(a, b);\n' },
  { cat: 'math', name: '正负号 SIGN',    hint: 'int r = SIGN(x);',      code: 'int r = SIGN(x);\n' },
  { cat: 'math', name: '正弦 SIN',        hint: 'float y = SIN(x);',     code: 'float y = SIN(x);\n' },
  { cat: 'math', name: '反正切二 ATAN2',  hint: 'float a = ATAN2(y, x);', code: 'float a = ATAN2(y, x);\n' },

  /* ---- 判断 ---- */
  { cat: 'if', name: '如果 ...',                 hint: 'if (x > 0) { }',                 code: 'if (x > 0) {\n}\n' },
  { cat: 'if', name: '如果 / 否则',              hint: 'if ... else ...',                 code: 'if (x > 0) {\n} else {\n}\n' },
  { cat: 'if', name: '如果 / 否则如果 / 否则',   hint: 'if ... else if ... else ...',     code: 'if (x > 0) {\n} else if (x < 0) {\n} else {\n}\n' },
  { cat: 'if', name: 'switch 分支',              hint: 'switch (值) { case 1: }',         code: 'switch (值) {\n    case 1:\n}\n' },
  { cat: 'if', name: 'case 分支值',              hint: 'case 1: …',                       code: 'case 1: ' },
  { cat: 'if', name: 'default 默认',             hint: 'default: …',                      code: 'default: ' },
  { cat: 'if', name: '比较结果存起来',           hint: 'bool ok = x > 0;',                code: 'bool ok2 = x > 0;\n' },
  { cat: 'if', name: '相等吗',                   hint: 'bool same = (a == b);',           code: 'bool same = (a == b);\n' },
  { cat: 'if', name: '两个都要成立 AND',          hint: 'bool both = … and …',             code: 'bool both = (x > 0) and (x < 10);\n' },
  { cat: 'if', name: '有一个成立 OR',             hint: 'bool one = … or …',               code: 'bool one = (x > 0) or (x < 0);\n' },
  { cat: 'if', name: '反过来 NOT',               hint: 'bool no = not (...);',            code: 'bool no = not (x > 0);\n' },
  { cat: 'if', name: '如果两个都成立',           hint: 'if (a and b) { }',                code: 'if (x > 0 and x < 10) {\n}\n' },
  { cat: 'if', name: '如果反过来',               hint: 'if (not (...)) { }',              code: 'if (not (x > 0)) {\n}\n' },

  /* ---- 循环 ---- */
  { cat: 'loop', name: '当 ... 的时候（循环）',   hint: 'while (x < 10) { }',              code: 'while (x < 10) {\n    x = x + 1;\n}\n' },
  { cat: 'loop', name: '一直重复',               hint: 'while (true) { }',                code: 'while (true) {\n}\n' },
  { cat: 'loop', name: '重复 10 次',             hint: 'for (int i = 0; i < 10; i++)',  code: 'for (int i = 0; i < 10; i++) {\n}\n' },
  { cat: 'loop', name: '从 1 数到 10',           hint: 'for (int i = 1; i <= 10; i++)', code: 'for (int i = 1; i <= 10; i++) {\n}\n' },
  { cat: 'loop', name: 'until 直到',             hint: 'until (条件) { }（条件为真就停）', code: 'until (条件) {\n}\n' },
  { cat: 'loop', name: 'do…while 至少做一次',    hint: 'do { } while (条件);',             code: 'do {\n} while (条件);\n' },
  { cat: 'loop', name: 'foreach 遍历数组',       hint: 'foreach (元素 in 数组) { }',       code: 'foreach (元素 in 数组) {\n}\n' },
  { cat: 'loop', name: '跳出循环',               hint: 'break;',                          code: 'break;\n' },
  { cat: 'loop', name: '下一次循环',             hint: 'continue;',                       code: 'continue;\n' },

  /* ---- 函数 ---- */
  { cat: 'fn', name: '定义函数',              hint: 'function hello() { }',                        code: 'function hello() {\n}\n' },
  { cat: 'fn', name: '自定义函数（手动定义语法）',
    hint: 'function 函数名() { }：点名字格改名，括号里点 ＋ 加参数，再写函数体',
    code: 'function 函数名() {\n}\n' },
  { cat: 'fn', name: '定义函数（带参数）',    hint: 'function add(int a, int b) { }',        code: 'function add(int a, int b) {\n    return a + b;\n}\n' },
  { cat: 'fn', name: '返回数字的函数',        hint: 'int double(int x) { }',                 code: 'int double(int x) {\n    return x * 2;\n}\n' },
  { cat: 'fn', name: '无返回值的函数',        hint: 'none greet(string who) { }',                    code: 'none greet(string who) {\n    print who, "!";\n}\n' },
  { cat: 'fn', name: '调用函数',              hint: 'hello();',                                    code: 'hello();\n' },
  { cat: 'fn', name: '调用并打印',            hint: 'print add(1, 2);',                            code: 'print add(1, 2);\n' },
  { cat: 'fn', name: '返回结果',              hint: 'return 0;',                                   code: 'return 0;\n' },

  /* ---- 字符串与数组 ---- */
  { cat: 'text', name: '文字连接 +',           hint: 'string s = a + b;',            code: 'string s2 = s + t;\n' },
  { cat: 'text', name: '连接 CONCAT',          hint: 'CONCAT(a, b)',               code: 'string s3 = CONCAT(s, name);\n' },
  { cat: 'text', name: '文字长度 LEN',         hint: 'LEN(s)',                     code: 'int n6 = LEN(s);\n' },
  { cat: 'text', name: '取左边 LEFT',          hint: 'LEFT(s, 3)',                 code: 'string t2 = LEFT(s, 3);\n' },
  { cat: 'text', name: '取右边 RIGHT',         hint: 'RIGHT(s, 3)',                code: 'string t3 = RIGHT(s, 3);\n' },
  { cat: 'text', name: '截取 MID',             hint: 'MID(s, 2, 3)',               code: 'string t4 = MID(s, 2, 3);\n' },
  { cat: 'text', name: '变大写 UPPER',         hint: 'UPPER(s)',                   code: 'string t5 = UPPER(s);\n' },
  { cat: 'text', name: '变小写 LOWER',         hint: 'LOWER(s)',                   code: 'string t6 = LOWER(s);\n' },
  { cat: 'text', name: '去掉两边空格 TRIM',    hint: 'TRIM(s)',                    code: 'string t7 = TRIM(s);\n' },
  { cat: 'text', name: '取第几个字 at()',      hint: 'at(s, 1)',                   code: 'char c2 = at(s, 1);\n' },
  { cat: 'text', name: '求和 SUM',             hint: 'SUM(数组)',                  code: 'int total3 = SUM(arr);\n' },
  { cat: 'text', name: '平均 AVERAGE',         hint: 'AVERAGE(数组)',              code: 'float avg2 = AVERAGE(arr);\n' },
  { cat: 'text', name: '最大 MAX',             hint: 'MAX(数组)',                  code: 'int m3 = MAX(arr);\n' },
  { cat: 'text', name: '最小 MIN',             hint: 'MIN(数组)',                  code: 'int m4 = MIN(arr);\n' },
  { cat: 'text', name: '个数 COUNT',           hint: 'COUNT(数组)',                code: 'int n7 = COUNT(arr);\n' },
  { cat: 'text', name: '条件取值 IF',          hint: 'IF(条件, 真值, 假值)',        code: 'int v2 = IF(score >= 60, 1, 0);\n' },
  { cat: 'text', name: '圆周率 PI',            hint: 'PI()',                       code: 'float p2 = PI();\n' },
  { cat: 'text', name: '随机数 RAND',          hint: 'RAND()',                     code: 'float r4 = RAND();\n' },
  { cat: 'text', name: '随机整数 RANDBETWEEN', hint: 'RANDBETWEEN(1, 10)',         code: 'int n8 = RANDBETWEEN(1, 10);\n' },
  { cat: 'text', name: '整数数组',             hint: 'int arr[5];',             code: 'int arr[5];\n' },
  { cat: 'text', name: '文字数组',             hint: 'string names[3];',             code: 'string names[3];\n' },
  { cat: 'text', name: '数组整表初始化',       hint: 'int arr[5] = {…};',       code: 'int arr[5] = {10, 20, 30, 40, 50};\n' },
  { cat: 'text', name: '二维数组',             hint: 'int t[3][4];',            code: 'int table[3][4];\n' },
  { cat: 'text', name: '找位置 FIND',          hint: 'int p = FIND(文字, 要找的);', code: 'int p6 = FIND(文字, 要找的);\n' },
  { cat: 'text', name: '替换 SUBSTITUTE',      hint: 'SUBSTITUTE(文字, 旧, 新)', code: 'string s8 = SUBSTITUTE(文字, "旧", "新");\n' },
  { cat: 'text', name: '重复 REPT',            hint: 'REPT(文字, 次数)',       code: 'string s9 = REPT("ab", 3);\n' },
  { cat: 'text', name: '完全一样吗 EXACT',     hint: 'bool eq = EXACT(a, b);', code: 'bool eq = EXACT(a, b);\n' },

  /* ---- 窗口程序（C_OPED.cash 打开 C / C++ / 原生窗口世界） ---- */
  { cat: 'win', name: '创建窗口',       hint: 'int 窗口 = window("标题", 460, 260);', code: 'int 窗口 = window("我的窗口", 460, 260);\n' },
  { cat: 'win', name: '标签',           hint: 'int 标签 = label("文字", x, y, 宽, 高);', code: 'int 标签 = label("你好", 20, 20, 200, 24);\n' },
  { cat: 'win', name: '按钮',           hint: 'int 按钮 = button("点我", x, y, 宽, 高);', code: 'int 按钮 = button("点我", 20, 60, 120, 32);\n' },
  { cat: 'win', name: '单行输入框',     hint: 'int 输入框 = textbox("", x, y, 宽, 高);', code: 'int 输入框 = textbox("", 20, 100, 220, 26);\n' },
  { cat: 'win', name: '多行文本区',     hint: 'int 文本区 = memo("", x, y, 宽, 高);',  code: 'int 文本区 = memo("", 20, 140, 300, 80);\n' },
  { cat: 'win', name: '复选框',         hint: 'int 勾选 = checkbox("同意", x, y, 宽, 高);', code: 'int 勾选 = checkbox("我同意", 20, 230, 220, 24);\n' },
  { cat: 'win', name: '列表',           hint: 'int 列表 = listbox(x, y, 宽, 高);',    code: 'int 列表 = listbox(20, 20, 200, 120);\n' },
  { cat: 'win', name: '进度条',         hint: 'int 进度 = progress(x, y, 宽, 高);',   code: 'int 进度 = progress(20, 20, 300, 20);\n' },
  { cat: 'win', name: '显示／读入文字', hint: 'settext(控件, "文字"); readtext(控件);', code: 'settext(标签, "新的文字");\n' },
  { cat: 'win', name: '读输入框的文字', hint: 'string s = readtext(输入框);',        code: 'string 内容 = readtext(输入框);\n' },
  { cat: 'win', name: '列表加一项',     hint: 'additem(列表, "第一项");',            code: 'additem(列表, "第一项");\n' },
  { cat: 'win', name: '勾选状态',       hint: 'if (checked(勾选)) { }',              code: 'if (checked(勾选)) {\n}\n' },
  { cat: 'win', name: '点击事件',       hint: 'function on_click(int 控件) { }',     code: 'function on_click(int 控件) {\n}\n' },
  { cat: 'win', name: '内容改变事件',   hint: 'function on_change(int 控件) { }',    code: 'function on_change(int 控件) {\n}\n' },
  { cat: 'win', name: '关闭事件',       hint: 'function on_close() { }',             code: 'function on_close() {\n}\n' },
  { cat: 'win', name: '弹窗提示 alert', hint: 'alert("你好");',                      code: 'alert("你好");\n' },
  { cat: 'win', name: '弹窗带标题',     hint: 'msgbox("内容", "标题");',             code: 'msgbox("内容", "标题");\n' },
  { cat: 'win', name: '是／否询问',     hint: 'int 答案 = ask("要保存吗？", "询问");', code: 'int 答案 = ask("要保存吗？", "询问");\n' },
  { cat: 'win', name: '延时',           hint: 'sleep(500);',                         code: 'sleep(500);\n' },
  { cat: 'win', name: '启动窗口',       hint: 'run();',                              code: 'run();\n' },
  { cat: 'win', name: '关闭控件／窗口', hint: 'destroy(窗口);',                      code: 'destroy(窗口);\n' },
  { cat: 'win', name: '启动外部程序',   hint: 'launch("notepad.exe");',             code: 'launch("notepad.exe");\n' },

  /* ---- CSS 美化（C_CSS.cash，要配 C_OPED.cash 一起 use） ---- */
  { cat: 'css', name: '文字颜色',        hint: 'cstext(控件, "RRGGBB");',  code: 'cstext(标签, "00C853");\n' },
  { cat: 'css', name: '背景颜色',        hint: 'csbg(控件, "RRGGBB");',    code: 'csbg(输入框, "FFF9C4");\n' },
  { cat: 'css', name: '字号粗细',        hint: 'csfont(控件, 字号, 加粗);', code: 'csfont(标题, 20, 1);\n' },
  { cat: 'css', name: '圆角',           hint: 'csradius(控件, 像素);',     code: 'csradius(按钮, 8);\n' },
  { cat: 'css', name: '投影',           hint: 'csshadow(控件, 0~3);',     code: 'csshadow(窗口, 1);\n' },
  { cat: 'css', name: '密码打码',        hint: 'cspass(输入框);',          code: 'cspass(密码框);\n' },
  { cat: 'css', name: '隐藏／显示',      hint: 'cshide(控件) / csshow(控件);', code: 'cshide(标签);\ncsshow(标签);\n' },
  { cat: 'css', name: '起个样式名',      hint: 'cscolor(名字, "RRGGBB");',  code: 'cscolor("主题", "1E88E5");\n' },
  { cat: 'css', name: '涂到控件上',      hint: 'csapply(名字, 控件);',      code: 'csapply("主题", 按钮);\n' },
  { cat: 'css', name: '打开 CSS 工具箱', hint: 'use "C_CSS.cash";',         code: 'use "C_CSS.cash";\n' },

  { cat: 'misc', name: '程序入口',   hint: 'function main() { }',      code: 'function main() {\n}\n' },
  { cat: 'misc', name: '安装窗口插件', hint: 'use "C_OPED.cash";',      code: 'use "C_OPED.cash";\n' },
  /* ---- 文件与系统（C_IO.cash） ---- */
  { cat: 'io', name: '安装文件系统插件', hint: 'use "C_IO.cash";',            code: 'use "C_IO.cash";\n' },
  { cat: 'io', name: '读整个文件',     hint: 'readfile(路径)',              code: '内容 = readfile("config.txt");\n' },
  { cat: 'io', name: '写整个文件',     hint: 'writefile(路径, 内容);',       code: 'writefile("config.txt", 内容);\n' },
  { cat: 'io', name: '追加写入',       hint: 'appendfile(路径, 内容);',      code: 'appendfile("log.txt", "一行记录" + "\\n");\n' },
  { cat: 'io', name: '文件存在？',     hint: 'fileexists(路径)',            code: 'if (fileexists("config.txt") != 0) {\n}\n' },
  { cat: 'io', name: '建目录',         hint: 'makedir(路径);',              code: 'makedir("save");\n' },
  { cat: 'io', name: '程序所在目录',   hint: 'appdir()',                   code: '路径 = appdir();\n' },
  { cat: 'io', name: '读环境变量',     hint: 'envget("名字")',              code: 'v = envget("PATH");\n' },
  { cat: 'io', name: '启动并等它退出', hint: 'launchwait(命令);',           code: '退出码 = launchwait("cmd /c pause");\n' },

  /* ---- 操作系统（C_OS.cash） ---- */
  { cat: 'os', name: '安装操作系统插件', hint: 'use "C_OS.cash";',          code: 'use "C_OS.cash";\n' },
  { cat: 'os', name: '本程序进程号',    hint: 'int p = os_pid();',         code: 'int 进程号 = os_pid();\n' },
  { cat: 'os', name: 'CPU 核数',        hint: 'int n = os_cores();',       code: 'int 核数 = os_cores();\n' },
  { cat: 'os', name: '内存（GB）',      hint: 'int g = os_ram();',         code: 'int 内存GB = os_ram();\n' },
  { cat: 'os', name: 'CPU 型号',        hint: 'string c = os_cpu();',      code: 'string 型号 = os_cpu();\n' },
  { cat: 'os', name: '主机名',          hint: 'string h = os_hostname();', code: 'string 主机 = os_hostname();\n' },
  { cat: 'os', name: '平台',            hint: 'string p = os_platform();', code: 'string 平台 = os_platform();\n' },
  { cat: 'os', name: '是管理员吗',     hint: 'if (isadmin()) { }',         code: 'if (isadmin() == 1) {\n}\n' },
  { cat: 'os', name: '开机毫秒数',      hint: 'int ms = uptime();',        code: 'int 毫秒 = uptime();\n' },
  { cat: 'os', name: '找进程',          hint: 'if (findproc("名称") != 0) { }', code: 'if (findproc("notepad") != 0) {\n}\n' },

  /* ---- 网络与攻防（C_NET.cash） ---- */
  { cat: 'net', name: '安装网络插件',   hint: 'use "C_NET.cash";',         code: 'use "C_NET.cash";\n' },
  { cat: 'net', name: '是网址吗',       hint: 'if (isurl(文字) != 0) { }', code: 'if (isurl("https://a.com") != 0) {\n}\n' },
  { cat: 'net', name: '是 IP 吗',       hint: 'if (isip(文字) != 0) { }',  code: 'if (isip("1.2.3.4") != 0) {\n}\n' },
  { cat: 'net', name: '取网址的主机',   hint: 'urlhost("https://a.com/x")', code: 'string 主机 = urlhost("https://a.com/x");\n' },
  { cat: 'net', name: '取网址的端口',   hint: 'urlport("https://a.com")',  code: 'int 端口 = urlport("https://a.com");\n' },
  { cat: 'net', name: '内容哈希',       hint: 'int h = fnv1a(文字);',      code: 'int 哈希 = fnv1a(口令);\n' },
  { cat: 'net', name: 'base64 编码',    hint: 'string e = base64(文字);', code: 'string 密 = base64(原文);\n' },
  { cat: 'net', name: 'base64 解码',    hint: 'string d = unbase64(文字);', code: 'string 明 = unbase64(密);\n' },
  { cat: 'net', name: '主机在线吗',     hint: 'if (hostalive(主机, 端口) != 0) { }', code: 'if (hostalive("127.0.0.1", 8080) != 0) {\n}\n' },
  { cat: 'net', name: '抓服务横幅',     hint: 'string b = bannerget(主机, 端口, 探测串);', code: 'string 横幅 = bannerget("127.0.0.1", 21, "help\r\n");\n' },
  { cat: 'net', name: 'HTTP 状态码',    hint: 'int c = httpcode(网址);', code: 'int 状态码 = httpcode("http://127.0.0.1:8000/");\n' },
  { cat: 'net', name: '解析主机名',     hint: 'string ip = dnsresolve(主机);', code: 'string ip = dnsresolve("a.com");\n' },

  /* ---- 便携化交互（C_SC.cash） ---- */
  { cat: 'sc', name: '安装便携插件',    hint: 'use "C_SC.cash";',          code: 'use "C_SC.cash";\n' },
  { cat: 'sc', name: '能就地存文件吗',  hint: 'if (isportable() != 0) { }', code: 'if (isportable() != 0) {\n}\n' },
  { cat: 'sc', name: '写到程序旁边',    hint: 'copytoexe(名, 内容);',     code: 'copytoexe("save.txt", "内容");\n' },
  { cat: 'sc', name: '空闲内存（字节）', hint: 'int b = freebytes();',     code: 'int 空闲 = freebytes();\n' },
  { cat: 'sc', name: '磁盘空闲（字节）', hint: 'int b = diskfree(路径);',  code: 'int 空闲 = diskfree("C:");\n' },
  { cat: 'sc', name: '是 64 位吗',      hint: 'if (is64bit() != 0) { }',  code: 'if (is64bit() != 0) {\n}\n' },
  { cat: 'sc', name: '端口开着吗',      hint: 'if (portcheck(端口) != 0) { }', code: 'if (portcheck(8080) != 0) {\n}\n' },
  { cat: 'sc', name: '数开着的端口',    hint: 'portscan(起, 止);',        code: 'int 个数 = portscan(80, 89);\n' },

  { cat: 'misc', name: '安装 CSS 插件', hint: 'use "C_CSS.cash";',     code: 'use "C_CSS.cash";\n' },
  { cat: 'misc', name: '引入头文件', hint: 'use "my_helper.h";',       code: 'use "my_helper.h";\n' },
  { cat: 'misc', name: '空语句',     hint: ';',                        code: ';\n' },
  { cat: 'misc', name: '注释一行',   hint: '// 说明文字',               code: '// 说明文字\n' }
];

/* ================================================================== *
 * 6. 导出（浏览器 + node）
 * ================================================================== */

var Blocks = {
  init: function () {},       /* 浏览器里由 UI 覆盖 */
  refresh: function () {},
  showError: function () {},
  _internals: {
    parse: parse,
    serialize: serialize,
    splice: splice,
    editSlot: editSlot,
    deleteRange: deleteRange,
    moveRange: moveRange,
    insertSnippet: insertSnippet,
    insertTemplate: insertTemplate,
    lineOf: lineOf,
    lineIndentAt: lineIndentAt,
    insertPosAfterStmt: insertPosAfterStmt,
    insertPosBeforeStmt: insertPosBeforeStmt,
    indentSnippet: indentSnippet,
    lexHead: lexHead,
    readToken: readToken,
    skipWs: skipWs,
    findCloseBrace: findCloseBrace,
    classifyHead: classifyHead,
    PALETTE: PALETTE,
    CATEGORIES: CATEGORIES,
    CAT_COLOR: CAT_COLOR,
    HEADER_GATED: HEADER_GATED,
    scanHeaders: scanHeaders,
    isCatUnlocked: isCatUnlocked,
    stmtCat: stmtCat,
    blockClassOf: blockClassOf,
    parenInfo: parenInfo,
    slotTip: slotTip,
    holeTip: holeTip,
    CALL_CAT: CALL_CAT,
    firstCallName: firstCallName,
    KEYWORDS: KW
  }
};


/* ================================================================== *
 * 7. DOM 层（只在浏览器里初始化；node 里这段不会执行）
 * ================================================================== */

var UI = (typeof window === 'undefined' || typeof document === 'undefined') ? null : (function () {

  var CSS_TEXT = [
    '.blk-canvas{background:#1e1f26;color:#e8eaf0;overflow:auto;position:relative;height:100%;',
    '  font-family:-apple-system,"Microsoft YaHeiUI",sans-serif;font-size:13px;user-select:none}',
    '.blk-tip{margin:8px 10px;padding:6px 10px;background:#2a2c36;border:1px dashed #4a5064;',
    '  border-radius:8px;color:#9aa3b5;font-size:12px}',
    '.blk-root{position:relative;padding:2px 12px 48px}',
    '.blk-wrap{margin:0 0 6px}',
    '.blk-chain .blk{margin-bottom:0}',
    '.blk-chain .blk:not(:first-child){border-top-left-radius:0;border-top-right-radius:0}',
    '.blk{--c:#888;--bg:#444;display:flex;flex-direction:column;background:var(--bg);color:#fff;',
    '  border-radius:8px;width:fit-content;max-width:100%;box-shadow:0 1px 3px rgba(0,0,0,.45)}',
    '.blk.has-body{border-left:12px solid var(--c)}',
    '.blk:not(.has-body){border-left:5px solid var(--c)}',
    '.blk-head{display:flex;flex-wrap:wrap;align-items:center;gap:4px;padding:5px 10px;min-height:24px;cursor:grab}',
    '.blk-body{padding:4px 8px 6px 6px;margin-left:4px;min-height:20px;display:flex;flex-direction:column}',
    '.blk-foot{border-top:1px solid rgba(255,255,255,.18)}',
    '.blk-empty{color:rgba(255,255,255,.45);font-size:11px;padding:2px 4px}',
    '.kw{font-weight:600}',
    '.pn{opacity:.75;font-family:Consolas,monospace}',
    '.slot{background:#ffe9a8;color:#241f10;border-radius:4px;padding:0 6px;',
    '  font-family:Consolas,monospace;cursor:text;min-width:8px}',
    '.slot:hover{background:#ffdb70}',
    '.slot-add{background:transparent;color:#ffd54a;border:1px dashed rgba(255,213,74,.55);',
    '  font-weight:700;padding:0 4px;font-size:11px;min-width:0;opacity:.5}',
    '.blk:hover .slot-add{opacity:1}',
    '.slot-add:hover{background:#ffd54a;color:#3a2c00;border-color:#ffd54a;opacity:1}',
    /* 空括号里的 ＋：一直亮着，点它就能加参数 */
    '.slot-blank{background:#ffd54a;color:#3a2c00;border:0;opacity:.92;font-size:12px}',
    '.slot-blank:hover{background:#ffdf6b;opacity:1}',
    '.blk-tipcard{position:fixed;z-index:1200;max-width:320px;padding:6px 9px 7px 9px;',
    '  background:#2b2d38;border:1px solid #4a5064;border-left:3px solid #ffd54a;border-radius:6px;',
    '  color:#dfe3ec;font-family:-apple-system,"Microsoft YaHeiUI",sans-serif;font-size:12px;',
    '  line-height:17px;box-shadow:0 6px 18px rgba(0,0,0,.5);pointer-events:none}',
    '.blk-tipcard .tc-title{font-weight:700;color:#ffe08a}',
    '.blk-tipcard .tc-sig{font-family:Consolas,monospace;color:#9cd2ff;white-space:pre-wrap;margin-top:2px}',
    '.blk-tipcard .tc-note{color:#a7aebb;margin-top:2px}',
    '.blk-wrap.sel>.blk{outline:2px solid #ffd54a;outline-offset:1px}',
    '.blk-wrap.ph>.blk{background:transparent!important;box-shadow:none;',
    '  border:2px dashed #7d8698!important;border-left-width:14px!important}',
    '.blk-wrap.ph .blk>*{visibility:hidden}',
    '.blk-wrap.blk-land>.blk{animation:blkland .12s ease-out}',
    '@keyframes blkland{from{opacity:0;transform:translateY(-6px)}to{opacity:1;transform:none}}',
    '.blk-wrap.blk-err>.blk{animation:blkerr 1.6s}',
    '@keyframes blkerr{0%,55%{box-shadow:0 0 0 3px #ff5252}100%{box-shadow:0 1px 3px rgba(0,0,0,.45)}}',
    '.blk-drop-line{position:absolute;height:3px;background:#ff5252;border-radius:2px;',
    '  box-shadow:0 0 8px rgba(255,82,82,.9);z-index:30;pointer-events:none;display:none}',
    '.blk-ghost{position:fixed;z-index:1000;pointer-events:none;opacity:.9;',
    '  box-shadow:0 10px 28px rgba(0,0,0,.55);border-radius:8px;transform:translate3d(-999px,-999px,0)}',
    '.blk-input{position:fixed;z-index:1100;background:#ffe9a8;color:#1a1a1a;',
    '  border:2px solid #ffb300;border-radius:4px;font:12px Consolas,monospace;',
    '  padding:1px 4px;outline:none;box-shadow:0 4px 12px rgba(0,0,0,.5)}',
    '.pal{color:#dfe3ec;font-family:-apple-system,"Microsoft YaHeiUI",sans-serif;font-size:13px;',
    '  height:100%;overflow:auto;background:#23242d;user-select:none}',
    '.pal-cat-h{display:flex;align-items:center;gap:6px;padding:7px 10px;cursor:pointer;',
    '  font-weight:600;background:#2a2c37;border-bottom:1px solid #1e1f26;position:sticky;top:0;z-index:2}',
    '.pal-cat-h .arrow{transition:transform .15s;font-size:10px;color:#9aa3b5}',
    '.pal-group.closed .arrow{transform:rotate(-90deg)}',
    '.pal-group.closed .pal-items{display:none}',
    '.pal-item{display:flex;gap:8px;align-items:center;padding:5px 10px;cursor:grab;',
    '  border-left:3px solid var(--c,transparent);background:var(--tint,transparent)}',
    '.pal-item:hover{background:#333647}',
    '.pal-dot{width:10px;height:10px;border-radius:3px;flex:none}',
    '.pal-hint{color:#8b93a5;font-size:11px;font-family:Consolas,monospace;overflow:hidden;',
    '  text-overflow:ellipsis;white-space:nowrap;max-width:10em;margin-left:auto}',
    '.c-out{--c:#4ec9b0;--bg:#1f5a4f}',
    '.c-var{--c:#569cd6;--bg:#274a74}',
    '.c-if{--c:#c586c0;--bg:#5a3a55}',
    '.c-loop{--c:#d7ba7d;--bg:#5a4a2f}',
    '.c-fn{--c:#dcdcaa;--bg:#5a5136}',
    '.c-math{--c:#b5cea8;--bg:#3d4a3a}',
    '.c-text{--c:#ce9178;--bg:#5a4038}',
    '.c-win{--c:#4fc1ff;--bg:#1f4a66}',
    '.c-css{--c:#ab7dff;--bg:#3a2f5a}',
    '.c-io{--c:#dcaa5a;--bg:#5a4a28}',
    '.c-os{--c:#5fc9b8;--bg:#2a5a50}',
    '.c-net{--c:#ff7f6e;--bg:#5a2a24}',
    '.c-sc{--c:#e2b743;--bg:#5a4a18}',
    '.c-misc{--c:#8b93a3;--bg:#474e5e}',
    '.c-comment{--c:#6a9955;--bg:#2a3325}',
    '.slot.blk-comment{font-style:italic;font-weight:600;background:#2a3325;color:#a5d6a7;',
    '  border:1px solid #6a9955;opacity:.92;padding:1px 8px}',
    '.slot.blk-comment:hover{background:#33402b;opacity:1}',
    '.c-data{--c:#b5cea8;--bg:#3d4a3a}',
    '.c-other{--c:#8b93a3;--bg:#474e5e}',
    '.c-entry{--c:#4c6ef5;--bg:#2b3fa0}',
    '.c-block{--c:#7f8aa3;--bg:#3d4457}',
    /* 未 use 的门禁分类 */
    '.pal-lock{margin-left:auto;font-size:11px;color:#ffd54a;background:#3a2c00;',
    '  border:1px solid #6a5410;border-radius:8px;padding:0 6px;font-weight:700}',
    '.pal-locked{padding:8px 12px;color:#8b93a5;font-size:11px;line-height:17px;',
    '  border-left:3px solid #6a5410;background:#2a2c36}',
    /* use 积木：块保持紧凑，概述卡片拎到右栏（在所有积木框外面），
     * 一条长线从块右边缘穿到卡片。 */
    '.blk-stage{display:flex;gap:0;position:relative;min-height:100%;align-items:flex-start}',
    '.blk-col{flex:1 1 auto;min-width:0}',
    '.blk-root{position:relative;padding:2px 8px 48px}',
    '.hdr-panel{flex:0 0 420px;width:420px;padding:8px 12px 48px;box-sizing:border-box;',
    '  display:flex;flex-direction:column;gap:14px;border-left:1px solid #2a2c36}',
    '.hdr-panel:empty{display:none}',
    '.hdr-lines{position:absolute;inset:0;pointer-events:none;z-index:5;overflow:hidden}',
    '.hdr-line{position:absolute;height:2px;border-radius:1px}',
    '.hdr-use>.blk{max-width:220px}',
    '.hdr-card{position:relative;border:1px solid #4a5064;border-left:4px solid var(--c,#888);',
    '  border-radius:8px;background:#262833;padding:8px 12px}',
    '.hdr-card .hcd-name{font-weight:700;color:#ffe08a;font-size:12px;margin-bottom:3px}',
    '.hdr-card .hcd-sum{color:#c3c9d6;font-size:11.5px;line-height:17px}',
    '.hdr-card .hcd-badge{float:right;font-size:10px;font-weight:700;padding:1px 8px;',
    '  border-radius:8px;margin-left:6px}',
    '.hdr-card.ok .hcd-badge{background:#1f5a4f;color:#4ec9b0}',
    '.hdr-card.miss .hcd-badge{background:#5a4a2f;color:#d7ba7d}',
    '.hdr-card .hdr-anchor{position:absolute;left:-6px;top:50%;width:10px;height:10px;',
    '  border-radius:50%;background:var(--c,#888);transform:translateY(-50%);',
    '  box-shadow:0 0 4px rgba(0,0,0,.5)}',
    /* 选中 / 幽灵 / 落地动画：use 积木 + 右栏卡片联动 */
    '.blk-wrap.sel>.blk{outline:2px solid #ffd54a;outline-offset:1px}',
    '.blk-wrap.ph .hdr-conn{visibility:hidden}',
  ].join('\n');

  var opts = null;                 /* {canvas, palette, getText, setText, setStatus} */
  var codeText = '';
  var ast = null;
  var idMap = new Map();
  var idSeq = 0;
  var bodyEls = [];                /* [{el, list, stmt, body}] */
  var rootEl = null, dropLine = null;
  var stage = null, blockCol = null, hdrPanel = null, linesLayer = null;
  var hdrItems = [];               /* use 积木 -> 独立信息卡片 {stmt, wrap, cardEl, lineEl} */
  var selKey = null;               /* 选中语句的 start 偏移 */
  var gaps = [], currentGap = null;
  var drag = null;                 /* 拖动积木 */
  var palDrag = null;              /* 从积木箱拖新积木 */
  var slotPending = null;          /* 待进入编辑的槽位 */
  var cardEl = null;               /* 悬浮说明卡片 */

  /* 头文件清单 + 扫描结果（由 app.js 注入） */
  var headerInfo = null;          /* { "C_OPED.cash": {name, summary, ...} } */
  var headerStatus = {};          /* { "C_OPED.cash": {ok:true}, "C_CSS.cash": {ok:false} } */

  /* ---------- 小工具 ---------- */
  function div(cls) { var d = document.createElement('div'); if (cls) d.className = cls; return d; }
  function el(tag, cls, text) {
    var n = document.createElement(tag || 'span');
    if (cls) n.className = cls;
    if (text != null) n.textContent = text;
    return n;
  }
  function injectStyle() {
    if (document.getElementById('cstar-blocks-style')) return;
    var st = document.createElement('style');
    st.id = 'cstar-blocks-style';
    st.textContent = CSS_TEXT;
    document.head.appendChild(st);
  }
  function status(msg) { if (opts.setStatus) opts.setStatus(msg); }
  function walkStmts(list, fn) {
    for (var i = 0; i < list.length; i++) {
      var s = list[i];
      fn(s);
      for (var p = 0; p < s.parts.length; p++) {
        var ch = s.parts[p].children;
        if (ch) walkStmts(ch, fn);
      }
    }
  }
  function findStmtByStart(key) {
    if (key == null || !ast) return null;
    var found = null;
    walkStmts(ast.stmts, function (s) { if (s.start === key) found = s; });
    return found;
  }
  function wrapOf(stmt) {
    return rootEl ? rootEl.querySelector('.blk-wrap[data-stmt="' + stmt._id + '"]') : null;
  }

  function catColor(id) { return CAT_COLOR[id] || CAT_COLOR.misc; }
  function hexTint(hex, a) {
    var m = /^#([0-9a-f]{6})$/i.exec(hex);
    if (!m) return 'transparent';
    var n = parseInt(m[1], 16);
    return 'rgba(' + ((n >> 16) & 255) + ',' + ((n >> 8) & 255) + ',' + (n & 255) + ',' + a + ')';
  }

  /* ---------- 积木箱 ---------- */
  function usedHeaders() {
    /* 扫描当前代码里 use 了哪些头文件 */
    var re = /\buse\s+"([^"]+)"/g, m, list = [];
    while ((m = re.exec(codeText)) !== null) list.push(m[1]);
    return list;
  }
  function catUnlocked(cat, used) {
    var h = HEADER_GATED[cat];
    if (!h) return true;
    if (!used) used = usedHeaders();
    for (var i = 0; i < used.length; i++) if (used[i] === h) return true;
    return false;
  }

  /* 调色板缓存：use 的头文件集合 + headerInfo 签名不变就跳过重建 DOM */
  var palSig = '';
  function renderPalette() {
    var used = usedHeaders();
    var sig = used.slice().sort().join(',') +
      '|' + (headerInfo ? Object.keys(headerInfo).sort().join(',') : '');
    if (sig === palSig && opts.palette.children.length) return;  /* 没变化不重画 */
    palSig = sig;
    var pal = opts.palette;
    pal.className = 'pal';
    pal.innerHTML = '';
    CATEGORIES.forEach(function (cat) {
      var items = PALETTE.filter(function (p) { return p.cat === cat.id; });
      /* 没 use 对应 .cash 头文件的分类（win/css）：像没装的插件，整组不出现 */
      if (!catUnlocked(cat.id, used)) return;
      if (!items.length) return;
      var group = div('pal-group');
      group.classList.add('closed');                       /* 默认全部收起，如图 */
      var h = div('pal-cat-h');
      var hc = catColor(cat.id);
      h.style.borderLeft = '3px solid ' + hc.dot;
      h.style.background = hexTint(hc.dot, 0.14);
      h.appendChild(el('span', 'arrow', '▸'));
      h.appendChild(el('span', '', cat.name));
      group.appendChild(h);
      var box = div('pal-items');
      items.forEach(function (p) {
        var cc = catColor(cat.id);
        var it = div('pal-item ' + cc.cls);
        it.style.setProperty('--c', cc.dot);
        it.style.setProperty('--tint', hexTint(cc.dot, 0.08));
        it.dataset.pal = String(PALETTE.indexOf(p));
        var dot = div('pal-dot');
        dot.style.background = cc.dot;
        it.appendChild(dot);
        it.appendChild(el('span', '', p.name));
        it.appendChild(el('span', 'pal-hint', p.hint));
        box.appendChild(it);
      });
      group.appendChild(box);
      pal.appendChild(group);
      h.addEventListener('click', function () { group.classList.toggle('closed'); });
    });
  }

  /* 自动在程序开头补 use "X";（已 use 过就不动） */
  function autoUseHeader(headerName) {
    var used = usedHeaders();
    for (var i = 0; i < used.length; i++) if (used[i] === headerName) return;
    var line = 'use "' + headerName + '";\n';
    var head = codeText.indexOf('\n') >= 0 ? codeText.indexOf('\n') + 1 : 0;
    /* 放在所有已有的 use 之后；没有就放第一行 */
    var pos = head;
    var re = /\buse\s+"[^"]+"\s*;\s*$/m, m;
    while ((m = re.exec(codeText)) !== null) pos = m.index + m[0].length;
    var sp = codeText.slice(0, pos);
    var eol = sp.indexOf('\n') < 0 ? '' : '\n';
    apply(sp + eol + line + codeText.slice(pos));
    status('已加入：' + line.trim());
  }

  /* ---------- 渲染画布 ---------- */
  function render() {
    ast = parse(codeText);
    hideCard();
    var canvas = opts.canvas;
    canvas.classList.add('blk-canvas');
    canvas.innerHTML = '';
    var tip = div('blk-tip');
    tip.textContent = '点左边的积木加进程序；按住积木拖动可以换位置；点黄色小格子改内容，' +
                      '鼠标停在小格子上会说明这一格填什么；括号里的 ＋ 用来加参数；右键或 Delete 删除整句。';
    canvas.appendChild(tip);
    /* 两栏：左列积木（含嵌套框），右列头文件信息栏（在所有积木框外面） */
    stage = div('blk-stage');
    blockCol = div('blk-col');
    rootEl = div('blk-root');
    blockCol.appendChild(rootEl);
    hdrPanel = div('hdr-panel');
    stage.appendChild(blockCol);
    stage.appendChild(hdrPanel);
    linesLayer = div('hdr-lines');          /* 铺满 stage，长线在它里面按 stage 坐标画 */
    stage.appendChild(linesLayer);
    canvas.appendChild(stage);
    idMap = new Map();
    idSeq = 0;
    bodyEls = [];
    hdrItems = [];
    hdrPanel.innerHTML = '';
    ast.stmts.forEach(function (s) { rootEl.appendChild(renderStmt(s)); });
    dropLine = div('blk-drop-line');
    rootEl.appendChild(dropLine);
    /* use 积木：卡片拎出积木树，放进右栏；长线从块右边缘连过去 */
    ast.stmts.forEach(function (s) { if (s.type === 'use') collectHeader(s); });
    layoutHeaders();
  }

  /* 把一条 use 语句登记进右栏信息卡片 + 预留一条长线 */
  function collectHeader(s) {
    var card = renderHeaderCard(s, s.parts[0]);
    var anchor = el('span', 'hdr-anchor', '');
    card.appendChild(anchor);
    var line = div('hdr-line');
    linesLayer.appendChild(line);
    hdrPanel.appendChild(card);
    hdrItems.push({ stmt: s, wrap: wrapOf(s), cardEl: card, lineEl: line,
                    color: catColor(stmtCat(s)).dot });
  }

  /* 用像素坐标把每条长线从 use 积木右边缘拉到对应卡片左边缘 */
  function layoutHeaders() {
    if (!stage) return;
    var stageR = stage.getBoundingClientRect();
    hdrItems.forEach(function (it) {
      var wr = it.wrap ? it.wrap.getBoundingClientRect() : null;
      var cr = it.cardEl.getBoundingClientRect();
      if (!wr || !cr) { it.lineEl.style.display = 'none'; return; }
      var x1 = wr.right - stageR.left;                 /* 块右边缘 */
      var y1 = wr.top + wr.height / 2 - stageR.top;    /* 块垂直中心 */
      var x2 = cr.left - stageR.left;                  /* 卡片左边缘 */
      var y2 = cr.top + cr.height / 2 - stageR.top;     /* 卡片垂直中心 */
      var len = Math.max(0, x2 - x1);
      var ang = Math.atan2(y2 - y1, len || 1);
      it.lineEl.style.display = '';
      it.lineEl.style.cssText =
        'left:' + x1 + 'px;top:' + y1 + 'px;width:' + len + 'px;height:2px;' +
        'transform-origin:0 50%;transform:rotate(' + ang + 'rad);' +
        'background:' + it.color + ';box-shadow:0 0 4px rgba(0,0,0,.4)';
    });
  }

  function renderStmt(s) {
    s._id = ++idSeq;
    idMap.set(s._id, s);
    var wrap = div('blk-wrap');
    wrap.dataset.stmt = String(s._id);
    if (s.parts.length > 1) wrap.classList.add('blk-chain');
    for (var i = 0; i < s.parts.length; i++) wrap.appendChild(renderPart(s, s.parts[i], i));
    /* use 积木本身保持紧凑；概述卡片由 render() 拎到右栏信息栏 + 长线连接 */
    if (s.type === 'use') wrap.classList.add('hdr-use');
    return wrap;
  }

  function renderPart(s, part, pi) {
    var hasKids = !!(part.children && part.children.length);
    var box = div('blk ' + blockClassOf(s));
    if (part.body || hasKids) box.classList.add('has-body');
    var head = div('blk-head' + (part.kind === 'while-tail' ? ' blk-foot' : ''));
    if (pi === 0 && s.type === 'start') head.appendChild(el('span', 'kw', '入口'));
    renderTokens(head, s, part, pi);
    box.appendChild(head);
    if (part.body || hasKids) {
      var body = div('blk-body');
      if (part.children) part.children.forEach(function (c) { body.appendChild(renderStmt(c)); });
      if (!hasKids) {
        var hint = div('blk-empty');
        hint.textContent = '（空）';
        body.appendChild(hint);
      }
      bodyEls.push({ el: body, list: part.children || [], stmt: s, body: part.body });
      box.appendChild(body);
    }
    return box;
  }

  /* use 积木右侧拉出一条线 + 一个方框：头文件的概述卡片（卡片在积木外面） */
  function renderHeaderCard(s, part) {
    var fn = '';
    (s.slots || []).forEach(function (sl) { if (sl.kind === 'str') fn = sl.text; });
    var name = fn.replace(/^"|"$/g, '');
    var info = (headerInfo && headerInfo[name]) || null;
    var ok = !(headerStatus[name] && headerStatus[name].ok === false);
    var card = div('hdr-card ' + (ok ? 'ok' : 'miss'));
    var nm = el('span', 'hcd-name', (info && info.name) || name);
    var badge = el('span', 'hcd-badge', ok ? '已扫描' : '未找到');
    card.appendChild(nm);
    card.appendChild(badge);
    card.appendChild(el('div', 'hcd-sum',
      (info && info.summary) ||
      '自定义头文件：编译器会在 caskc 旁边找 "' + name + '"，找不到时这条 use 会报"找不到头文件"。'));
    var req = info && info.requires;
    if (req && req.length)
      card.appendChild(el('div', 'hcd-sum', '要先 use：' + req.join('、') + ''));
    return card;
  }

  function renderTokens(host, s, part, pi) {
    /* 注释积木：整段注释是一个可点改的黄色槽位（斜体绿字），
     * 点它就能改写注释文字，回车写回。 */
    if (s.type === 'comment') {
      var slot = (part.slots || [])[0];
      var ctext = slot ? slot.text : codeText.slice(s.start, s.end);
      var n = el('span', 'slot blk-comment', ctext);
      n.dataset.part = String(pi);
      n.dataset.slot = slot ? '0' : '-1';
      n.title = '点我改写这条注释';
      host.appendChild(n);
      return;
    }
    var toks = part.toks || [];
    if (!toks.length) {
      var name;
      if (s.type === 'empty') name = ';';
      else if (s.type === 'block') name = '{ }';
      else if (part.kind === 'else') name = 'else';
      else if (part.kind === 'while-tail') name = 'while';
      else name = part.kind;
      host.appendChild(el('span', 'kw', name));
      return;
    }
    var slotIdx = new Map();
    part.slots.forEach(function (sl, i) { slotIdx.set(sl.start, i); });
    for (var ti = 0; ti < toks.length; ti++) {
      var t = toks[ti], n;
      /* 收尾的 ')' 前面塞一个能点的小加号：空括号必须能填，函数调用的括号末尾能续参数 */
      if (t.kind === 'op' && t.text === ')' && ti > 0) {
        var prev = toks[ti - 1];
        var isEmpty = prev.kind === 'op' && prev.text === '(';
        var pin = parenInfo(toks, ti);
        var callName = pin && pin.name ? pin.name : '';
        /* 入口 main 不能带参数（编译器会报「'function main' 不要参数」），这里就别放加号了 */
        var skip = (s.type === 'function' && callName === 'main');
        if (!skip && (isEmpty || (callName && !KW[callName]))) {
          host.appendChild(makeHole(pi, t.start, isEmpty ? 'empty' : 'add'));
        }
      }
      var si = slotIdx.get(t.start);
      if (si !== undefined) {
        n = el('span', 'slot', t.text);
        n.dataset.part = String(pi);
        n.dataset.slot = String(si);
      } else if (t.kind === 'ident') {
        n = el('span', 'kw', t.text);
      } else {
        n = el('span', 'pn', t.text);
      }
      host.appendChild(n);
    }
  }

  /* 括号里的"加东西"小格子 */
  function makeHole(pi, at, mode) {
    var n = el('span', 'slot slot-add' + (mode === 'empty' ? ' slot-blank' : ''), '＋');
    n.dataset.part = String(pi);
    n.dataset.hole = mode;
    n.dataset.at = String(at);
    return n;
  }

  /* ---------- 选中 ---------- */
  function select(stmt) {
    selKey = stmt ? stmt.start : null;
    var sels = rootEl.querySelectorAll('.blk-wrap.sel');
    for (var i = 0; i < sels.length; i++) sels[i].classList.remove('sel');
    if (stmt) {
      var w = wrapOf(stmt);
      if (w) w.classList.add('sel');
    }
  }
  function restoreSelection(animate) {
    var stmt = findStmtByStart(selKey);
    if (!stmt) { selKey = null; return; }
    select(stmt);
    if (animate) {
      var w = wrapOf(stmt);
      if (w) { w.classList.add('blk-land'); }
    }
  }

  /* ---------- 应用一次编辑（带撤销历史） ---------- */
  var undoStack = [], redoStack = [], MAXHIST = 300;
  var gBlockClip = '';               /* 积木剪贴板（本视图内部，粘贴兜底） */
  function pushHistory() {
    undoStack.push(codeText);
    if (undoStack.length > MAXHIST) undoStack.shift();
    redoStack.length = 0;
  }
  function apply(newText) {
    pushHistory();
    codeText = newText;
    if (opts.setText) opts.setText(newText);
    hideCard();
    renderPalette();
    render();
    restoreSelection(true);
  }
  /* 不记历史的重画（undo/redo 本身用） */
  function applyNoHistory(newText) {
    codeText = newText;
    if (opts.setText) opts.setText(newText);
    hideCard();
    renderPalette();
    render();
    restoreSelection(false);
  }

  function undo() {
    if (!undoStack.length) { status('没有可撤销的操作'); return; }
    redoStack.push(codeText);
    applyNoHistory(undoStack.pop());
    status('撤销了上一步');
  }
  function redo() {
    if (!redoStack.length) { status('没有可重做的操作'); return; }
    undoStack.push(codeText);
    applyNoHistory(redoStack.pop());
    status('重做了一步');
  }

  /* 选中积木整行（含缩进和换行）的文本 */
  function stmtLineText(s) {
    var ls = codeText.lastIndexOf('\n', s.start - 1) + 1;
    var le = s.end;
    while (le < codeText.length && codeText.charAt(le) !== '\n') le++;
    if (codeText.charAt(le) === '\n') le++;
    return codeText.slice(ls, le);
  }

  /* ---------- 复制 / 剪切 / 粘贴（积木视图） ---------- */
  function copySel() {
    var s = findStmtByStart(selKey);
    if (!s) { status('先选中一块积木再复制（点一下积木）'); return; }
    var text = stmtLineText(s);
    gBlockClip = text;
    if (navigator.clipboard && navigator.clipboard.writeText)
      navigator.clipboard.writeText(text).then(function () {}, function () {});
    status('已复制 1 块积木（Ctrl+V 粘贴）');
  }
  function cutSel() {
    var s = findStmtByStart(selKey);
    if (!s) { status('先选中一块积木再剪切'); return; }
    copySel();
    selKey = null;
    apply(deleteRange(codeText, s.start, s.end));
    status('已剪切，Ctrl+V 粘贴到别处');
  }
  function doPasteSnippet(snippet) {
    var ins = String(snippet || '').replace(/\r\n/g, '\n').replace(/\n+$/, '');
    if (!ins.trim()) { status('没有可粘贴的内容'); return; }
    /* 去掉复制时带的原缩进，按粘贴位置的缩进重新对齐 */
    var lines = ins.split('\n');
    var min = -1;
    for (var i = 0; i < lines.length; i++) {
      var m = /^[ \t]*/.exec(lines[i]);
      var w = m[0].length;
      if (lines[i].trim() && (min < 0 || w < min)) min = w;
    }
    if (min > 0) ins = lines.map(function (l) { return l ? l.slice(min) : l; }).join('\n');
    /* 插在哪：选中了一块就插它后面；否则塞进 main 身体；再否则放末尾 */
    var s = findStmtByStart(selKey);
    var pos;
    if (s) pos = insertPosAfterStmt(codeText, s);
    else {
      var host = entryFunction();
      if (host && host.body) {
        var kids = host.children || [];
        pos = kids.length
          ? insertPosAfterStmt(codeText, kids[kids.length - 1])
          : host.body.start + 1;
      } else pos = codeText.length;
    }
    var indent = lineIndentAt(codeText, pos);
    apply(insertTemplate(codeText, pos, ins + '\n', indent));
    status('已粘贴 1 块积木');
  }
  function pasteBlock() {
    if (navigator.clipboard && navigator.clipboard.readText) {
      navigator.clipboard.readText().then(function (ct) {
        if (ct && ct.trim()) doPasteSnippet(ct);
        else doPasteSnippet(gBlockClip);
      }, function () { doPasteSnippet(gBlockClip); });
    } else doPasteSnippet(gBlockClip);
  }

  /* ---------- 拖放：插入缝隙 ---------- */
  function computeGaps() {
    gaps = [];
    function wrapsOf(container) {
      var out = [];
      for (var i = 0; i < container.children.length; i++) {
        var n = container.children[i];
        if (n.classList && n.classList.contains('blk-wrap')) out.push(n);
      }
      return out;
    }
    function addList(list, container, owner, body) {
      var ws = wrapsOf(container);
      ws.forEach(function (w, i) { gaps.push({ list: list, index: i, ref: w, container: container, owner: owner, body: body }); });
      gaps.push({ list: list, index: ws.length, ref: null, container: container, owner: owner, body: body });
    }
    addList(ast.stmts, rootEl, null, null);
    bodyEls.forEach(function (b) { addList(b.list, b.el, b.stmt, b.body); });
  }

  function updateDrop(x, y) {
    var best = null, bestD = Infinity;
    var rr0 = rootEl.getBoundingClientRect();
    for (var i = 0; i < gaps.length; i++) {
      var g = gaps[i];
      var cr = g.container.getBoundingClientRect();
      if (y < cr.top - 8 || y > cr.bottom + 8) continue;
      if (x < cr.left - 12 || x > cr.right + 140) continue;
      var gy, d;
      if (g.ref) { gy = g.ref.getBoundingClientRect().top; d = Math.abs(y - gy); }
      else { gy = cr.bottom - 6; d = Math.abs(y - cr.bottom) + 3; }
      if (d < bestD) { bestD = d; best = g; }
    }
    currentGap = best;
    if (!best) { dropLine.style.display = 'none'; return; }
    var cr2 = best.container.getBoundingClientRect();
    var left, top, width;
    if (best.ref) {
      var rr = best.ref.getBoundingClientRect();
      left = rr.left - rr0.left; top = rr.top - rr0.top; width = rr.width;
    } else {
      left = cr2.left - rr0.left + 12; top = cr2.bottom - rr0.top - 6; width = cr2.width - 24;
    }
    dropLine.style.display = 'block';
    dropLine.style.left = left + 'px';
    dropLine.style.top = top + 'px';
    dropLine.style.width = Math.max(40, width) + 'px';
  }

  function hideDrop() {
    currentGap = null;
    if (dropLine) dropLine.style.display = 'none';
  }

  /* 移动一条语句到缝隙 */
  function moveToGap(stmt, gap) {
    if (!gap) return;
    if (gap.owner && gap.owner.start >= stmt.start && gap.owner.end <= stmt.end) return;  // 拖进自己肚子里：忽略
    var pos;
    if (gap.index < gap.list.length) {
      pos = insertPosBeforeStmt(codeText, gap.list[gap.index]);
    } else if (gap.list.length) {
      pos = insertPosAfterStmt(codeText, gap.list[gap.list.length - 1]);
    } else {
      pos = gap.body ? gap.body.start + 1                            // 空体：紧跟 '{'
          : (gap.owner ? insertPosAfterStmt(codeText, gap.owner) : codeText.length);
    }
    if (pos === stmt.start) return;                               // 原地
    var newText = moveRange(codeText, stmt.start, stmt.end, pos);
    /* 移动后新位置： */
    var len = stmt.end - stmt.start;
    selKey = (pos < stmt.start) ? pos : (pos - len);
    apply(newText);
    status('已移动语句');
  }

  /* 把调色板条目插进缝隙（或选中语句之后 / 程序末尾） */
  /* 语句类模板默认要放进程序入口；use / constant / 函数定义放顶层 */
  function wantsTopLevel(code) {
    return /^(use|constant|const|function|func)\b/.test(code) ||
           /^(number|real|text|letter|bool|none)\s+[^\s(]*\s*\(/.test(code);
  }
  /* 找一个能收留语句的函数：优先 main，其次第一个带身体的 */
  function entryFunction() {
    var fns = (ast ? ast.stmts : []).filter(function (s) {
      return (s.type === 'function' || s.type === 'start') && s.body;
    });
    if (!fns.length) return null;
    for (var i = 0; i < fns.length; i++) {
      if (/\bmain\b/.test(codeText.slice(fns[i].headStart, fns[i].headEnd))) return fns[i];
    }
    return fns[0];
  }

  function insertFromPalette(itemIdx, gap) {
    var item = PALETTE[itemIdx];
    if (!item) return;
    var pos, indent;
    if (gap) {
      if (gap.index < gap.list.length) {
        pos = insertPosBeforeStmt(codeText, gap.list[gap.index]);
        indent = lineIndentAt(codeText, pos);
      } else if (gap.list.length) {
        var last = gap.list[gap.list.length - 1];
        pos = insertPosAfterStmt(codeText, last);
        indent = lineIndentAt(codeText, last.start);
      } else {
        pos = gap.body ? gap.body.start + 1
            : (gap.owner ? insertPosAfterStmt(codeText, gap.owner) : codeText.length);
        indent = lineIndentAt(codeText, gap.body ? gap.body.start : (gap.owner ? gap.owner.start : codeText.length)) + '    ';
      }
    } else if (wantsTopLevel(item.code)) {
      /* 入口 / use / constant / 函数：永远放顶层（选中哪条都不跟着走） */
      if (ast.stmts.length) {
        var tp = ast.stmts[ast.stmts.length - 1];
        pos = insertPosAfterStmt(codeText, tp);
        indent = lineIndentAt(codeText, tp.start);
      } else {
        pos = codeText.length;
        indent = '';
      }
    } else {
      var stmt = findStmtByStart(selKey);
      if (stmt) {
        pos = insertPosAfterStmt(codeText, stmt);
        indent = lineIndentAt(codeText, stmt.start);
      } else {
        var host = entryFunction();
        if (host && host.body) {
          /* 没选中 = 放进 main 的身体里，而不是扔到顶层外面 */
          var kids = host.children || [];
          if (kids.length) {
            var lastK = kids[kids.length - 1];
            pos = insertPosAfterStmt(codeText, lastK);
            indent = lineIndentAt(codeText, lastK.start);
          } else {
            pos = host.body.start + 1;
            indent = lineIndentAt(codeText, host.body.start) + '    ';
          }
        } else if (ast.stmts.length) {
          var t = ast.stmts[ast.stmts.length - 1];
          pos = insertPosAfterStmt(codeText, t);
          indent = lineIndentAt(codeText, t.start);
        } else {
          /* 空程序 + 语句模板：直接给它一个 function main() 的家 */
          if (!wantsTopLevel(item.code)) {
            apply('function main() {\n' + indentSnippet(item.code, '    ') + '}\n');
            status('已插入：' + item.name);
            return;
          }
          pos = codeText.length;
          indent = '';
        }
      }
    }
    selKey = pos;
    apply(insertTemplate(codeText, pos, item.code, indent));
    status('已插入：' + item.name);
  }

  /* ---------- 拖动（积木 & 积木箱共用一套手势） ---------- */
  function makeGhost(node) {
    var g = node.cloneNode(true);
    g.classList.add('blk-ghost');
    document.body.appendChild(g);
    return g;
  }
  function moveGhost(g, x, y) {
    g.style.transform = 'translate3d(' + (x + 10) + 'px,' + (y + 10) + 'px,0)';
  }
  function removeGhost(g) { if (g && g.parentNode) g.parentNode.removeChild(g); }

  function onMove(e) {
    if (slotPending) {
      if (Math.abs(e.clientX - slotPending.x) + Math.abs(e.clientY - slotPending.y) > 4) {
        slotPending = null;                    // 移动了就不算点击编辑
      }
      return;
    }
    if (drag) {
      if (!drag.active) {
        if (Math.abs(e.clientX - drag.x) + Math.abs(e.clientY - drag.y) <= 4) return;
        drag.active = true;
        hideCard();
        drag.ghost = makeGhost(drag.wrap);
        drag.ghost.style.width = drag.wrap.getBoundingClientRect().width + 'px';
        drag.wrap.classList.add('ph');
        computeGaps();
      }
      moveGhost(drag.ghost, e.clientX, e.clientY);
      updateDrop(e.clientX, e.clientY);
      e.preventDefault();
      return;
    }
    if (palDrag) {
      if (!palDrag.active) {
        if (Math.abs(e.clientX - palDrag.x) + Math.abs(e.clientY - palDrag.y) <= 4) return;
        palDrag.active = true;
        var src = document.querySelector('.pal-item[data-pal="' + palDrag.idx + '"]');
        palDrag.ghost = makeGhost(src || opts.palette);
        computeGaps();
      }
      moveGhost(palDrag.ghost, e.clientX, e.clientY);
      updateDrop(e.clientX, e.clientY);
      e.preventDefault();
    }
  }

  function onUp(e) {
    document.removeEventListener('mousemove', onMove, true);
    document.removeEventListener('mouseup', onUp, true);
    if (slotPending) {
      var sp = slotPending;
      slotPending = null;
      startSlotEdit(sp);
      return;
    }
    if (drag) {
      var d = drag;
      drag = null;
      if (d.active) {
        removeGhost(d.ghost);
        d.wrap.classList.remove('ph');
        var gap = currentGap;
        hideDrop();
        if (gap) moveToGap(d.stmt, gap);
      }
      return;
    }
    if (palDrag) {
      var pd = palDrag;
      palDrag = null;
      if (pd.active) {
        removeGhost(pd.ghost);
        var gap2 = currentGap;
        hideDrop();
        insertFromPalette(pd.idx, gap2);
      } else {
        insertFromPalette(pd.idx, null);        // 单击 = 插到选中积木之后
      }
    }
  }

  function beginGesture() {
    document.addEventListener('mousemove', onMove, true);
    document.addEventListener('mouseup', onUp, true);
  }

  /* ---------- 悬浮小卡片：鼠标停在小格子上说这一格填什么 ---------- */
  function card() {
    if (cardEl) return cardEl;
    cardEl = div('blk-tipcard');
    cardEl.style.display = 'none';
    cardEl.appendChild(div('tc-title'));
    cardEl.appendChild(div('tc-sig'));
    cardEl.appendChild(div('tc-note'));
    document.body.appendChild(cardEl);
    return cardEl;
  }

  function showCard(info, target) {
    if (!info) { hideCard(); return; }
    var c = card();
    var t = c.children[0], g = c.children[1], n = c.children[2];
    t.textContent = info.title || '';
    g.textContent = info.sig || '';
    n.textContent = info.note || '';
    t.style.display = t.textContent ? '' : 'none';
    g.style.display = g.textContent ? '' : 'none';
    n.style.display = n.textContent ? '' : 'none';
    c.style.display = '';
    c.style.visibility = 'hidden';
    var r = target.getBoundingClientRect();
    var w = c.offsetWidth, h = c.offsetHeight;
    var left = Math.min(Math.max(6, r.left), Math.max(6, window.innerWidth - w - 8));
    var top = r.bottom + 6;
    if (top + h > window.innerHeight - 6) top = Math.max(6, r.top - h - 6);
    c.style.left = left + 'px';
    c.style.top = top + 'px';
    c.style.visibility = '';
  }

  function hideCard() { if (cardEl) cardEl.style.display = 'none'; }

  function slotInfoOf(el) {
    var wrap = el.closest('.blk-wrap');
    var stmt = wrap ? idMap.get(+wrap.dataset.stmt) : null;
    if (!stmt) return null;
    var part = stmt.parts[+el.dataset.part];
    if (!part) return null;
    if (el.dataset.hole) {
      var at = +el.dataset.at, toks = part.toks || [], ti = -1;
      for (var i = 0; i < toks.length; i++) { if (toks[i].start === at) { ti = i; break; } }
      return holeTip(stmt, part, ti, el.dataset.hole);
    }
    return slotTip(stmt, part, +el.dataset.slot);
  }

  function onCanvasOver(e) {
    var el2 = e.target && e.target.closest ? e.target.closest('.slot') : null;
    if (!el2) { hideCard(); return; }
    showCard(slotInfoOf(el2), el2);
  }

  function onCanvasOut(e) {
    var to = e.relatedTarget;
    if (to && e.target && e.target.closest && e.target.closest('.slot') &&
        to.closest && to.closest('.slot') === e.target.closest('.slot')) return;   /* 还在同一格上 */
    hideCard();
  }

  /* ---------- 画布事件 ---------- */
  function onCanvasDown(e) {
    if (e.button !== 0) return;
    var wrap = e.target && e.target.closest ? e.target.closest('.blk-wrap') : null;
    if (!wrap) { select(null); return; }
    var stmt = idMap.get(+wrap.dataset.stmt);
    if (!stmt) return;
    select(stmt);
    var slotEl = e.target.closest('.slot');
    if (slotEl) {
      slotPending = { el: slotEl, x: e.clientX, y: e.clientY };
    } else {
      drag = { stmt: stmt, wrap: wrap, x: e.clientX, y: e.clientY, active: false, ghost: null };
    }
    beginGesture();
    e.preventDefault();
  }

  function onCanvasContext(e) {
    var wrap = e.target && e.target.closest ? e.target.closest('.blk-wrap') : null;
    if (!wrap) return;
    e.preventDefault();
    var stmt = idMap.get(+wrap.dataset.stmt);
    if (!stmt) return;
    select(stmt);
    deleteSelected();
  }

  function onPalDown(e) {
    if (e.button !== 0) return;
    var item = e.target && e.target.closest ? e.target.closest('.pal-item') : null;
    if (!item) return;
    palDrag = { idx: +item.dataset.pal, x: e.clientX, y: e.clientY, active: false, ghost: null };
    beginGesture();
    e.preventDefault();
  }

  function onKey(e) {
    var t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA')) return;
    /* Ctrl 系：复制/剪切/粘贴/撤销/重做（只在积木视图里，代码视图让 textarea 自己处理） */
    var mod = e.ctrlKey || e.metaKey;
    if (mod) {
      var k = e.key.toLowerCase();
      if (k === 'c' && !e.shiftKey) { e.preventDefault(); copySel(); return; }
      if (k === 'x' && !e.shiftKey) { e.preventDefault(); cutSel(); return; }
      if (k === 'v') { e.preventDefault(); pasteBlock(); return; }
      if (k === 'z' && !e.shiftKey) { e.preventDefault(); undo(); return; }
      if ((k === 'z' && e.shiftKey) || k === 'y') { e.preventDefault(); redo(); return; }
    }
    if (e.key === 'Delete' || e.key === 'Backspace') {
      if (selKey != null) { e.preventDefault(); deleteSelected(); }
    } else if (e.key === 'Escape') {
      select(null);
    }
  }

  function deleteSelected() {
    var stmt = findStmtByStart(selKey);
    if (!stmt) return;
    selKey = null;
    apply(deleteRange(codeText, stmt.start, stmt.end));
    status('已删除一条语句');
  }

  /* ---------- 槽位就地编辑 ---------- */
  function unescapeLit(t) {
    var body = t.length >= 2 ? t.slice(1, -1) : t;
    return body.replace(/\\(u)?(.)/g, function (m, u, c) {
      if (c === 'n') return '\n';
      if (c === 't') return '\t';
      return c;
    });
  }

  function startSlotEdit(pending) {
    var slotEl = pending.el;
    if (slotEl.dataset.hole) { startHoleEdit(slotEl); return; }
    var wrap = slotEl.closest('.blk-wrap');
    var stmt = idMap.get(+wrap.dataset.stmt);
    if (!stmt) return;
    var part = stmt.parts[+slotEl.dataset.part];
    var slot = part && part.slots[+slotEl.dataset.slot];
    if (!slot) return;
    var rect = slotEl.getBoundingClientRect();
    var input = document.createElement('input');
    input.className = 'blk-input';
    input.value = (slot.kind === 'str' || slot.kind === 'chr') ? unescapeLit(slot.text) : slot.text;
    input.style.left = rect.left + 'px';
    input.style.top = (rect.top - 2) + 'px';
    input.style.width = Math.max(56, rect.width + 18) + 'px';
    document.body.appendChild(input);
    input.focus();
    input.select();
    var done = false;
    function finish(commit) {
      if (done) return;
      done = true;
      var v = input.value;
      if (input.parentNode) input.parentNode.removeChild(input);
      if (commit) {
        var newText = editSlot(codeText, slot, v);
        if (newText !== codeText) {
          selKey = stmt.start;
          apply(newText);
          status('已修改：' + v);
        }
      }
    }
    input.addEventListener('keydown', function (ev) {
      ev.stopPropagation();
      if (ev.key === 'Enter') { ev.preventDefault(); finish(true); }
      else if (ev.key === 'Escape') { ev.preventDefault(); finish(false); }
    });
    input.addEventListener('blur', function () { finish(true); });
  }

  /* 括号里的 ＋：就地敲内容，回车把它插进括号里 */
  function startHoleEdit(holeEl) {
    var wrap = holeEl.closest('.blk-wrap');
    var stmt = wrap ? idMap.get(+wrap.dataset.stmt) : null;
    if (!stmt) return;
    var part = stmt.parts[+holeEl.dataset.part] || {};
    var at = +holeEl.dataset.at;
    var mode = holeEl.dataset.hole;
    var toks = part.toks || [], ti = -1;
    for (var i = 0; i < toks.length; i++) { if (toks[i].start === at) { ti = i; break; } }
    var info = holeTip(stmt, part, ti, mode);

    var rect = holeEl.getBoundingClientRect();
    var input = document.createElement('input');
    input.className = 'blk-input';
    input.placeholder = (info && info.title) ? info.title : '填内容…';
    input.value = '';
    input.style.left = rect.left + 'px';
    input.style.top = (rect.top - 2) + 'px';
    input.style.width = Math.max(120, rect.width + 60) + 'px';
    document.body.appendChild(input);
    hideCard();
    input.focus();
    var done = false;
    function finish(commit) {
      if (done) return;
      done = true;
      var v = input.value.replace(/^\s+|\s+$/g, '');
      if (input.parentNode) input.parentNode.removeChild(input);
      if (!commit || !v) return;
      var ins = v;
      if (mode === 'add') {
        /* 前面已经有参数了：补一个逗号（结尾本来就多一个逗号的话就不补） */
        var k = at - 1;
        while (k >= 0 && /\s/.test(codeText.charAt(k))) k--;
        if (k >= 0 && codeText.charAt(k) !== ',' && codeText.charAt(k) !== '(') ins = ', ' + v;
      }
      var newText = splice(codeText, at, at, ins);
      if (newText === codeText) return;
      selKey = stmt.start;
      apply(newText);
      status(mode === 'add' ? '括号里补上了：' + v : '括号里填上了：' + v);
    }
    input.addEventListener('keydown', function (ev) {
      ev.stopPropagation();
      if (ev.key === 'Enter') { ev.preventDefault(); finish(true); }
      else if (ev.key === 'Escape') { ev.preventDefault(); finish(false); }
    });
    input.addEventListener('blur', function () { finish(true); });
  }

  /* ---------- 对外三方法 ---------- */
  function init(o) {
    opts = o;
    injectStyle();
    codeText = String(opts.getText ? opts.getText() : '');
    renderPalette();
    render();
    opts.canvas.addEventListener('mousedown', onCanvasDown);
    opts.canvas.addEventListener('contextmenu', onCanvasContext);
    opts.canvas.addEventListener('mouseover', onCanvasOver);
    opts.canvas.addEventListener('mouseout', onCanvasOut);
    opts.canvas.addEventListener('scroll', function () { hideCard(); layoutHeaders(); });
    opts.palette.addEventListener('mousedown', onPalDown);
    document.addEventListener('keydown', onKey);
    /* 窗口变化后，积木列和右栏卡片位置变了，长线要重画 */
    var relayout = debounce(function () { layoutHeaders(); }, 100);
    window.addEventListener('resize', relayout);
    window.__cstarBlocksRelayout = relayout;
  }

  /* 简单防抖 */
  function debounce(fn, ms) {
    var t = null;
    return function () { var a = arguments; if (t) clearTimeout(t); t = setTimeout(function () { t = null; fn.apply(null, a); }, ms); };
  }

  function refresh(code) {
    if (!opts) return;
    var newCode = String(code == null ? opts.getText() : code);
    if (newCode === codeText && ast) return;  /* 代码没变不重画，防卡死 */
    codeText = newCode;
    renderPalette();
    render();
    restoreSelection(false);
  }

  /* app.js 注入头文件清单 / 扫描结果（决定概述卡片文案与门禁） */
  function setHeaderInfo(info) {
    headerInfo = info;
    palSig = '';
    /* 头文件清单变了：画布上的 use 积木卡片也要重画 */
    if (ast && rootEl) { render(); restoreSelection(false); }
  }
  function setHeaderStatus(status) {
    headerStatus = status || {};
    if (ast && rootEl) { render(); restoreSelection(false); }
  }

  function showError(line) {
    if (!ast || line == null) return;
    var best = null;
    walkStmts(ast.stmts, function (s) {
      var l0 = lineOf(codeText, s.start);
      var l1 = lineOf(codeText, Math.max(s.start, s.end - 1));
      if (line >= l0 && line <= l1) {
        if (!best || (s.end - s.start) < (best.end - best.start)) best = s;
      }
    });
    if (!best) return;
    var w = wrapOf(best);
    if (!w) return;
    w.classList.remove('blk-err');
    void w.offsetWidth;                                     /* 重启动画 */
    w.classList.add('blk-err');
    if (w.scrollIntoView) w.scrollIntoView({ block: 'center', behavior: 'smooth' });
    setTimeout(function () { w.classList.remove('blk-err'); }, 1900);
  }

  return { init: init, refresh: refresh, showError: showError,
           setHeaderInfo: setHeaderInfo, setHeaderStatus: setHeaderStatus,
           copySel: copySel, cutSel: cutSel, pasteBlock: pasteBlock,
           undo: undo, redo: redo };
})();

if (UI) {
  Blocks.init = UI.init;
  Blocks.refresh = UI.refresh;
  Blocks.showError = UI.showError;
  Blocks.setHeaderInfo = UI.setHeaderInfo;
  Blocks.setHeaderStatus = UI.setHeaderStatus;
  Blocks.copySel = UI.copySel;
  Blocks.cutSel = UI.cutSel;
  Blocks.pasteBlock = UI.pasteBlock;
  Blocks.undo = UI.undo;
  Blocks.redo = UI.redo;
}

if (typeof window !== 'undefined') window.Blocks = Blocks;
if (typeof module !== 'undefined' && module.exports) module.exports = Blocks;
