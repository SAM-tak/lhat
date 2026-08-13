# L^ (lhat) Language Support

`lhatls`（言語サーバー本体。ソースは `lsp/`、CMake ターゲット名は `lhat_lsp`）を
起動し、VSCode に L^ の型検査の診断（赤波線）・セマンティックハイライト・
ホバーを届ける薄いクライアント。加えて L^ のグラフ表示を持つ。
補完・定義ジャンプは今回のスコープ外。

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

## 普段使いの VSCode に入れる

拡張開発ホスト（F5）は起動するたびに別ウィンドウが立ち上がる。
そうではなく普段開いている VSCode 自体に入れたいときは、`.vsix` に固めて
インストールする。

```powershell
cd vscode-extension
npm run install-extension
```

`npm run package`（`vsce package`。`vscode:prepublish` 経由で `compile` も走る）
で `lhat-lsp-client.vsix` を作り、`code --install-extension … --force` で
入れ直すところまでを一度にやる。拡張を更新したいときも同じコマンドを叩けばよく、
`--force` があるのでバージョンを上げなくても上書きされる。
反映には **`Developer: Reload Window`**（ウィンドウの再読込）が要る。

外すときは `npm run uninstall-extension`。

`lhatls.exe` が PATH 上に無ければ、ここでも設定 `lhat.serverPath` に
絶対パスを与える（`settings.json`）:

```json
"lhat.serverPath": "C:\\path\\to\\lhat\\build\\release\\lhatls.exe"
```

なお `vsce` は `.vsix` の中に LICENSE が入っていることを求めるため、
リポジトリルートの `LICENSE`（Apache-2.0）をこのフォルダにも置いてある。
どちらを直すときも両方を揃える。

## ホバー

名前にマウスを乗せると、その名前が届いた定義が出る（07 の 4 章）。

- 定義を導入した行（本体が長くても1行目だけ）
- その定義に書かれたコメント（`#` や `#[ ]#` は外して表示）

加えて検査器が推論した型が出る（`f^number^ -> string^;` のような 13 章の記法）。

`module^` にホバーすると、その単位そのものの説明が出る。
別の単位から来た名前は `require^` や `import^` の行が出るので、モジュール名も見える。
メンバーへのアクセス（`io.print` の `print`）にはまだ答えない（07 の L8）。

## グラフ表示

`.lh` を開いた状態で **`L^: Open Graph View`**（コマンドパレット、または
エディタ右上のボタン）を実行すると、グラフが開く。
DesignDocuments/06-visual-editor.md の写像を実装したもので、今のところ
**閲覧専用**。グラフ上の箱をクリックすると、テキスト側の対応箇所が選択される。

既定では同じエディタグループの別タブに開く。横に並べたい場合は設定
`lhat.graph.openBeside` を有効にする。分割するとグラフの使える幅が半分になり、
06 の 8.1 の実測では最初に足りなくなるのが幅であるため、既定は分割しない。

- 定義は既定で畳まれた状態で開く（06 の 6.5）。`fold / unfold` ボタンで切り替わる
- **畳まれた定義をクリックするとその中に入る**（06 の 8.2）。上部に来た道が出て、
  そこから戻る。入った先で開くのはその定義の本体までで、さらに内側の定義はまた畳まれる
- **コンテナを左ドラッグすると中身がずれる**（06 の 8.3）。順序に意味のある向き
  （文なら上下、分岐の節なら左右）ではなく、その直交方向にだけ動く。
  枠からはみ出した分は切り取られる
- 右下の地図で全体のどこを見ているかが分かり、掴んで移動できる（06 の 8.4）
- リーフにマウスを乗せると左右にハンドルが出る。データの流れの線（06 の 5.5）の
  試作で、まだ保存されない
- テキストを編集すると `lhat/ast` を引き直して描き直す
- 「waiting for the language server…」が出たままのときは、その単位がまだ
  検査に入っていない（06 の 4.3）。診断が出れば描かれる

操作は次のとおり。配置が常に自動で決まるため、**キャンバスを掴んで動かす操作は無い**。

| 操作 | 意味 |
| --- | --- |
| ホイール | 上下に動く |
| Shift + ホイール | 左右に動く |
| Ctrl + ホイール（macOS は Command） | 拡大・縮小 |
| コンテナを左ドラッグ | その中身をずらす |
| 全体地図を掴む | そこへ移動する |

グラフは `lhat/ast`（06 の 4 章）で構文木を受け取り、webview 側で
配置規則を当てて ELK.js に渡す。webview は LSP を直接話せないので、
拡張本体が要求を代行する（07 の L3）。

描画は **React Flow**（06 の 8.4）。配置は ELK が決めたものをそのまま使い、
React Flow は見せる側に徹する。この半分だけ esbuild が束ねる（07 の 7.1）。

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

## ホスト API を教える — lhat-host.json

ホストが `lhat_register_func` 等で C から登録する API（このリポジトリなら
サンプル標準ライブラリの `std.io`・`std.thread` 等）は、そのままでは
このサーバーから見えない。`import^std.io` が「no module of this name」に
なるのはこのため。

登録内容をテキストに落とした **`lhat-host.json`** をワークスペースの
ルート直下に置くと、サーバーが起動時に読み込んで同じ登録を再現する
（コールバックの実体は持たないが、検査しか行わないので困らない）。
手書きはせず、CLI に吐かせる:

```powershell
.\build\debug\lhat.exe --dump-host-api lhat-host.json
```

CLI が実際に登録しているもの（stdlib 込み）がそのまま出る。独自の組み込み
ホストなら、自分の登録を済ませた `LhatProgram` に対して
`lhat_program_dump_host_api`（`include/lhat/program.h`）を呼べば同じ形式で
書き出せる。

ファイルはエディタで編集するとその場で反映される（保存前の内容で
再検査が走る）。無ければ従来どおり `print`/`collectgarbage` の最小登録に
フォールバックする。

## 既知の制約

- 補完・定義ジャンプは無い（ホバーは実装済み）。
- ワークスペース内の `*.lh` を毎回それぞれ独立したルートとして検査するため、
  共有される依存ファイルが多いほど検査コストが増える（`lsp/workspace.c` 参照）。
