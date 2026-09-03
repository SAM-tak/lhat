# 言語サーバとエディタ統合

`L^` の言語サーバ `lhatls`（`lsp/`）と、それを利用する VSCode 拡張（`vscode-extension/`）の設計。

ビジュアルエディタは同じ拡張の中に載るが、設計は [06-visual-editor.md](06-visual-editor.md) に分けてある。
本文書はその土台にあたる部分を扱う。

節番号のない参照（`14.17` など）は [02-syntax.md](02-syntax.md) を指す。

## 1. 位置づけ

処理系本体の仕様ではなく、処理系を利用するツールの仕様。言語の意味論には影響しない。

ただし本文書が要求する情報のうち、字句解析器や構文木が持っていないものがある。
それらは本体側の設計文書に未決事項として起票し、そちらで決める。

## 2. 構成

```text
  VSCode
   └ vscode-extension (拡張、TypeScript)
      ├ 言語クライアント (vscode-languageclient) ──► lhatls (子プロセス、stdio)
      ├ 構文強調の文法定義 (syntaxes/lhat.tmLanguage.json)
      ├ ビジュアルエディタ (webview)             ──► lhatls (同じ接続)
      └ デバッグクライアント (DAP)               ──► lhat --dap=PORT (別プロセス、TCP)
```

言語サーバは `L^` の実装（`src/`）をそのまま組み込む。字句解析・構文解析・型検査を
サーバ側で二重に実装しない。ビジュアルエディタも同じ接続を使う（06 の 2 章）。
デバッグは別で、`lhat --dap` が担う（[09-debugger.md](09-debugger.md)）——`lhatls` とは別の
プロセスで、枠（Content-Length）だけを共有する。

## 3. 実装済みの機能

| 機能 | 方法 | 実装 |
| --- | --- | --- |
| 診断 | `textDocument/publishDiagnostics` | `lsp/diagnostics.c`、`lsp/worker.c` |
| 意味的な構文強調 | `textDocument/semanticTokens/full` | `lsp/handlers/semantic_tokens.c` |
| 文書の同期 | `textDocument/didOpen` / `didChange` / `didClose` | `lsp/handlers/text_document_sync.c` |
| ホバー | `textDocument/hover` | 4 章。`lsp/hover.c` |
| 定義へ移動 | `textDocument/definition` | 4 章の `LhatResolution` がそのまま答える。`lsp/definition.c` |
| 文書シンボル | `textDocument/documentSymbol` | 構文木だけから作る。`lsp/document_symbol.c` |

検査はワーカースレッドで動く。ワークスペース内の `*.lh` はそれぞれが独立した根であり、
1つの編集は「その根」と「最後の検査でその文書を通過した根」だけを検査し直す
（`lsp/workspace.h`）。

新しい要求への対応は `lsp/dispatch_table.c` に1行と `lsp/handlers/*.c` を1つ足すだけでよく、
`dispatch.c` と `server.c` には手を入れない。

**文書シンボル（アウトライン）は検査を待たない。**列挙するのは `let^` / `var^` の束縛、
`def^` のメンバー（`self^{ }` 欄の項目はフィールド、関数値はメソッド）、テーブルリテラルの
名前付き項目、`errordef^` とその種類、`module^`、そして単位が `return^` で答えるテーブルの
項目 — LTON はこの最後の形（08 章）。いずれも名前は構文木に書かれていて、検査器の答えを
要しない。エディタはファイルを開いた瞬間にアウトラインを求めるので、受け口
（`lsp/handlers/document_symbol.c`）はワークスペースの検査済み単位を引かず、
文書ストアが持つその時点のテキストをその場で構文解析して答える。
ループの焦点・引数に渡したコールバックの中身は載せない。アウトラインはファイルの形であって、
名前の全列挙ではない。

## 4. ホバー

名前にホバーすると、**その名前が届いた定義**を示す（`textDocument/hover`、
`lsp/hover.c`）。示すのは次の3つ。

1. **定義の1行目** — その名前を導入した構文を、最初の改行まで。
   本体が何ページあっても1行に収まる
2. **推論された型** — 検査器が落ち着いた型（`lhat_type_write`）
3. **説明文** — その定義に結び付いたコメント（01 の 6.4）。`#` や `#[ ]#` は外す

```text
let^ twice = f^n:number^ -> number^ {
: f^number^ -> number^;

二倍にする
```

1 と 2 は**両方出す**。書かれたものと、そこから導かれたものは別の話であり、
注釈のない定義には 2 しかなく、注釈のある定義は書かれた形も見たい。

型は 13 章の記法で書き出す（`lhat_type_write`、`src/type.c`）。
`number^`、`t^{ sku : string^ }`、`f^number^ -> string^;` のように読める。
表示のための道具であって、往復できる表現ではない。

14 章はテーブルが自分自身を含むことを許すので、**深さと要素数で打ち切る**
（`LHAT_TYPE_WRITE_MAX_DEPTH`、`LHAT_TYPE_WRITE_MAX_ITEMS`）。
打ち切りも、バッファに収まらない場合も、末尾を `…` にする。

### 名前解決は検査器の答えを読む

どの定義に届いたかは、**検査器が解決したときに控えたものを読む**
（`check.h` の `LhatResolution` と `lhat_check_resolution_at`）。
`scope_find` が答えを出すその場所で、使用位置・定義位置・型を記録する。

