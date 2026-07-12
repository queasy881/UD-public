#!/bin/sh
# install.sh -- bootstrap installer for UD (Linux / macOS / MSYS2).
#
# Curl-fetchable:
#   curl -fsSL https://raw.githubusercontent.com/queasy881/UD-public/main/install.sh | sh
#
# It builds `ud`, puts it on your PATH, associates .ud files where possible,
# installs the VS Code extension, and runs a self-check. Nothing needs root.
set -eu

REPO="https://github.com/queasy881/UD-public.git"
PREFIX="${UD_PREFIX:-$HOME/.ud}"
BIN_DIR="$PREFIX/bin"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# --- 1. locate or fetch the source ------------------------------------------
if [ -f main.c ] && [ -f vm.c ] && [ -f build.sh ]; then
    SRC_DIR="$(pwd)"
    say "Building from the current directory: $SRC_DIR"
else
    command -v git >/dev/null 2>&1 || die "git is required to fetch UD."
    SRC_DIR="$PREFIX/src"
    say "Cloning UD into $SRC_DIR"
    rm -rf "$SRC_DIR"
    mkdir -p "$PREFIX"
    git clone --depth 1 "$REPO" "$SRC_DIR"
fi

# --- 2. build ----------------------------------------------------------------
command -v "${CC:-gcc}" >/dev/null 2>&1 || \
    die "a C compiler (gcc or clang) is required. Install one and re-run."
say "Compiling the interpreter"
( cd "$SRC_DIR" && sh build.sh )

# --- 3. install the binary ---------------------------------------------------
mkdir -p "$BIN_DIR"
if [ -f "$SRC_DIR/ud.exe" ]; then
    EXE=ud.exe
else
    EXE=ud
fi
cp -f "$SRC_DIR/$EXE" "$BIN_DIR/$EXE"
chmod +x "$BIN_DIR/$EXE"
say "Installed $BIN_DIR/$EXE"

# --- 4. put it on PATH -------------------------------------------------------
add_path_line='export PATH="'"$BIN_DIR"':$PATH"'
added=0
for rc in "$HOME/.bashrc" "$HOME/.zshrc" "$HOME/.profile"; do
    [ -f "$rc" ] || continue
    if ! grep -qs "$BIN_DIR" "$rc"; then
        printf '\n# added by the UD installer\n%s\n' "$add_path_line" >> "$rc"
        say "Added $BIN_DIR to PATH in $rc"
    fi
    added=1
done
if [ "$added" -eq 0 ]; then
    printf '\n# added by the UD installer\n%s\n' "$add_path_line" >> "$HOME/.profile"
    say "Added $BIN_DIR to PATH in $HOME/.profile"
fi

# --- 5. associate .ud files (best effort, desktop Linux) ---------------------
if command -v xdg-mime >/dev/null 2>&1 && [ -d "$HOME/.local/share" ]; then
    mimedir="$HOME/.local/share/mime/packages"
    mkdir -p "$mimedir"
    cat > "$mimedir/ud.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="text/x-ud">
    <comment>UD source file</comment>
    <glob pattern="*.ud"/>
  </mime-type>
</mime-info>
XML
    update-mime-database "$HOME/.local/share/mime" >/dev/null 2>&1 || true
    say "Registered the .ud file type"
fi

# --- 6. install the VS Code extension ----------------------------------------
if [ -d "$SRC_DIR/vscode-ud" ]; then
    for extroot in "$HOME/.vscode/extensions" "$HOME/.vscode-insiders/extensions"; do
        base="$(dirname "$extroot")"
        [ -d "$base" ] || continue
        mkdir -p "$extroot/ud-lang"
        cp -R "$SRC_DIR/vscode-ud/." "$extroot/ud-lang/"
        say "Installed the VS Code extension into $extroot/ud-lang"
    done
fi

# --- 7. self-check -----------------------------------------------------------
say "Self-check:"
"$BIN_DIR/$EXE" --version || die "the installed binary did not run."

cat <<EOF

UD is installed.

  Binary : $BIN_DIR/$EXE
  Try    : ud $SRC_DIR/examples/hello.ud

Open a new terminal (or 'source' your shell profile) so PATH takes effect.
EOF
