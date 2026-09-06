// L^ (lhat) -- the document outline, for when the graph is what is open.
//
// VSCode's own Outline view asks the active editor for symbols, and a custom
// editor cannot answer: it is not a text editor, and there is no API to hand
// the view a tree from one (microsoft/vscode#101476, open). So the graph tab
// would lose the outline the text tab has -- as VSCode's own Markdown preview
// does, which is how strong the limit is.
//
// This goes away if microsoft/vscode#304909 lands: it proposes
// registerCustomEditorOutlineProvider, which fills the real Outline,
// Breadcrumbs and Go to Symbol from a custom editor. It is a *proposed* API
// behind the "customEditorOutline" flag, so it is unusable in a published
// extension until it stabilises -- and its revealItem() is where the box
// hunt below would move to.
//
// This is the same tree in a view of our own, shown in the Explorer beside
// the real Outline and only while a graph is up (the `when` clause in
// package.json), so the two never appear at once. The symbols come from the
// same provider the real one uses -- lhatls's textDocument/documentSymbol,
// reached through VSCode rather than through our language client, so this
// works with any provider registered for the file.

import * as vscode from "vscode";

/** The file a graph tab is showing, or undefined if that is not what is up. */
export function graphedUri(): vscode.Uri | undefined {
    const tab = vscode.window.tabGroups.activeTabGroup.activeTab;
    const input: unknown = tab?.input;
    if (!(input instanceof vscode.TabInputCustom)) return undefined;
    return input.viewType === "lhat.graph" ? input.uri : undefined;
}

export class LhatOutlineProvider
implements vscode.TreeDataProvider<vscode.DocumentSymbol> {
    private readonly changed =
        new vscode.EventEmitter<vscode.DocumentSymbol | undefined>();
    public readonly onDidChangeTreeData = this.changed.event;

    /** What the tree was last built from, for the click to resolve against. */
    private uri: vscode.Uri | undefined;

    public refresh(): void {
        this.changed.fire(undefined);
    }

    public getTreeItem(symbol: vscode.DocumentSymbol): vscode.TreeItem {
        const item = new vscode.TreeItem(
            symbol.name,
            symbol.children.length > 0
                ? vscode.TreeItemCollapsibleState.Expanded
                : vscode.TreeItemCollapsibleState.None);
        item.description = symbol.detail;
        // The codicon names for symbols are "symbol-" plus the kind, which is
        // exactly what the enum's own names give once lowercased.
        item.iconPath = new vscode.ThemeIcon(
            `symbol-${vscode.SymbolKind[symbol.kind].toLowerCase()}`);
        item.tooltip = symbol.detail !== "" ? symbol.detail : symbol.name;
        if (this.uri !== undefined) {
            item.command = {
                command: "lhat.outlineReveal",
                title: "Show in the graph",
                // selectionRange is the name alone; range is the whole
                // construct, which is what the graph has a box for.
                arguments: [this.uri, symbol.range],
            };
        }
        return item;
    }

    public async getChildren(
        symbol?: vscode.DocumentSymbol,
    ): Promise<vscode.DocumentSymbol[]> {
        if (symbol !== undefined) return symbol.children;
        const uri = graphedUri();
        this.uri = uri;
        if (uri === undefined) return [];
        // The command runs whatever DocumentSymbolProvider is registered for
        // the file -- lhatls, through the language client -- so this needs no
        // knowledge of the server beyond it being there.
        const symbols = await vscode.commands.executeCommand<
            vscode.DocumentSymbol[] | undefined>(
            "vscode.executeDocumentSymbolProvider", uri);
        return symbols ?? [];
    }
}
