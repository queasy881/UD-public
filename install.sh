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
ASSET_DIR="$PREFIX/assets"

say()  { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31mxx\033[0m %s\n' "$*" >&2; exit 1; }

# --- 1. locate or fetch the source ------------------------------------------
if [ -f src/main.c ] && [ -f src/vm.c ] && [ -f build.sh ]; then
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

mkdir -p "$ASSET_DIR"
for asset in ud-source.ico ud-bytecode.ico ud-source.png ud-bytecode.png; do
    [ -f "$SRC_DIR/assets/$asset" ] && cp -f "$SRC_DIR/assets/$asset" "$ASSET_DIR/$asset"
done

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

# --- 5. associate .ud and .ldx files (best effort, desktop Linux) -----------
if command -v xdg-mime >/dev/null 2>&1 && [ -d "$HOME/.local/share" ]; then
    share="$HOME/.local/share"
    mimedir="$share/mime/packages"
    appdir="$share/applications"
    mkdir -p "$mimedir" "$appdir"

    cat > "$mimedir/ud.xml" <<'XML'
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="text/x-ud">
    <comment>UD source file</comment>
    <glob pattern="*.ud"/>
  </mime-type>
  <mime-type type="application/x-ud-bytecode">
    <comment>UD compiled program</comment>
    <glob pattern="*.ldx"/>
  </mime-type>
</mime-info>
XML
    update-mime-database "$share/mime" >/dev/null 2>&1 || true

    # .ud opens the source; .ldx runs the compiled program. Console apps, so Terminal=true.
    cat > "$appdir/ud-source.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=UD
Exec=$BIN_DIR/$EXE %f
Icon=text-x-ud
MimeType=text/x-ud;
NoDisplay=true
Terminal=true
EOF
    cat > "$appdir/ud-bytecode.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=UD (run compiled)
Exec=$BIN_DIR/$EXE run %f
Icon=application-x-ud-bytecode
MimeType=application/x-ud-bytecode;
NoDisplay=true
Terminal=true
EOF
    update-desktop-database "$appdir" >/dev/null 2>&1 || true
    xdg-mime default ud-source.desktop text/x-ud >/dev/null 2>&1 || true
    xdg-mime default ud-bytecode.desktop application/x-ud-bytecode >/dev/null 2>&1 || true

    if command -v xdg-icon-resource >/dev/null 2>&1; then
        [ -f "$SRC_DIR/assets/ud-source.png" ] && xdg-icon-resource install \
            --context mimetypes --size 256 "$SRC_DIR/assets/ud-source.png" \
            text-x-ud >/dev/null 2>&1 || true
        [ -f "$SRC_DIR/assets/ud-bytecode.png" ] && xdg-icon-resource install \
            --context mimetypes --size 256 "$SRC_DIR/assets/ud-bytecode.png" \
            application-x-ud-bytecode >/dev/null 2>&1 || true
    fi
    say "Registered the .ud and .ldx file types"
fi

# --- 6. install the VS Code extension ----------------------------------------
# Prefer the `code` CLI with a .vsix: that writes VS Code's extensions.json
# cache, which a raw folder-copy skips (so a copied folder never activates).
if [ -d "$SRC_DIR/editor/vscode" ]; then
    vsdir="$SRC_DIR/editor/vscode"
    code_cli=""
    for c in code code-insiders; do
        if command -v "$c" >/dev/null 2>&1; then code_cli="$c"; break; fi
    done
    if [ -n "$code_cli" ]; then
        # Build a fresh .vsix if Python is on hand; else use the shipped one.
        for p in python3 python; do
            if command -v "$p" >/dev/null 2>&1; then "$p" "$vsdir/make_vsix.py" "$vsdir" || true; break; fi
        done
        vsix="$(ls -t "$vsdir"/*.vsix 2>/dev/null | head -n1 || true)"
        if [ -n "$vsix" ] && "$code_cli" --install-extension "$vsix" --force; then
            say "Installed the VS Code extension via $code_cli"
        else
            code_cli=""
        fi
    fi
    if [ -z "$code_cli" ]; then
        for extroot in "$HOME/.vscode/extensions" "$HOME/.vscode-insiders/extensions"; do
            base="$(dirname "$extroot")"
            [ -d "$base" ] || continue
            dest="$extroot/ud.ud-lang-1.0.0"
            mkdir -p "$dest"
            cp -R "$vsdir/." "$dest/"
            rm -f "$dest/make_vsix.py" "$dest"/*.vsix
            say "Copied the VS Code extension into $dest (restart VS Code to load it)"
        done
    fi
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
