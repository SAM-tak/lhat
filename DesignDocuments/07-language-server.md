# 言語サーバとエディタ統合

`L^` の言語サーバ `lhatls`（`lsp/`）と、それを利用する VSCode 拡張（`vscode-extension/`）の設計。

ビジュアルエディタは同じ拡張の中に載るが、設計は [06-visual-editor.md](06-visual-editor.md) に分けてある。
本文書はその土台にあたる部分を扱う。

節番号のない参照（`14.17` など）は [02-syntax.md](02-syntax.md) を指す。

## 1. 位置づけ

処理系本体の仕様ではなく、処理系を利用するツールの仕様。言語の意味論には影響しない。

ただし本文書が要求する情報のうち、字句解析器や構文木が持っていないものがある。
それらは本体側の設計文書に未決事項として起票し、そちらで決める。
現時点では [01-lexical-structure.md](01-lexical-structure.md) の Q13（コメントの保持）がそれにあたる。

## 2. 構成

```text
  VSCode
   └ vscode-extension (拡張、TypeScript)
      ├ 言語クライアント (vscode-languageclient) ──► lhatls (子プロセス、stdio)
      ├ 構文強調の文法定義 (syntaxes/lhat.tmLanguage.json)
      └ ビジュアルエディタ (webview)             ──► lhatls (同じ接続)
```

言語サーバは `L^` の実装（`src/`）をそのまま組み込む。字句解析・構文解析・型検査を
サーバ側で二重に実装しない。ビジュアルエディタも同じ接続を使う（06 の 2 章）。

## 3. 実装済みの機能

| 機能 | 方法 | 実装 |
| --- | --- | --- |
| 診断 | `textDocument/publishDiagnostics` | `lsp/diagnostics.c`、`lsp/worker.c` |
| 意味的な構文強調 | `textDocument/semanticTokens/full` | `lsp/handlers/semantic_tokens.c` |
| 文書の同期 | `textDocument/didOpen` / `didChange` / `didClose` | `lsp/handlers/text_document_sync.c` |

検査はワーカースレッドで動く。ワークスペース内の `*.lh` はそれぞれが独立した根であり、
1つの編集は「その根」と「最後の検査でその文書を通過した根」だけを検査し直す
（`lsp/workspace.h`）。

新しい要求への対応は `lsp/dispatch_table.c` に1行と `lsp/handlers/*.c` を1つ足すだけでよく、
`dispatch.c` と `server.c` には手を入れない。

## 4. ホバー［提案］

定義済みの関数などにホバーしたとき、その説明を表示する（`textDocument/hover`）。
表示する内容は次の3つ。

1. **綴り** — 名前と、推論または注釈された型。型は検査器が既に持っている
2. **説明文** — その定義に結び付いたコメント
3. **定義元** — 別の単位から来たものであれば、どのモジュールのものか（05 の 8.7）

2 は字句解析器がコメントを保持していないと作れない。01 の 6.4 がそれを決めており、
**定義の直前に置かれたコメント塊**が説明文である。専用の記法はない。

ビジュアルエディタもノードのコメントとして同じものを使う（06 の 5.4）。
説明文を組み立てる処理は両方から呼べる場所に置き、ホバー専用にしない。

言語サーバは `LHAT_WITH_COMMENTS` を定義したビルドを使う（01 の 6.4）。
これが外れた構成では 2 が空になる。1 と 3 だけでも表示は成り立つ。

［未決］ **L1** — 説明文をどの範囲まで拾うか。
定義に結び付いたものだけか、`module^` に結び付いたものをモジュールの説明として扱うか。

［未決］ **L5** — 説明文の中で返り値や引数への説明を書き分ける記法（`@Return` など）。
01 の 6.4 が後回しとしたもので、決めるときは 01 側に番号を起票する。
それまで説明文は一塊として扱う。

## 5. これから足す標準の機能

優先度の高い順。いずれも構文木と検査結果から作れる。

| 機能 | 方法 | 備考 |
| --- | --- | --- |
| ホバー | `textDocument/hover` | 4 章 |
| 定義へ移動 | `textDocument/definition` | 検査器の名前解決が要る |
| 参照の検索 | `textDocument/references` | 単位をまたぐ場合は根の逆引き（`lsp/workspace.h`）を使う |
| 文書シンボル | `textDocument/documentSymbol` | 構文木から直接作れる |
| 補完 | `textDocument/completion` | 途中まで書かれた不完全な木を扱う必要がある |
| 名前の変更 | `textDocument/rename` | 参照の検索の後 |

［未決］ **L2** — 補完のために、構文解析器が誤りを含む入力からどこまで木を作るか。
現在の構文解析器の誤り回復がどの水準にあるかを確かめていない。

## 6. 独自拡張

| 要求 | 用途 | 定義 | 実装 |
| --- | --- | --- | --- |
| `lhat/ast` | ビジュアルエディタに構文木を渡す | 06 の 4 章 | `lsp/ast_json.c`、`lsp/handlers/ast.c` |

独自拡張は `lhat/` を接頭辞とする。

直列化と受け口を分けるのは `semantic_tokens` と同じ形。
直列化の側にはノード種ごとの場合分けが1つもない
（`lhat_node_visit_children` が子とその名前を渡す）。

## 7. VSCode 拡張

現状は言語クライアントと構文強調の文法定義のみ（`vscode-extension/package.json` の
`contributes` は `languages` / `grammars` / `configuration` / `configurationDefaults`）。

ビジュアルエディタを載せるにあたって足すもの。

- **カスタムエディタ** — `*.lh` をグラフとして開く。`contributes.customEditors` に
  `CustomTextEditorProvider` として登録する。テキストエディタと排他にはせず、
  同じ文書を両方で開けるようにする
- **webview** — React Flow と ELK.js を含む。外部への通信は行わない

［未決］ **L3** — webview と言語サーバの経路。
拡張本体が言語クライアント経由で `lhat/ast` を呼び、結果を webview に転送する形が素直だが、
文書の変更のたびに拡張本体を経由する往復が増える。

［未決］ **L4** — カスタムエディタを既定にするか。既定にすると `*.lh` を開いたときに
いきなりグラフが出る。第1段階（06 の 3 章）は読み取り専用なので、既定にはしない。

## 8. 未決事項一覧

| 番号 | 内容 |
| --- | --- |
| L1 | 説明文をどの範囲まで拾うか |
| L2 | 補完に必要な誤り回復の水準 |
| L3 | webview と言語サーバの経路 |
| L4 | カスタムエディタを既定にするか |
| L5 | 説明文の中の書き分け記法（`@Return` など）。当分後回し |
