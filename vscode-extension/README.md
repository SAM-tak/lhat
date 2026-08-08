# L^ (lhat) Language Support

`lhatls`（言語サーバー本体。ソースは `lsp/`、CMake ターゲット名は `lhat_lsp`）を
起動し、VSCode に L^ の型検査の診断（赤波線）とセマンティックハイライトを届ける
薄いクライアント。ホバー・補完・定義ジャンプは今回のスコープ外。

## セットアップ

1. サーバー本体（リポジトリルート）をビルド:

   ```powershell
   . .\scripts\devshell.ps1
   cmake --preset debug
   cmake --build --preset debug --target lhat_lsp
   ```

   `build\debug\lhatls.exe` ができる。

2. この拡張の依存関係を入れてコンパイル:

   ```powershell
   cd vscode-extension
   npm install
   npm run compile
   ```

3. VSCode で **`vscode-extension` フォルダそのものを** 開く
   （`File > Open Folder`）。リポジトリルートを開いたままだと
   `.vscode/launch.json` が認識されず、F5 は今開いているファイルを
   素で（Plain Text として）デバッグしようとしてしまう。
   `vscode-extension` を開いた状態で F5 → 拡張開発ホストが起動する。
   `lhatls.exe` が PATH 上になければ、設定 `lhat.serverPath` に
   `build\debug\lhatls.exe` への絶対パスを指定する。

4. `.lh` ファイルを開くと、保存前の編集内容がそのまま型検査され、
   `require^` で参照する同じワークスペース内の他ファイルも辿って検査される。

## シンタックスハイライト

`syntaxes/lhat.tmLanguage.json` が静的な色分けを提供する（LSP とは無関係、
拡張だけで完結）。01-lexical-structure.md 2章のとおり L^ に予約語は無く、
すべてのキーワードは `^` 付きの語（`let^`・`if^`・`number^` など）として
字句的には一律 `HAT_IDENT` になる。ハイライト上は意味カテゴリ（制御構文・
宣言・型・定数など）で色分けしているが、これは構文解析の知識を借りた
便宜的な分類であり、言語仕様そのものが持つ区別ではない。

キーワードを担うのは語の部分で、末尾の `^` 自体は記号でしかないため、
`punctuation.definition.hat.lhat` という専用スコープに分離し、
拡張の既定設定（`configurationDefaults`）で半透明グレー
（`#88888899`）を当てている。テーマごとに色を出し分けるのではなく、
エディタの背景色（ダーク/ライト）に応じて自動的に馴染むようにする狙い。
この既定はユーザーが自分で `editor.tokenColorCustomizations` を
設定していれば上書きされない。

## セマンティックハイライト

`textDocument/semanticTokens/full` を実装済み。構文木（`lsp/semantic_tokens.c`
がノードの文脈だけを見て歩く。型検査は通さない）から、宣言と参照、パラメータ、
関数呼び出しの対象、型名、モジュールパスを区別して塗る。TextMate は正規表現しか
見えないためこの区別ができず、意味カテゴリ（キーワード等）の色分けを担う土台
として今も残る。両者は VSCode の「TextMate が下地、セマンティックトークンが
上書き」という標準の2層構造で共存する。

## 既知の制約

- ホバー・補完・定義ジャンプは無い。
- ホストが `lhat_register_func` 等で登録する独自 API は、このサーバーは知らない
  （`print`/`collectgarbage` のみ登録済み）。独自 API を使うスクリプトは
  「no such name in scope」の偽陽性が出る。
- ワークスペース内の `*.lh` を毎回それぞれ独立したルートとして検査するため、
  共有される依存ファイルが多いほど検査コストが増える（`lsp/workspace.c` 参照）。
