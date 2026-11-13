# モジュール

## 翻訳単位と公開

`module^名前` はファイル先頭で、その翻訳単位の名前を宣言します。1ファイルに1回までです。
名前は `game.player` のように `.` で区切れ、ファイルパスとは独立しています。
モジュール宣言のないスクリプトも読み込めます。

宣言は既定で非公開です。`public^let^`、`public^errordef^`、`public^enum^` など、
単位の最上位で公開したシンボルがモジュールのメンバになります。
`public^var^` は使えません。公開は束縛の再代入を禁止しますが、テーブルの深い不変性を保証するものではありません。
公開用テーブルを末尾の `return^` で組み立てる必要はありません。

## require^

| 形 | 取り込み先 |
| --- | --- |
| `let^m = require^"path.lh"` | 取り込み側が選んだ名前 `m` |
| `require^"path.lh"` | 読み込まれた単位の `module^` 名 |

パスは文字列リテラルで、通常は要求元のファイルからの相対パスです。
`"$src/file.lh"` はホストが設定するプロジェクトルートからの相対パスです。
`$` は環境変数の展開ではありません。
束縛しない省略形には、読み込まれる側の単位に `module^` 宣言が必要です。

`require^` は公開メンバを持つテーブルを返します。同じ単位の初期化は一度だけで、同じテーブルを返します。
型検査は依存先を静的に辿り、循環する依存はエラーです。
初期化を実行するため、関数 `f^` の本体には書けません。
実行時に決まるソースの読み込みは、ホストが提供する `std.load` などの API の役割です。

## import^ とホストのバインディング

`import^` は、ホストの言語で実装され、L^ に登録・公開されたモジュールを取り込みます。
自作アプリケーションの API と、`std.math` などの標準ライブラリは同じ仕組みを使います。

| 形 | 参照方法 |
| --- | --- |
| `import^app` | `app.report(...)` など |
| `let^host = import^app` | `host.report(...)` など |
| `let^m = import^std.math` | `m.sqrt(...)` など |

使える名前と型はホストが登録した内容で決まります。登録されたシグネチャを型検査にも使います。
`import^` 自体は L^ の単位の初期化を実行しません。
子モジュールは使う単位を明示して取り込みます。
`print` などの初期束縛もホストの設定であり、言語がすべての実行環境へ暗黙に提供するものではありません。

具体的な使用例は[インポートとモジュール](../guide/modules.md)、
バインディングの登録方法は[ホスト API と型を公開する](../embedding/host-api.md)を参照してください。

参照元：[モジュール](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/05-modules.md)
（[modules.unit]、[modules.public]、[modules.require]、[modules.deps]、[modules.host.initial]、[modules.host.import]）、
[構文](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)（[syntax.enum.declare]）。
