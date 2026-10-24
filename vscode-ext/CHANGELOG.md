# Change Log

## 0.5.1

- Extension is now a program (adds `extension.js`): besides highlighting
  and snippets it ships a **Run** button (editor title bar) that compiles and
  runs the open `.cask` with `caskc` (adds `--console` for window programs so
  `print` is visible), and **syntax checking** — `caskc --check --diag` whose
  machine-readable `P/S/E/H|line|col|message` lines are parsed into the
  VS Code **Problems** panel (auto on open/save, or the "C*: 语法检查"
  command). Point `casterisk.caskcPath` at your `caskc` if it is not on a
  `bin/` folder above the file.
- `.cask` files get a per-file **language icon** (`languages.icon`), not just
  the package icon.
- Follows C* 0.5.x headers `C_OS.cash` / `C_NET.cash` / `C_SC.cash`
  (operating-system info, network/security helpers, portable S/C interop) plus
  the 0.5.0 `C_IO.cash`.

## 0.5.0

- Follows C* 0.5.0. The compiler now emits x86-64 assembly directly
  (`.cask` → `.s` → `as` → `ld`), so no C compiler is involved when
  building a `.cask` program; only `as`/`ld` plus `libcastrt.a`.
- New header `use "C_IO.cash";`: `readfile` `writefile` `appendfile`
  `fileexists` `makedir` `appdir` `envget` `launchwait` (file and system
  helpers that also work in console programs).
- Window styling on Windows is real now: owner-drawn buttons, window
  background, edit-control background, and DPI-correct `csfont` sizes.
- New sample `examples/mc_launcher.cask` (Minecraft launcher built with
  `C_IO.cash` + `C_CSS.cash`).
- Fixed in the assembly backend: `TRUE()/FALSE()` in expressions, signed
  `char` operands, excess array initializers, `INT64_MIN % -1`, nested
  calls to non-ASCII function names, and `--fast` letter range checks.

## 0.4.2

- Language now builds and runs on **Linux and macOS** as well as Windows:
  the compiler is portable (`.cask` → C → `gcc` → native binary), and the
  window header `C_OPED.cash` is cross-platform — Win32 on Windows, GTK3 on
  Linux. This VS Code extension is unchanged and works on all three.

- Type names follow C* 0.4.2: `int` `float` `string` `char` are highlighted as the
  canonical types; the 0.3 spelling `number` `real` `text` `letter` still highlights.
- New keywords highlighted: `do` `until` `foreach` `in` `switch` `case` `default`.
- `VALUE` / `VAL` (text to number) added to the built-in function list.
- Built-ins added in the 0.4.2 language audit, all highlighted:
  `ATAN2` `TRUNC` `LOG10` and the Excel-style math names
  `SIN` `COS` `TAN` `ASIN` `ACOS` `ATAN` `FLOOR` `CEILING`,
  plus the text helpers `FIND` `SUBSTITUTE` `REPT` `EXACT` `CHAR` `CODE`.
- Lowercase legacy spellings (`abs` `len` `sin` `floor` `ceil` `log10` …) still
  highlight; the compiler marks them soft-deprecated with a blue H-hint, so new
  code should use the uppercase names.
- The `rcl` easter egg (rcl永远爱方程235) highlights as a built-in.
- New window-program snippets: `win` (`use "C_OPED.cash";` + window + `run()`),
  `winuse`, `winonclick`.
- New snippets: `atan2`, `trunc`, `find`, `subst`, `rept`.
- Snippets that used `number` now use `int`.

## 0.2.0

- Keyword update for C* 0.2:
  - `print` is now the main output keyword (`say` is kept as an alias).
  - `function` / `func` and `int` are recognized.
- Highlight **Chinese identifiers** (`number 苹果 = 3;`), including
  function calls written with non-ASCII letters.
- New snippets: `main` (`function main() { ... }`), `print`, `function`,
  `fnv`, `var`, `repeat`; the `say` snippet now expands to `print`.
- README updated with the 0.2 syntax.

## 0.1.0

- Initial release.
- Syntax highlighting for `.cask` files (keywords, types, Excel-style built-in
  functions, numbers, strings, comments, operators).
- Language configuration: line/block comments, brackets, auto-closing pairs,
  indentation rules.
- Snippets: `start`, `if`, `ifelse`, `while`, `for`, `say`, `show`, `input`,
  `func`, `constant`, `use`, `IF`, `SUM`.