8 章のスコープ規則をホバー側で解き直す道は採らない。
実装が二つになれば歩調を合わせ続ける必要が生じ、しかも**規則が難しい場所でこそ食い違う**。

記録は使用位置の順に並ぶので、位置からの検索は二分探索で済む。
この表は定義へ移動・参照の検索（5 章）の土台でもある。

### 定義の文を見つける

`LhatResolution` が持つのは定義された**名前の位置**である。
そこから、その位置を含む最も内側の「名前を導入する構文」（`DEFINE`、`PARAM`、
`ERRORDEF`、`TABLE_ENTRY` など）を木から探し、その範囲の1行目を切り出す。

**`module^` に結び付いたコメントは、その単位そのものの説明**である。
`module^` は名前を宣言するもので使いはしないため解決の記録に載らない。
位置から直に探して答える。

**別の単位から来た名前は、モジュール名が見える。**追加の仕組みは要らない。
05 章では他の単位の名前に触れる道が `require^` と `import^` しかなく、
束縛の定義行に必ずモジュール名が現れるためである。

```text
let^ other = require^ "lib/util.lh"
```

**定義が別の単位にあるとき、定義の行と説明文はその単位から切る。** 記録の
`definition_path` が名指す単位を二段目に引く（定義へ移動と同じ形、
`lsp/handlers/hover.c`）。型と範囲は元の単位で決め（`lsp/hover.h` の `LspHoverPart`）、
ロックを越えるのは文字列とオフセットだけ。相手の単位に届かなければ型だけを出す。

**記録が付くのは名前だけではない。** `require^ "lib.lh"` の文字列はその単位の先頭（0）を、
型の位置に書いた `store.Held` は輸出したメンバーの宣言を、`E.Bad` / `Mode.Walk` は種の宣言を
指す。種は型（`type.h` の `v.error`）に書かれた場所を持つ — メンバーの `declared_at` と同じ。
先頭がコメントで始まる単位の 0 には何も立たないので、hover は最初の文（`module^`）を示す。

［未決］ **L5** — 説明文の中で返り値や引数への説明を書き分ける記法（`@Return` など）。
01 の 6.4 が後回しとしたもので、決めるときは 01 側に番号を起票する。
それまで説明文は一塊として扱う。

なお言語サーバは `LHAT_WITH_COMMENTS` を定義したビルドを使う（01 の 6.4）。
外れた構成では 2 が空になり、1 だけが出る。
ビジュアルエディタもノードのコメントとして同じものを使う（06 の 5.4）。

## 5. これから足す標準の機能

優先度の高い順。いずれも構文木と検査結果から作れる。

| 機能 | 方法 | 備考 |
| --- | --- | --- |
| 参照の検索 | `textDocument/references` | 同じ表を定義位置で引く。単位をまたぐ場合は根の逆引き（`lsp/workspace.h`） |
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

`contributes` は `languages` / `grammars` / `configuration` / `configurationDefaults` に加え、
ビジュアルエディタのために次を持つ。

- **カスタムエディタ** — `contributes.customEditors` に `CustomTextEditorProvider`
  として登録（`src/graphEditor.ts`）。文書はテキスト文書のままなので、
  保存・取り消し・未保存の印は VSCode が扱う
- **webview** — 写像（`src/webview/map.ts`）と描画（同 `render.ts`）。
  外部への通信は行わない

**webview は言語サーバに直接触らない。拡張本体が代行する。**
webview には言語クライアントも接続先も無いため、これは選択ではなく前提である。
webview → 拡張本体は `postMessage`、拡張本体 → `lhatls` は `sendRequest("lhat/ast", …)`。
往復は一段増えるが、6.5 の測定からしてレイアウトの方が桁違いに重く、問題にならない。

**カスタムエディタは既定にしない**（`"priority": "option"`）。
`*.lh` を開けばテキストエディタが出る。グラフは `L^: Open Graph View` で横に開く。
第1段階（06 の 3 章）が読み取り専用である以上、グラフだけが出る状態は行き止まりになる。

### 7.1 二つの半分と、その作り分け

拡張は性質の違う二つの半分でできている。

- **拡張本体**（`src/*.ts`）— Node の上で動き、`tsc` が CommonJS に出す
- **webview**（`src/webview/`）— ブラウザの上で動き、React Flow を使う（06 の 8.4）。
  JSX を含み、React とその依存ごと束ねる必要があるので **esbuild** が1つに固める

esbuild は型を見ないので、型検査は `tsconfig.rf.json`（`npm run check:rf`）が別に行う。
**束ねるものと検査するものを分けてある**ので、片方だけを走らせても意味がある。

写像（`src/webview/map.ts`）は描画層に依存しない。これは意図してそう書いたもので、
描画層を差し替えても写像は動いたことが 06 の 8.4 の検証で確かめられている。

## 8. 未決事項一覧

| 番号 | 内容 |
| --- | --- |
| L2 | 補完に必要な誤り回復の水準 |
| L5 | 説明文の中の書き分け記法（`@Return` など）。当分後回し |

## 改定履歴（要約）

- L1（`module^` のコメントは単位の説明）・L3（webview は拡張本体経由）・
  L4（カスタムエディタは既定にしない）・L6（定義行と推論型を両方出す）・
  L7（モジュール名は定義行に出る）・L8（メンバーも解決の記録に載せ、ホバーと
  定義へ移動が答える。型が書かれた場所を持つ）は決定済み。本文に取り込み欠番
