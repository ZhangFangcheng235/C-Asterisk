/* extension.js -- C Asterisk (C*) VS Code extension
 *
 * 在语法高亮 / 代码片段之外，再加两样：
 *   · 运行按钮（编辑器标题栏 ▸）：用 caskc 编译并运行当前 .cask
 *   · 语法检查（标题栏 ✓ + 自动）：跑 `caskc --check --diag`，把
 *     机器可读的 P/S/E/H 行解析进 VS Code 的 Problems 面板
 *
 * 依赖：caskc 可执行文件（本仓库 build 出来的 bin/caskc[.exe]）。
 * 路径解析顺序：
 *   1. casterisk.caskcPath 设置里写的绝对路径
 *   2. 从当前文件所在目录逐级向上找  bin/caskc(.exe)
 *   3. 系统 PATH 里的 caskc
 */
'use strict';

const vscode = require('vscode');
const cp = require('child_process');
const path = require('path');
const fs = require('fs');

let checkTimer = null;   // 自动检查的防抖
let lastRunTerminal = null;

/* ---------- 找 caskc ---------- */
function findCaskc() {
  const cfg = vscode.workspace.getConfiguration('casterisk');
  const explicit = (cfg.get('caskcPath') || '').trim();
  if (explicit) return explicit;
  // 从当前活动文件目录逐级向上找 bin/caskc(.exe)
  const ed = vscode.window.activeTextEditor;
  let base = ed ? path.dirname(ed.document.uri.fsPath) : process.cwd();
  let dir = base;
  for (let i = 0; i < 8; i++) {
    for (const cand of [path.join(dir, 'bin', 'caskc.exe'),
                        path.join(dir, 'bin', 'caskc')]) {
      if (fs.existsSync(cand)) return cand;
    }
    const parent = path.dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  // 退回 PATH
  return 'caskc';
}

/* ---------- 命令：语法检查 ---------- */
function doCheck(doc) {
  const caskc = findCaskc();
  // 写到一个临时文件再喂给 caskc（--diag 的行里只给 line/col，不带文件名）
  const tmp = path.join(require('os').tmpdir(),
                        'caskc-check-' + Date.now() + path.extname(doc.uri.fsPath));
  fs.writeFileSync(tmp, doc.getText(), 'utf8');

  const isWindows = process.platform === 'win32';
  const caskcArg = caskc.replace(/\//g, isWindows ? '\\' : '/');
  const outExe = path.join(require('os').tmpdir(), 'caskc-check-out' + Date.now());
  const args = [tmp, '--check', '--diag', '-o', outExe];

  cp.execFile(caskcArg, args, { maxBuffer: 1 << 20 }, (err, stdout) => {
    // 删临时物
    for (const f of [tmp, outExe]) { try { fs.unlinkSync(f); } catch (e) {} }
    if (!doc.isClosed) showDiagnostics(doc, stdout || '');
  });
}

/* 解析 "K|line|col|消息" 行 -> Problems 面板 */
function showDiagnostics(doc, stdout) {
  const base = vscode.languages.getDiagnosticCollection('casterisk');
  const dlist = [];
  const SEV = { P: vscode.DiagnosticSeverity.Warning,
                S: vscode.DiagnosticSeverity.Error,
                E: vscode.DiagnosticSeverity.Error,
                H: vscode.DiagnosticSeverity.Information };
  const LABEL = { P: '拼写', S: '语法', E: '严重', H: '提示' };
  const uri = doc.uri;
  for (const ln of stdout.split(/\r?\n/)) {
    const m = /^([PSEH])\|(\d+)\|(\d+)\|(.*)$/.exec(ln);
    if (!m) continue;
    const sev = SEV[m[1]] || vscode.DiagnosticSeverity.Information;
    const line = Math.max(1, parseInt(m[2], 10));
    const col = Math.max(1, parseInt(m[3], 10));
    const msg = m[4].trim();
    const range = new vscode.Range(line - 1, col - 1, line - 1, col - 1);
    const d = new vscode.Diagnostic(range, '[' + LABEL[m[1]] + '] ' + msg);
    d.severity = sev;
    d.source = 'C*';
    dlist.push(d);
  }
  base.set(uri, dlist);
}

/* ---------- 命令：运行 ---------- */
function doRun() {
  const ed = vscode.window.activeTextEditor;
  if (!ed || ed.document.languageId !== 'casterisk') {
    vscode.window.showWarningMessage('当前文件不是 .cask，没有 C* 程序可运行。');
    return;
  }
  const cfg = vscode.workspace.getConfiguration('casterisk');
  const attachConsole = cfg.get('runAttachConsole', true);
  // 先把当前缓冲区保存的文本喂给 caskc（不强制覆盖磁盘：用临时文件）
  const tmp = path.join(require('os').tmpdir(),
                        'caskc-run-' + Date.now() + '.cask');
  fs.writeFileSync(tmp, ed.document.getText(), 'utf8');

  const caskc = findCaskc();
  const isWindows = process.platform === 'win32';
  const outExe = path.join(require('os').tmpdir(),
                           'caskc-run-' + Date.now() + (isWindows ? '.exe' : ''));
  const args = [tmp, '-o', outExe];
  if (attachConsole) args.push('--console');
  const exeArg = outExe.replace(/\//g, isWindows ? '\\' : '/');

  let term = lastRunTerminal;
  if (!term || !term.exitStatus) term = vscode.window.createTerminal('C* Run');
  lastRunTerminal = term;
  term.show();
  // 编译并运行；把编译错误也显示在终端
  if (isWindows) {
    term.sendText(
      `^& "` + caskc + '" "' + tmp + '" -o "' + outExe + '" ' +
      (attachConsole ? '--console ' : '') + '& "' + exeArg + '"`');
  } else {
    term.sendText(
      caskc + ' "' + tmp + '" -o "' + outExe + '" ' +
      (attachConsole ? '--console ' : '') + '&& "' + exeArg + '"');
  }
  setTimeout(() => { try { fs.unlinkSync(tmp); } catch (e) {} }, 5000);
}

/* ---------- 激活 ---------- */
function activate(context) {
  const runCmd = vscode.commands.registerCommand('casterisk.run', doRun);
  const checkCmd = vscode.commands.registerCommand('casterisk.check',
    () => { const e = vscode.window.activeTextEditor; if (e) doCheck(e.document); });
  context.subscriptions.push(runCmd, checkCmd);

  const auto = () => vscode.workspace.getConfiguration('casterisk').get('autoCheck', true);

  // 打开 / 自动保存：首次打开就检查
  const onOpen = vscode.workspace.onDidOpenTextDocument((d) => {
    if (d.languageId === 'casterisk' && auto()) scheduleCheck(d);
  });
  const onBuf = vscode.workspace.onDidChangeTextDocument((e) => {
    if (e.document.languageId === 'casterisk' && auto()) scheduleCheck(e.document);
  });
  const onSave = vscode.workspace.onDidSaveTextDocument((d) => {
    if (d.languageId === 'casterisk' &&
        vscode.workspace.getConfiguration('casterisk').get('checkOnSave', true)) {
      doCheck(d);
    }
  });
  context.subscriptions.push(onOpen, onBuf, onSave);

  // 已打开的 .cask 文件立即检查一遍
  for (const d of vscode.workspace.textDocuments) {
    if (d.languageId === 'casterisk' && auto()) doCheck(d);
  }
}

function scheduleCheck(doc) {
  if (checkTimer) clearTimeout(checkTimer);
  checkTimer = setTimeout(() => { if (!doc.isClosed) doCheck(doc); }, 450);
}

function deactivate() {}

module.exports = { activate, deactivate };
