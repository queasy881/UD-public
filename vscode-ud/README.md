# UD Language for VS Code

Syntax highlighting and a one-click **Run** button for the
[UD programming language](https://github.com/queasy881/UD-public).

## Features

- Syntax highlighting for `.ud` files (keywords, types, strings, numbers,
  comments, operators, `cout`/`cin`/`len`/`type`).
- A **Run** button (▶) in the editor title bar for any `.ud` file — or press
  **F5**. It runs `ud "<file>"` in an integrated terminal.
- **UD: Build to .ldx** in the Command Palette, which runs `ud build`.
- Lua-style auto-indentation (indents after `function`/`if…then`/`while…do`/
  `for…do`/`struct`, dedents on `end`/`else`/`elseif`/`unless`).

## Requirements

The `ud` executable must be on your PATH. The UD bootstrap installer sets this up
for you; run `ud --version` in a terminal to confirm.

## Install (from source)

Copy this folder to your VS Code extensions directory:

- Windows: `%USERPROFILE%\.vscode\extensions\ud-lang`
- macOS/Linux: `~/.vscode/extensions/ud-lang`

Then reload VS Code. The UD bootstrap installer does this automatically.
