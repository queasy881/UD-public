// UD language extension: a Run button and a Build command that shell out to `ud`.
const vscode = require("vscode");

let terminal = null;

function udTerminal() {
    if (!terminal || terminal.exitStatus !== undefined) {
        terminal = vscode.window.createTerminal("UD");
    }
    return terminal;
}

async function currentFile() {
    const editor = vscode.window.activeTextEditor;
    if (!editor || editor.document.languageId !== "ud") {
        vscode.window.showErrorMessage("UD: open a .ud file first.");
        return null;
    }
    if (editor.document.isDirty || editor.document.isUntitled) {
        await editor.document.save();
    }
    return editor.document.fileName;
}

function activate(context) {
    const run = vscode.commands.registerCommand("ud.run", async () => {
        const file = await currentFile();
        if (!file) return;
        const t = udTerminal();
        t.show(true);
        t.sendText(`ud "${file}"`);
    });

    const build = vscode.commands.registerCommand("ud.build", async () => {
        const file = await currentFile();
        if (!file) return;
        const t = udTerminal();
        t.show(true);
        t.sendText(`ud build "${file}"`);
    });

    context.subscriptions.push(run, build);
}

function deactivate() {}

module.exports = { activate, deactivate };
