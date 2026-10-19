/* editor.js -- the code editing area of C* Studio.
 *
 * A textarea on top of a highlighted copy of the same text: the text is
 * transparent, the pretty version shines through underneath, and a gutter
 * on the left shows line numbers and marks the lines with problems.
 *
 * Smart editing touches (VS-like):
 *   - brackets ( ) [ ] { } and quotes " ' are paired automatically;
 *     typing a closing one next to its pair just steps over it
 *   - the pair of the bracket under the caret is highlighted
 *   - the current line gets a faint full-width highlight (and the line
 *     number in the gutter turns white)
 *   - variables are tinted (light blue), so code is readable at a glance
 *
 *   Editor.init({ onCaret(line, col), onChange() })
 *   Editor.getCode() / setCode(text) / focusLine(line, col)
 *   Editor.setErrors([{line, k}])    -- which lines to mark
 *   Editor.setReadOnly(on)
 */

var Editor = (function () {
  var ta, hl, gutter, wrap, tip, lm;
  var onCaret = null, onChange = null;
  var internal = false;
  var SYN = (typeof SYNTAX !== 'undefined') ? SYNTAX : null;

  var KEYWORDS = {};
  ('print say show input start if else while for break continue return constant use ' +
   'and or not true false number real text letter bool none function func int const void ' +
   'switch case default do foreach in until').split(' ').forEach(function (k) { KEYWORDS[k] = 1; });

  /* 配对的括号：开括号 -> 闭括号 */
  var PAIR_CLOSE = { '(': ')', '[': ']', '{': '}', '"': '"', "'": "'" };

  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  /* 悬浮小卡片：记下每个能解释的词，鼠标停上去就显示它的用法 */
  var tokHints = [];

  function hintAt(pos) {
    for (var i = 0; i < tokHints.length; i++) {
      var t = tokHints[i];
      if (pos < t.start) break;
      if (pos < t.end) return t;
    }
    return null;
  }

  function showTip(t, x, y) {
    if (!tip) return;
    tip.querySelector('.tip-sig').textContent = t.sig;
    tip.querySelector('.tip-note').textContent = t.note;
    tip.hidden = false;
    var r = document.getElementById('editor').getBoundingClientRect();
    var w = tip.offsetWidth, h = tip.offsetHeight;
    var left = Math.max(4, Math.min(x - r.left + 14, r.width - w - 8));
    var top = y - r.top + 22;
    if (top + h > r.height) top = Math.max(4, top - h - 30);
    tip.style.left = left + 'px';
    tip.style.top = top + 'px';
  }

  function hideTip() { if (tip) tip.hidden = true; }

  function onMove(e) {
    if (e.buttons) { hideTip(); return; }           /* 正拖着选字，别来打扰 */
    var pos = -1;
    if (document.caretPositionFromPoint) {
      var c = document.caretPositionFromPoint(e.clientX, e.clientY);
      if (c && c.offsetNode === ta) pos = c.offset;
    }
    var t = pos >= 0 ? hintAt(pos) : null;
    if (t) showTip(t, e.clientX, e.clientY); else hideTip();
  }

  /* ---------------- 括号配对 ---------------- */

  /* 找与 src[pos] 配对的括号的位置；找不到返回 -1。
   * 扫描时跳过字符串与注释，避免把引号里的符号也算进去。 */
  function findMatch(src, pos) {
    var c = src[pos], open, close, dir;
    if (c === '(' || c === '[' || c === '{') { open = c; close = PAIR_CLOSE[c]; dir = 1; }
    else if (c === ')' || c === ']' || c === '}') { close = c; open = PAIR_CLOSE[c]; dir = -1; }
    else return -1;
    var n = src.length, depth = 0, i = pos;
    while (dir === 1 ? i < n : i >= 0) {
      var ch = src[i];
      if (ch === '"' || ch === "'") {
        if (dir === 1) {
          var q = ch, j = i + 1;
          while (j < n) {
            if (src[j] === '\\') j += 2;
            else if (src[j] === q) { j++; break; }
            else if (src[j] === '\n') break;
            else j++;
          }
          i = j; continue;
        }
        /* 反向：往回找这个字符串的开头引号，跳到它前面 */
        var q2 = ch, k = i - 1;
        while (k >= 0) {
          if (src[k] === '\\') { k -= 2; continue; }
          if (src[k] === q2) break;
          k--;
        }
        i = (k >= 0) ? k - 1 : i - 1;
        continue;
      }
      if (ch === '/' && src[i + 1] === '/') {
        if (dir === 1) { var j2 = src.indexOf('\n', i); i = j2 < 0 ? n : j2; continue; }
        var nl = src.lastIndexOf('\n', i - 1);
        i = nl - 1; continue;
      }
      if (ch === '/' && src[i + 1] === '*') {
        if (dir === 1) { var j3 = src.indexOf('*/', i + 2); i = j3 < 0 ? n : j3 + 2; continue; }
        var st = src.lastIndexOf('/*', i);
        i = (st >= 0 ? st : 0) - 1; continue;
      }
      if (ch === open) depth++;
      else if (ch === close) { depth--; if (depth === 0) return i; }
      i += dir;
    }
    return -1;
  }

  /* 光标附近要配对的括号：优先看光标左边的，其次右边的 */
  function bracketPairAt(src, caret) {
    var l = caret > 0 ? src[caret - 1] : '';
    if (l === '(' || l === '[' || l === '{' || l === ')' || l === ']' || l === '}') {
      var m = findMatch(src, caret - 1);
      if (m >= 0) return { a: caret - 1, b: m };
    }
    var r = caret < src.length ? src[caret] : '';
    if (r === '(' || r === '[' || r === '{' || r === ')' || r === ']' || r === '}') {
      var m2 = findMatch(src, caret);
      if (m2 >= 0) return { a: caret, b: m2 };
    }
    return null;
  }

  /* ---------------- paint ---------------- */

  var lastCaret = -1;

  /* paint the pretty copy of the code */
  function paint() {
    var src = ta.value, out = '', i = 0, n = src.length;
    tokHints = [];
    var caret = ta.selectionStart;
    var pair = bracketPairAt(src, caret);
    while (i < n) {
      var c = src[i], d = src[i + 1];
      if (c === '/' && d === '/') {
        var j = src.indexOf('\n', i); if (j < 0) j = n;
        out += '<span class="c-com">' + esc(src.slice(i, j)) + '</span>'; i = j; continue;
      }
      if (c === '/' && d === '*') {
        var j2 = src.indexOf('*/', i + 2); j2 = j2 < 0 ? n : j2 + 2;
        out += '<span class="c-com">' + esc(src.slice(i, j2)) + '</span>'; i = j2; continue;
      }
      if (c === '"' || c === "'") {
        var j3 = i + 1;
        while (j3 < n && src[j3] !== c) { if (src[j3] === '\\') j3++; j3++; }
        j3 = Math.min(j3 + 1, n);
        out += '<span class="c-str">' + esc(src.slice(i, j3)) + '</span>'; i = j3; continue;
      }
      if (c >= '0' && c <= '9') {
        var j4 = i;
        while (j4 < n && ((src[j4] >= '0' && src[j4] <= '9') || src[j4] === '.')) j4++;
        out += '<span class="c-num">' + esc(src.slice(i, j4)) + '</span>'; i = j4; continue;
      }
      if (/[A-Za-z_]/.test(c) || c.charCodeAt(0) > 127) {
        var j5 = i;
        while (j5 < n && (/[A-Za-z0-9_]/.test(src[j5]) || src[j5].charCodeAt(0) > 127)) j5++;
        var w = src.slice(i, j5);
        var e = SYN ? SYN.lookup(w) : null;
        if (e) {
          out += '<span class="' + SYN.CAT[e.cat] + '">' + esc(w) + '</span>';
          tokHints.push({ start: i, end: j5, sig: e.sig, note: e.note });
        } else {
          var k5 = j5;
          while (k5 < n && (src[k5] === ' ' || src[k5] === '\t')) k5++;
          if (src[k5] === '(' && !KEYWORDS[w]) {
            out += '<span class="t-fn">' + esc(w) + '</span>';   /* 自己定义的函数 */
            tokHints.push({
              start: i, end: j5, sig: w + '(…)',
              note: '程序里自己定义的函数（或者来自 use 头文件）。调用时括号里按定义时的顺序填参数。'
            });
          } else {
            out += '<span class="c-var2">' + esc(w) + '</span>';  /* 变量 */
          }
        }
        i = j5; continue;
      }
      if (pair && (i === pair.a || i === pair.b)) {
        out += '<span class="c-bmatch">' + esc(c) + '</span>';
      } else {
        out += esc(c);
      }
      i++;
    }
    hl.innerHTML = out + '\n';
    tokHints.sort(function (a, b) { return a.start - b.start; });
  }

  var errLines = {};   /* line -> 'err' | 'warn' */

  function paintGutter() {
    var count = ta.value.split('\n').length, html = '';
    var cl = caretPos().line;
    for (var i = 1; i <= count; i++) {
      var cls = errLines[i] ? ' ' + errLines[i] : '';
      if (i === cl) cls += ' cur';                       /* 光标所在行号变白 */
      html += '<span class="gl' + cls + '">' + i + '</span>';
    }
    gutter.innerHTML = html;
    syncScroll();
  }

  function syncScroll() {
    /* 高亮层和代码框是叠在一起的两层：字号、行高、内边距都一样，
     * 只要把高亮层滚到同一个偏移，彩色字就和上面的透明文字严丝合缝。 */
    hl.scrollTop = ta.scrollTop;
    hl.scrollLeft = ta.scrollLeft;
    gutter.scrollTop = ta.scrollTop;
    updateLineMark();
  }

  /* 光标所在行的整行高亮（和 VS 一样，淡色横条） */
  function updateLineMark() {
    if (!lm) return;
    var line = caretPos().line;
    lm.style.top = (8 + (line - 1) * 21 - ta.scrollTop) + 'px';
  }

  function caretPos() {
    var upto = ta.value.slice(0, ta.selectionStart);
    var lines = upto.split('\n');
    return { line: lines.length, col: lines[lines.length - 1].length + 1 };
  }

  function reportCaret() {
    if (onCaret) { var p = caretPos(); onCaret(p.line, p.col); }
  }

  /* 光标一动（点击/方向键）就重画高亮层：当前行、括号配对跟着走 */
  function paintCaret() {
    if (internal) return;
    if (ta.selectionStart === lastCaret) return;
    lastCaret = ta.selectionStart;
    paint();
    paintGutter();
    syncScroll();
  }

  function refresh() {
    lastCaret = ta.selectionStart;
    paint();
    paintGutter();
    syncScroll();
    reportCaret();
    hideTip();
  }

  /* Tab = four spaces; Enter copies the indent (and adds one level
   * after a '{'), so the shape of the program follows itself. */
  function onKeyDown(e) {
    var k = e.key;

    /* 括号 / 引号自动配对 */
    if (k === '(' || k === '[' || k === '{' || k === '"' || k === "'") {
      var s0 = ta.selectionStart, e0 = ta.selectionEnd;
      if (s0 === e0 && (k === '"' || k === "'") && ta.value[s0] === k) {
        /* 光标正好停在配好的引号前：直接穿过去，别再补一对 */
        e.preventDefault();
        ta.setSelectionRange(s0 + 1, s0 + 1);
        return;
      }
      var close = PAIR_CLOSE[k];
      var sel = ta.value.slice(s0, e0);
      e.preventDefault();
      if (sel) {
        /* 有选中文字：用一对括号把它包起来 */
        insertText(k + sel + close);
        ta.setSelectionRange(s0 + 1, s0 + 1 + sel.length);
      } else {
        insertText(k + close);
        ta.setSelectionRange(s0 + 1, s0 + 1);
      }
      return;
    }
    if (k === ')' || k === ']' || k === '}') {
      /* 智能跳过：下一位正好是这个闭括号就不重复输入 */
      if (ta.selectionStart === ta.selectionEnd && ta.value[ta.selectionStart] === k) {
        e.preventDefault();
        ta.setSelectionRange(ta.selectionStart + 1, ta.selectionStart + 1);
        return;
      }
    }

    if (k === 'Tab') {
      e.preventDefault();
      insertText('    ');
      return;
    }
    if (k === 'Enter') {
      e.preventDefault();
      var s = ta.selectionStart, v = ta.value;
      var ls = v.lastIndexOf('\n', s - 1) + 1;
      var head = v.slice(ls, s);
      var indent = (head.match(/^[ \t]*/) || [''])[0];
      if (/\{\s*$/.test(head)) indent += '    ';
      var tailIndent = indent;
      var closing = /^[ \t]*\}/.test(v.slice(s));
      var ins = '\n' + indent + (closing ? '\n' + tailIndent : '');
      var caretAfter = s + 1 + indent.length;   /* 光标停在空行上，别落到 } 那行 */
      insertText(ins);
      ta.setSelectionRange(caretAfter, caretAfter);
      return;
    }
  }

  function insertText(t) {
    /* execCommand keeps the native undo stack in step (Tab/Enter stay
     * undoable with Ctrl+Z); setRangeText would break it, so only fall
     * back to it if the browser refuses. */
    ta.focus();
    if (!document.execCommand || !document.execCommand('insertText', false, t)) {
      var s = ta.selectionStart, e = ta.selectionEnd;
      ta.setRangeText(t, s, e, 'end');
    }
    ta.dispatchEvent(new Event('input', { bubbles: true }));
  }

  function onInput() {
    if (!internal) {
      refresh();
      if (onChange) onChange();
    }
  }

  function init(opts) {
    ta = document.getElementById('code');
    hl = document.getElementById('hl');
    gutter = document.getElementById('gutter');
    wrap = document.getElementById('editor-wrap');
    onCaret = opts.onCaret;
    onChange = opts.onChange;

    lm = document.createElement('div');
    lm.id = 'linemark';
    document.getElementById('editor').appendChild(lm);

    tip = document.createElement('div');
    tip.id = 'tip';
    tip.hidden = true;
    tip.innerHTML = '<div class="tip-sig"></div><div class="tip-note"></div>';
    document.getElementById('editor').appendChild(tip);

    ta.addEventListener('input', onInput);
    ta.addEventListener('scroll', function () { syncScroll(); hideTip(); });
    ta.addEventListener('keydown', onKeyDown);
    ta.addEventListener('keyup', function () { reportCaret(); paintCaret(); });
    ta.addEventListener('click', function () { reportCaret(); paintCaret(); });
    ta.addEventListener('mousemove', onMove);
    ta.addEventListener('mouseleave', hideTip);
    refresh();
  }

  return {
    init: init,
    getCode: function () { return ta.value; },
    setCode: function (text) {
      internal = true;
      ta.value = text;
      refresh();
      internal = false;
    },
    focusLine: function (line, col) {
      var lines = ta.value.split('\n');
      var pos = 0, i;
      for (i = 0; i < line - 1 && i < lines.length; i++) pos += lines[i].length + 1;
      pos += Math.max(0, (col || 1) - 1);
      pos = Math.min(pos, ta.value.length);
      ta.focus();
      ta.setSelectionRange(pos, pos);
      /* scroll so the line sits in the middle */
      var lh = 21;
      ta.scrollTop = Math.max(0, (line - 4) * lh);
      syncScroll();
      reportCaret();
      paintCaret();
    },
    setErrors: function (errs) {
      errLines = {};
      (errs || []).forEach(function (e) {
        if (e.k === 'H') return;
        var cur = errLines[e.line];
        if (e.k === 'E' || !cur) errLines[e.line] = (e.k === 'E') ? 'err' : 'warn';
      });
      paintGutter();
    },
    setReadOnly: function (on) { ta.readOnly = !!on; },
    refresh: refresh
  };
})();
