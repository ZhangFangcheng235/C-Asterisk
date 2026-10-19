/* app.js -- C* Studio: toolbar, panels, status bar and the talks with
 * the little server inside caskide.exe (see editor\webide.c for the API).
 *
 * The page keeps the truth about the open file: its path and encoding.
 * Code lives in the editor; the blocks view is a view of the same code.
 */

var App = (function () {

  /* if anything on the page throws, keep the first message so the
   * page can be inspected from outside (used when testing the app) */
  window.__jserr = null;
  window.addEventListener('error', function (e) {
    if (!window.__jserr)
      window.__jserr = (e.message || 'error') + ' @ ' + (e.filename || '') + ':' + (e.lineno || 0);
  });

  var state = {
    path: '',
    encoding: 'utf-8',
    dirty: false,
    errs: [],
    runId: 0,
    view: localStorage.getItem('cs.view') || 'code',
    outShown: localStorage.getItem('cs.out') !== '0',
    probsShown: true,
    /* 快速编译：优先 tcc / -O0，编译更快；默认开（编辑器 F5 走 --fast），
     * 想要最高运行性能就关掉走 gcc -O2 */
    fast: localStorage.getItem('cs.fast') !== '0'
  };

  /* ---------------- small helpers ---------------- */

  function $(id) { return document.getElementById(id); }

  function api(path, data) {
    return fetch(path, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(data || {})
    }).then(function (r) { return r.json(); });
  }

  function apiGet(path) {
    return fetch(path).then(function (r) { return r.json(); });
  }

  function sleep(ms) { return new Promise(function (r) { setTimeout(r, ms); }); }

  var toastTimer = null;
  function toast(text) {
    var t = $('toast');
    t.textContent = text;
    t.classList.add('show');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () { t.classList.remove('show'); }, 2200);
  }

  function setTitle() {
    var name = state.path ? state.path.replace(/^.*[\\\/]/, '') : '未命名';
    if (state.dirty) name += ' *';
    api('/api/title', { title: name + ' - C* Studio' });
  }

  /* ---------------- status bar ---------------- */

  function setPos(line, col) { $('st-pos').textContent = '行 ' + line + '，列 ' + col; }

  function setPathUI() {
    $('st-path').textContent = state.path || '未命名';
    $('st-enc').textContent = state.encoding === 'gbk' ? 'GBK' : 'UTF-8';
    setTitle();
  }

  function setDirty(d) {
    state.dirty = d;
    $('st-path').innerHTML = (state.path || '未命名') + (d ? ' <span class="dirty-mark">●</span>' : '');
    setTitle();
  }

  function setRunState(text, busy) {
    $('runstate').textContent = text || '';
    $('runstate').className = busy ? 'busy' : '';
    $('btn-run').disabled = !!busy;
    $('btn-stop').disabled = !busy;
  }

  /* ---------------- problems ---------------- */

  var SEV_NAME = { E: '严重', S: '语法', P: '拼写', H: '提示' };

  function renderErrs(errs) {
    state.errs = errs || [];
    var c = { E: 0, S: 0, P: 0, H: 0 };
    state.errs.forEach(function (e) { if (c[e.k] !== undefined) c[e.k]++; });
    [['severe', 'E'], ['syntax', 'S'], ['spell', 'P'], ['hint', 'H']].forEach(function (p) {
      var el = $('st-' + p[0]);
      el.textContent = SEV_NAME[p[1]] + ' ' + c[p[1]];
      el.className = 'chip ' + (p[0] === 'severe' ? 'sev' : p[0] === 'syntax' ? 'syn' : p[0] === 'spell' ? 'spell' : 'hint') + (c[p[1]] ? '' : ' zero');
    });
    var list = $('problems-list'), html = '';
    state.errs.forEach(function (e, i) {
      html += '<div class="p-row p-' + e.k + '" data-i="' + i + '">' +
        '<span class="p-tag">' + SEV_NAME[e.k] + '</span>' +
        '<span class="p-where">' + e.line + ':' + e.col + '</span>' +
        '<span class="p-msg"></span></div>';
    });
    list.innerHTML = html;
    Array.prototype.forEach.call(list.querySelectorAll('.p-msg'), function (el, i) {
      el.textContent = state.errs[i].msg;      /* text, not html */
    });
    $('problems-count').textContent = state.errs.length ? '(' + state.errs.length + ')' : '';
    Editor.setErrors(state.errs);
  }

  function showProblems(errs, log) {
    renderErrs(errs);
    $('problems').classList.remove('collapsed');
    state.probsShown = true;
    if (log && log.trim()) addOutput('\n' + log, 'err');
  }

  /* ---------------- output panel ---------------- */

  function showOutput(show) {
    $('output').classList.toggle('collapsed', !show);
    state.outShown = show;
    localStorage.setItem('cs.out', show ? '1' : '0');
  }

  function setOutput(text) {
    var el = $('output-text');
    el.textContent = text;
    el.scrollTop = el.scrollHeight;
  }

  function addOutput(text, cls) {
    var el = $('output-text');
    var span = document.createElement('span');
    if (cls) span.className = cls;
    span.textContent = text;
    el.appendChild(span);
    el.scrollTop = el.scrollHeight;
  }

  /* ---------------- format ---------------- */

  /* one statement per line, indentation by brace depth, keeping
   * comments and strings untouched */
  function formatCode(src) {
    var out = [], line = '', lineDepth = 0, depth = 0, paren = 0;
    var inStr = '', inBlock = false, started = false;
    var s = src.replace(/\r\n/g, '\n'), i = 0;

    function flush() {
      var t = line.replace(/\s+$/, '');
      out.push(t.trim() ? new Array(lineDepth + 1).join('    ') + t.trim() : '');
      line = '';
      started = false;
    }

    while (i < s.length) {
      var c = s[i], d = s[i + 1];
      if (!started) {
        if (c === ' ' || c === '\t') { i++; continue; }
        if (c === '\n') { flush(); i++; continue; }
        started = true;
        lineDepth = (c === '}' && paren === 0) ? Math.max(0, depth - 1) : depth;
      }
      if (inBlock) {
        line += c;
        if (c === '*' && d === '/') { line += d; inBlock = false; i += 2; continue; }
        if (c === '\n') { flush(); i++; continue; }
        i++; continue;
      }
      if (inStr) {
        line += c;
        if (c === '\\' && d) { line += d; i += 2; continue; }
        if (c === inStr) inStr = '';
        if (c === '\n') { inStr = ''; flush(); }
        i++; continue;
      }
      if (c === '/' && d === '/') {
        var j = s.indexOf('\n', i); if (j < 0) j = s.length;
        line += s.slice(i, j);
        flush();
        i = j + 1;
        continue;
      }
      if (c === '/' && d === '*') { inBlock = true; line += '/*'; i += 2; continue; }
      if (c === '"' || c === "'") { inStr = c; line += c; i++; continue; }
      if (c === '\n') { flush(); i++; continue; }
      if (c === '}' && line.trim()) { flush(); continue; }   /* '}' gets its own line */
      if (c === '(') paren++;
      if (c === ')') paren = Math.max(0, paren - 1);
      if (c === '{') depth++;
      if (c === '}') depth = Math.max(0, depth - 1);
      line += c;
      if (c === ';' && paren === 0) {
        /* more statements on this line? then break here */
        var j2 = i + 1;
        while (j2 < s.length && (s[j2] === ' ' || s[j2] === '\t')) j2++;
        if (j2 < s.length && s[j2] !== '\n') { flush(); i = j2; continue; }
      }
      i++;
    }
    if (line.trim() || started) flush();
    while (out.length && !out[out.length - 1].trim()) out.pop();
    return out.join('\n') + '\n';
  }

  /* ---------------- check / run ---------------- */

  var checkTimer = null, checkSeq = 0;

  function scheduleCheck() {
    clearTimeout(checkTimer);
    checkTimer = setTimeout(doCheck, 450);
  }

  function doCheck() {
    var seq = ++checkSeq;
    var text = Editor.getCode();
    api('/api/check', { text: text, path: state.path }).then(function (r) {
      if (seq !== checkSeq) return;      /* a newer check is on the way */
      renderErrs(r.errs);
    }).catch(function () { /* the server is busy; the next edit retries */ });
  }

  function run() {
    setRunState('编译中…', true);
    showOutput(true);
    $('output-text').innerHTML = '';
    api('/api/run', { text: Editor.getCode(), path: state.path, fast: state.fast }).then(function (r) {
      if (!r.ok) {
        setRunState('', false);
        showProblems(r.errs, r.log);
        if (!r.errs.length) addOutput('编译失败：\n' + (r.log || '（没有信息）'), 'err');
        return;
      }
      renderErrs(r.errs);
      state.runId = r.id;
      setRunState('运行中 #' + r.id, true);
      $('output-input-row').hidden = false;
      pollOutput(r.id);
    }).catch(function () {
      setRunState('', false);
      addOutput('\n出错了：编辑器与后台失去联系', 'err');
    });
  }

  function pollOutput(id) {
    function step() {
      if (state.runId !== id) return;
      apiGet('/api/output?id=' + id).then(function (r) {
        if (state.runId !== id) return;
        setOutput(r.out);
        if (r.done) {
          state.runId = 0;
          setRunState('', false);
          $('output-input-row').hidden = true;
          addOutput(r.code === 0 ? '\n[结束，退出码 0]\n' : '\n[结束，退出码 ' + r.code + ']\n', 'dim');
          $('output-text').scrollTop = $('output-text').scrollHeight;
          scheduleCheck();
        } else {
          setTimeout(step, 120);
        }
      }).catch(function () {
        state.runId = 0;
        setRunState('', false);
      });
    }
    step();
  }

  function stop() {
    if (!state.runId) return;
    api('/api/stop', { id: state.runId });
    state.runId = 0;
    setRunState('', false);
    $('output-input-row').hidden = true;
    addOutput('\n[已停止]\n', 'dim');
  }

  /* ---------------- files ---------------- */

  function loadText(path, text, encoding) {
    state.path = path;
    state.encoding = encoding || 'utf-8';
    Editor.setCode(text);
    setDirty(false);
    setPathUI();
    renderErrs([]);
    if (state.view === 'blocks') Blocks.refresh(text);
    scheduleCheck();
    localStorage.setItem('cs.last', path);
  }

  function doOpen() {
    api('/api/dialog/open').then(function (r) {
      if (r.cancelled || !r.path) return;
      return api('/api/load', { path: r.path }).then(function (l) {
        if (l.error) { toast(l.error); return; }
        loadText(l.path, l.text, l.encoding);
      });
    });
  }

  function doSaveAs() {
    api('/api/dialog/saveas', { text: Editor.getCode(), encoding: state.encoding }).then(function (r) {
      if (r.cancelled || !r.path) return;
      state.path = r.path;
      setDirty(false);
      setPathUI();
      toast('已保存 ' + r.path.replace(/^.*[\\\/]/, ''));
      scheduleCheck();
      if (state.view === 'blocks') Blocks.refresh(Editor.getCode());
    });
  }

  function doSave() {
    if (!state.path) { doSaveAs(); return Promise.resolve(); }
    return api('/api/save', { path: state.path, text: Editor.getCode(), encoding: state.encoding })
      .then(function (r) {
        if (r.error) { toast(r.error); return; }
        setDirty(false);
        toast('已保存');
        if (state.view === 'blocks') Blocks.refresh(Editor.getCode());
      });
  }

  function doNew() {
    if (state.dirty && !confirm('现在的程序还没保存，真的要新建吗？')) return;
    loadText('', 'function main() {\n    \n}\n', 'utf-8');
    Editor.focusLine(2, 5);
  }

  function doEnc(to) {
    if (!state.path) { toast('先保存文件才能换编码'); return; }
    api('/api/enc', { path: state.path, to: to }).then(function (r) {
      if (r.error) { toast(r.error); return; }
      state.encoding = to;
      if (typeof r.text === 'string') {
        Editor.setCode(r.text);
        if (state.view === 'blocks') Blocks.refresh(r.text);
      }
      setPathUI();
      toast(to === 'gbk' ? '已转成 GBK' : '已转成 UTF-8');
      scheduleCheck();
    });
  }

  /* ---------------- menus ---------------- */

  function popup(items, anchor) {
    var m = document.createElement('div');
    m.className = 'menu';
    items.forEach(function (it) {
      var d = document.createElement('div');
      d.textContent = it.label;
      d.onclick = function () { close(); it.fn(); };
      m.appendChild(d);
    });
    document.body.appendChild(m);
    var r = anchor.getBoundingClientRect();
    m.style.left = r.left + 'px';
    m.style.top = (r.bottom + 4) + 'px';
    function close() { document.removeEventListener('mousedown', away, true); m.remove(); }
    function away(e) { if (!m.contains(e.target)) close(); }
    setTimeout(function () { document.addEventListener('mousedown', away, true); }, 0);
  }

  /* ---------------- view switching ---------------- */

  function setView(v) {
    state.view = v;
    localStorage.setItem('cs.view', v);
    $('btn-view-code').classList.toggle('on', v === 'code');
    $('btn-view-blocks').classList.toggle('on', v === 'blocks');
    $('editor-wrap').hidden = v !== 'code';
    $('blocks-wrap').hidden = v !== 'blocks';
    if (v === 'blocks') Blocks.refresh(Editor.getCode());
    else Editor.refresh();
  }

  /* ---------------- help ---------------- */

  function showHelp() {
    $('help').hidden = false;
  }

  /* ---------------- wiring ---------------- */

  function wire() {
    $('btn-new').onclick = doNew;
    $('btn-open').onclick = doOpen;
    $('btn-save').onclick = doSave;
    $('btn-saveas').onclick = doSaveAs;
    $('btn-run').onclick = run;
    $('btn-fast').onclick = function () {
      state.fast = !state.fast;
      localStorage.setItem('cs.fast', state.fast ? '1' : '0');
      syncFastBtn();
      toast(state.fast ? '快速编译：开（tcc／-O0，编译更快）'
                       : '快速编译：关（gcc -O2，运行最快）');
    };
    $('btn-stop').onclick = stop;
    $('btn-view-code').onclick = function () { setView('code'); };
    $('btn-view-blocks').onclick = function () { setView('blocks'); };
    $('btn-format').onclick = function () {
      var t = formatCode(Editor.getCode());
      Editor.setCode(t);
      setDirty(true);
      if (state.view === 'blocks') Blocks.refresh(t);
      toast('已理清结构');
      scheduleCheck();
    };
    $('btn-enc').onclick = function () {
      popup([
        { label: '转成 UTF-8（推荐）', fn: function () { doEnc('utf-8'); } },
        { label: '转成 GBK（给老环境）', fn: function () { doEnc('gbk'); } }
      ], $('btn-enc'));
    };
    $('btn-help').onclick = showHelp;
    $('help-close').onclick = function () { $('help').hidden = true; };
    $('help').onclick = function (e) { if (e.target === $('help')) $('help').hidden = true; };

    syncFastBtn();

    function syncFastBtn() {
      var b = $('btn-fast');
      b.classList.toggle('on', !!state.fast);
      b.title = state.fast
        ? '快速编译：开（tcc／-O0，编译更快）；点一下关掉走 gcc -O2'
        : '快速编译：关（gcc -O2，运行最快）；点一下开启 tcc／-O0 快编';
    }

    /* the menu bar reuses popup(); every item calls the same function the
     * matching toolbar button already calls, so behaviour stays identical. */
    function bindMenu(id, items) {
      var el = $(id);
      if (el) el.onclick = function () { popup(items, el); };
    }
    bindMenu('menu-file', [
      { label: '新建 (Ctrl+N)', fn: doNew },
      { label: '打开… (Ctrl+O)', fn: doOpen },
      { label: '保存 (Ctrl+S)', fn: function () { doSave(); } },
      { label: '另存为… (Ctrl+Shift+S)', fn: doSaveAs }
    ]);
    bindMenu('menu-edit', [
      { label: '格式化（理清缩进）', fn: function () { $('btn-format').onclick(); } },
      { label: '转成 UTF-8（推荐）', fn: function () { doEnc('utf-8'); } },
      { label: '转成 GBK（给老环境）', fn: function () { doEnc('gbk'); } }
    ]);
    bindMenu('menu-view', [
      { label: '代码视图', fn: function () { setView('code'); } },
      { label: '积木视图', fn: function () { setView('blocks'); } },
      { label: '输出', fn: function () { showOutput(true); } },
      { label: '问题', fn: function () { $('problems').classList.remove('collapsed'); } }
    ]);
    bindMenu('menu-build', [
      { label: '运行 (F5)', fn: run },
      { label: '停止 (Esc)', fn: stop }
    ]);
    bindMenu('menu-debug', [
      { label: '运行 (F5)', fn: run },
      { label: '停止 (Esc)', fn: stop }
    ]);
    bindMenu('menu-help', [
      { label: '使用帮助 (F1)', fn: showHelp }
    ]);

    $('btn-hide-output').onclick = function () { showOutput(false); };
    $('btn-clear').onclick = function () { setOutput(''); };
    $('btn-hide-problems').onclick = function () {
      $('problems').classList.toggle('collapsed');
    };

    $('problems-list').onclick = function (e) {
      var row = e.target.closest('.p-row');
      if (!row) return;
      var err = state.errs[+row.dataset.i];
      if (!err) return;
      if (state.view !== 'code') setView('code');
      Editor.focusLine(err.line, err.col);
    };

    $('output-input').addEventListener('keydown', function (e) {
      if (e.key !== 'Enter') return;
      var v = $('output-input').value;
      $('output-input').value = '';
      if (state.runId) api('/api/send', { id: state.runId, text: v });
      else toast('程序没有在等输入');
    });

    document.addEventListener('keydown', function (e) {
      if (e.key === 'F5') { e.preventDefault(); run(); }
      else if (e.key === 'F1') { e.preventDefault(); showHelp(); }
      else if (e.key === 'Escape') { if (state.runId) stop(); }
      else if ((e.ctrlKey || e.metaKey) && e.key === 's') {
        e.preventDefault();
        if (e.shiftKey) doSaveAs(); else doSave();
      }
      else if ((e.ctrlKey || e.metaKey) && e.key === 'o') { e.preventDefault(); doOpen(); }
      else if ((e.ctrlKey || e.metaKey) && e.key === 'n') { e.preventDefault(); doNew(); }
    });
  }

  function boot() {
    Editor.init({
      onCaret: setPos,
      onChange: function () {
        setDirty(true);
        scheduleCheck();
        if (state.view === 'blocks') Blocks.refresh(Editor.getCode());
      }
    });
    Blocks.init({
      canvas: $('blocks-canvas'),
      palette: $('blocks-palette'),
      getText: function () { return Editor.getCode(); },
      setText: function (code) {
        Editor.setCode(code);
        setDirty(true);
        scheduleCheck();
      },
      setStatus: toast
    });
    wire();
    showOutput(state.outShown);
    $('problems').classList.remove('collapsed');
    setView(state.view);

    /* 头文件清单：给 use 积木配概述卡片 + 给积木箱做门禁 */
    apiGet('header-info.json').then(function (info) {
      Blocks.setHeaderInfo(info || {});
      /* 清单里有的头文件 = 扫描成功（已知头文件）；自定义 .h 未确认前显示通用卡片 */
      var status = {};
      Object.keys(info || {}).forEach(function (k) { status[k] = { ok: true }; });
      Blocks.setHeaderStatus(status);
      if (state.view === 'blocks') Blocks.refresh(Editor.getCode());
    }).catch(function () { /* 清单没加载到也能照常显示积木，只是概述退化成通用文案 */ });

    api('/api/info').then(function (info) {
      if (info.open) {
        return api('/api/load', { path: info.open }).then(function (l) {
          if (l.error) { toast(l.error); return; }
          loadText(l.path, l.text, l.encoding);
        });
      }
      var last = localStorage.getItem('cs.last');
      Editor.setCode('function main() {\n    print "你好，C*！";\n}\n');
      setDirty(false);
      setPathUI();
      if (state.view === 'blocks') Blocks.refresh(Editor.getCode());
      if (last) $('st-path').textContent = '未命名（上次打开过 ' + last.replace(/^.*[\\\/]/, '') + '，按 Ctrl+O 打开）';
      Editor.focusLine(2, 5);
      scheduleCheck();
    });
  }

  return { boot: boot, toast: toast };
})();

window.addEventListener('DOMContentLoaded', App.boot);
