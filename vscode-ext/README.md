# C Asterisk (C*) for VS Code

Language support for **C Asterisk** (written `C*`), the beginner-friendly C-style
programming language that uses `.cask` files.

会 Excel 和 C 就会写 — 语法高亮、代码片段、注释切换一应俱全。

## Features

- **Syntax highlighting** for `.cask` files:
  - keywords — `function` / `func` `if` `else` `while` `for` `do` `until`
    `foreach` `in` `switch` `case` `default` `break` `continue` `return`
  - I/O & modules — `print` (old name `say`) `show` `input` `use` `constant` / `const`
  - logic — `and` `or` `not`, constants `true` `false`
  - types — `int` `float` `string` `char` `bool` `none`
    (the 0.3 names `number` `real` `text` `letter` are highlighted too)
  - **Excel-style built-in functions** — `IF AND OR NOT ABS SIGN MOD ROUND INT
    POWER SQRT EXP LN LOG PI RAND RANDBETWEEN SUM AVERAGE COUNT MAX MIN LEN
    LEFT RIGHT MID UPPER LOWER TRIM CONCAT VALUE` plus `sin cos tan at len ...`
  - **Chinese identifiers** are highlighted too (`int 苹果 = 3;`)
  - numbers, strings, escapes, `//` and `/* */` comments, operators
- **Language configuration**: toggle comments (`Ctrl+/`), auto-closing
  brackets/quotes, bracket matching, indentation rules.
- **Snippets**: `main`, `start`, `if`, `ifelse`, `while`, `for`, `repeat`,
  `print`, `say`, `show`, `input`, `function`, `fnv`, `var`, `constant`, `use`, `IF`, `SUM`,
  and for window programs (`use "C_OPED.cash";`) `win`, `winonclick`, `winuse`.

## Writing & running a program

```cask
// hello.cask
function main() {
    print "Hello, C Asterisk!";
}
```

Compile with the C* toolchain (see the main project README):

```powershell
bin\caskc.exe hello.cask --run
```

Or let VS Code do it — add a task that calls the compiler
(`bin\caskc.exe ${file} --run`).

## Requirements

- VS Code 1.75 or newer.
- The C* toolchain (`bin\caskc.exe`) is only needed to *run* programs, not to
  edit them.

## Installation

From the Marketplace: click **Install**, or from a `.vsix` file:

```powershell
code --install-extension c-asterisk-0.5.0.vsix
```

## License

MIT
