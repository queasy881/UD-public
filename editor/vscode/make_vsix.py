#!/usr/bin/env python3
"""Package the UD VS Code extension into a .vsix -- pure stdlib, no vsce/npm.

A .vsix is just a ZIP with a specific layout:

    [Content_Types].xml          which file extensions map to which MIME types
    extension.vsixmanifest       the marketplace/CLI manifest (XML)
    extension/                    every file of the actual extension

`code --install-extension ud-lang-<version>.vsix --force` reads that manifest,
copies the payload into VS Code's extensions folder, and -- crucially -- writes
the entry into VS Code's extensions.json cache. That cache step is the thing a
plain folder-copy skips, which is why a copied folder never lights up.

All metadata is pulled from package.json so the manifest can never drift.
Run it from anywhere:  python make_vsix.py [output-dir]
"""
import json, os, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

# files/dirs that never belong inside the package
EXCLUDE_NAMES = {"make_vsix.py", ".vscodeignore", ".gitignore", "node_modules",
                 "__pycache__", ".DS_Store"}


def _xml_escape(text):
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
                .replace('"', "&quot;"))


def _manifest(pkg):
    ident = pkg.get("name", "extension")
    publisher = pkg.get("publisher", "unknown")
    version = pkg.get("version", "0.0.0")
    display = _xml_escape(pkg.get("displayName", ident))
    desc = _xml_escape(pkg.get("description", ""))
    engine = _xml_escape(pkg.get("engines", {}).get("vscode", "*"))
    cats = pkg.get("categories", []) or ["Other"]
    categories = _xml_escape(",".join(cats))
    icon = pkg.get("icon")
    icon_meta = "\n    <Icon>extension/%s</Icon>" % icon if icon else ""
    icon_asset = ('\n    <Asset Type="Microsoft.VisualStudio.Services.Icons.Default" '
                  'Path="extension/%s" Addressable="true" />' % icon) if icon else ""
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<PackageManifest Version="2.0.0" '
        'xmlns="http://schemas.microsoft.com/developer/vsx-schema/2011" '
        'xmlns:d="http://schemas.microsoft.com/developer/vsx-schema-design/2011">\n'
        '  <Metadata>\n'
        '    <Identity Language="en-US" Id="%s" Version="%s" Publisher="%s" />\n'
        '    <DisplayName>%s</DisplayName>\n'
        '    <Description xml:space="preserve">%s</Description>\n'
        '    <Tags>%s</Tags>\n'
        '    <Categories>%s</Categories>\n'
        '    <GalleryFlags>Public</GalleryFlags>\n'
        '    <Properties>\n'
        '      <Property Id="Microsoft.VisualStudio.Code.Engine" Value="%s" />\n'
        '      <Property Id="Microsoft.VisualStudio.Code.ExtensionDependencies" Value="" />\n'
        '      <Property Id="Microsoft.VisualStudio.Code.ExtensionPack" Value="" />\n'
        '    </Properties>%s\n'
        '  </Metadata>\n'
        '  <Installation>\n'
        '    <InstallationTarget Id="Microsoft.VisualStudio.Code" />\n'
        '  </Installation>\n'
        '  <Dependencies />\n'
        '  <Assets>\n'
        '    <Asset Type="Microsoft.VisualStudio.Code.Manifest" '
        'Path="extension/package.json" Addressable="true" />%s\n'
        '  </Assets>\n'
        '</PackageManifest>\n'
        % (ident, version, publisher, display, desc, ident, categories,
           engine, icon_meta, icon_asset)
    )


CONTENT_TYPES = (
    '<?xml version="1.0" encoding="utf-8"?>\n'
    '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">\n'
    '  <Default Extension="json" ContentType="application/json" />\n'
    '  <Default Extension="js" ContentType="application/javascript" />\n'
    '  <Default Extension="png" ContentType="image/png" />\n'
    '  <Default Extension="md" ContentType="text/markdown" />\n'
    '  <Default Extension="vsixmanifest" ContentType="text/xml" />\n'
    '</Types>\n'
)


def _payload_files():
    for root, dirs, files in os.walk(HERE):
        dirs[:] = [d for d in dirs if d not in EXCLUDE_NAMES]
        for name in files:
            if name in EXCLUDE_NAMES or name.endswith(".vsix"):
                continue
            full = os.path.join(root, name)
            rel = os.path.relpath(full, HERE).replace(os.sep, "/")
            yield full, rel


def build(out_dir=None):
    with open(os.path.join(HERE, "package.json"), encoding="utf-8") as f:
        pkg = json.load(f)
    out_dir = out_dir or HERE
    vsix = os.path.join(out_dir, "%s-%s.vsix" % (pkg.get("name", "extension"),
                                                 pkg.get("version", "0.0.0")))
    with zipfile.ZipFile(vsix, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CONTENT_TYPES)
        z.writestr("extension.vsixmanifest", _manifest(pkg))
        for full, rel in _payload_files():
            z.write(full, "extension/" + rel)
    print("wrote %s (%d bytes)" % (vsix, os.path.getsize(vsix)))
    return vsix


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else None)
