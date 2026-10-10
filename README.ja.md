# プログラミング言語 L^

![L^ Logo](media/lhat-logo.svg)

**Modern & Better Lua with Visual Programming.**

[English](README.md) | **日本語**

L^（elhat）は、ゲームやアプリケーションに組み込んで使うスクリプト言語です。
組み込みやすさ、柔軟なテーブル、コルーチンといった Lua の長所を受け継ぎながら、
型安全性と表現力を高め、ビジュアルプログラミングとの相互運用を前提に設計しています。

**テキストとビジュアルで、同じソースを扱えます。**
L^ のソースコードは、そのままビジュアルプログラムの保存形式でもあります。
開発の途中でテキストとビジュアルを行き来できるように、言語そのものから設計しています。

- **言語設計の中心にあるビジュアルプログラミング。** コードとグラフが共通のソースを使います。
  ビジュアルエディタは開発中で、VS Code でのグラフ表示はほぼ完成していますが、グラフ上での編集はまだ完成していません。
- **型注釈を抑えながら、型安全に。** 静的型検査と双方向型推論により、簡潔なコードでも間違いを早期に検出できます。
- **すでに使える開発環境とエンジン連携。** VS Code の言語サポート、Godot バインディング、LÖVE ベースのゲームフレームワークを用意しています。
- **アプリケーションのロジックを素直に書ける機能。** 関数型と手続き型の併用、構造的型付けによるオブジェクト指向、型付きエラー、パターンマッチ、並行タスクに対応しています。
- **自分のプロジェクトにも組み込みやすい処理系。** C11 でコンパイルでき、コアの依存は C 標準ライブラリと数学ライブラリ（`libc`・`libm`）だけです。C API からの組み込みや独自のホスト型の登録にも対応しています。

**バージョン:** 0.4.4 · **開発状況:** pre-1.0、開発中 · **ライセンス:** Apache 2.0

[ビジュアルプログラミング](#テキストとビジュアルプログラミング) ·
[開発環境と連携](#開発環境と連携) ·
[言語の特徴](#言語の特徴) ·
[使い始める](#使い始める) ·
[アプリケーションへの組み込み](#アプリケーションへの組み込み)

## テキストとビジュアルプログラミング

ビジュアルプログラミングは、L^ の中心的な設計目標です。
`.lh` ファイルをテキストエディタとビジュアルエディタで共有することで、
ビジュアルプログラムにも通常のソースコードと同じバージョン管理、差分表示、
コードレビュー、コンパイル、型検査を利用できます。

言語とツールは、コードをグラフとして表現するための構造やコメントを保持します。

> [!NOTE]
> ビジュアルエディタは開発中です。VS Code 拡張でのグラフ表示はほぼ完成していますが、
> グラフ上での編集はまだ完成していません。開発は[拡張のリポジトリ](https://github.com/SAM-tak/lhat-vscode-extension)で進めています。

たとえば、[次のプログラム](sample/factorial.lh)は、無名関数で 10 の階乗を求めます。
**`this^` は、その関数自身を指します。** `factorial` のような名前を付けなくても、
無名のまま自分自身を再帰的に呼び出せます。

![L^ で書いた階乗のプログラム](media/readme-factorial.svg)

同じプログラムを、グラフとして表示することもできます。

![階乗のプログラムのグラフ表示](media/factorial-graph.svg)

## 開発環境と連携

### VS Code

[L^ Language Support](https://marketplace.visualstudio.com/items?itemName=SAMtak.lhat)
をインストールすると、編集中の診断、コード補完、ホバー表示、定義への移動、
シグネチャヘルプ、意味に基づくハイライトを利用できます。
グラフ表示に加え、ブレークポイント、ステップ実行、変数の確認、式の評価などのデバッグ機能も備えています。

Marketplace のプラットフォーム別パッケージには、言語サーバ `lhatls` が同梱されています。
プログラムの実行やデバッグには、[Releases](https://github.com/SAM-tak/lhat/releases)から
`lhat` を入手するか、後述の手順でビルドしてください。
設定方法は[拡張のドキュメント](https://github.com/SAM-tak/lhat-vscode-extension)を参照してください。

### ゲームエンジン・フレームワーク

| 連携先 | できること |
| --- | --- |
| [Godot](https://github.com/SAM-tak/lhat-gdextension) | GDExtension で L^ を Godot のスクリプト言語として登録し、ノードのスクリプトやエディタツールを記述できます。 |
| [LÖVE / LÔVE](https://github.com/SAM-tak/lhat-love) | LÖVE ベースのフレームワーク LÔVE で、L^ を使った 2D ゲームを開発できます。 |
| [Unreal Engine](https://github.com/SAM-tak/lhat-UE) | 初期段階の実験的な連携です。 |

導入方法と開発状況は、それぞれのリポジトリで案内しています。

## 言語の特徴

コード例の画像をクリックすると、元のソースファイルを開けます。

### 日常的な処理を書きやすい構文

Lua の便利なテーブルと手軽なスクリプティングを受け継ぎつつ、
**0 ベースの添字**、**波括弧によるブロック**、`+=` や `*=` などの**複合代入**を採用しています。
添字はホスト側の API や一般的な配列の慣習と合わせやすく、
制御構文は C 系の言語に慣れた人にも読みやすい形です。
Luau と同じく、複合代入によって日常的な更新処理を簡潔に書けます。

[![L^ の 0 ベースの添字、型推論、複合代入の例](media/readme-basics.svg)](sample/readme/basics.lh)

### 型安全性を前提に設計し、型注釈はできるだけ省略

L^ は、**完全な型安全性を設計の前提**にしています。
テーブルへのアクセス、オブジェクトの合成、関数の副作用、エラー、ホスト API の呼び出しまで、
型の規則を言語の意味と一体で定めています。
既定の strict モードでは、型が決まらない箇所や安全でない操作を実行前に報告します。

**双方向型推論**により、式から得られる型だけでなく、その式を使う場所で期待される型も推論に利用します。
局所変数、戻り値、多くの引数の型は推論できるため、型注釈の大部分を省略できます。
API の意図や満たしてほしい条件を明示したいところには、型を書くこともできます。

条件分岐による型の絞り込み、省略可能な値の扱い、添字の範囲を考慮したテーブルアクセスの検査にも対応しています。
推論結果はエディタの補完や診断にも使われるので、短く書いたコードでも開発支援を受けられます。

### 関数型と手続き型を自然に組み合わせる

L^ は、**関数**（`f^`）と**手続き**（`p^`）を区別します。
関数は値の計算や自分の局所的な状態の更新を行えますが、外部の状態を書き換えたり、
手続きを呼び出したりすることは型検査で禁止されます。
手続きは状態変更や入出力などを担当し、関数と手続きの両方を呼び出せます。

計算部分をテストしやすい関数として書き、ゲームやアプリケーションとの接続部分を手続きで書く、
という構成を自然に取れます。

[![関数で割引価格を計算し、手続きで表示する例](media/readme-functions.svg)](sample/readme/functions.lh)

関数の内部では、効率のために局所変数を更新することもできます。
呼び出し側にとって重要な、副作用の境界を検査する仕組みです。

### 値ごとの GC アロケーションが不要なユーザー定義の値型

ホストアプリケーションから、**8 バイトを超えるインラインの値型**を登録できます。
独自のフィールド、メソッド、演算子を持たせることも可能です。
値や演算途中の一時値は VM のスタックスロットに直接格納されるため、
値を一つ作るたびに GC 管理のオブジェクトを確保する必要がありません。

特定の組み込みベクトル表現に限定せず、ベクトル、クォータニオン、行列、
アプリケーション固有のレコードなど、さまざまなサイズの数値型を定義できます。
標準ライブラリにも、複素数、クォータニオン、2・3・4 次元ベクトルを用意しています。

[![L^ のインライン値型によるベクトル演算の例](media/readme-value-types.svg)](sample/readme/value-types.lh)

テーブルなどのヒープ上のコンテナに保存したい場合は、明示的にボックス化できます。
[値型のサンプル](sample/vector.lh)と[ホスト API の仕様](DesignDocuments/05-modules.md)で詳しく説明しています。

### 失敗を明示的に扱えるエラー処理

L^ は Zig 風のエラーハンドリングを備えています。
失敗する可能性のある操作は、成功時の値または型付きのエラーを返します。
`try^` で呼び出し元へ伝え、`catch^` で回復し、エラーの型によって対処を分けられます。
失敗する可能性のある戻り値を、そのまま捨ててしまうコードは型検査で指摘されます。

[![型付きのゼロ除算エラーを catch^ で処理する例](media/readme-errors.svg)](sample/readme/errors.lh)

エラーにはデータを持たせられるので、失敗の原因や対処に必要な情報を一緒に返せます。
メッセージの文字列を解析して、エラーの種類を判別する必要はありません。

### 列挙体とパターンマッチ

列挙体で状態や選択肢に名前を付けられます。
パターンマッチでは値や型で分岐でき、各分岐内では型が絞り込まれます。
選択肢が分かる場合には、網羅性も検査されます。

[![L^ の列挙体を網羅的にパターンマッチする例](media/readme-enums.svg)](sample/readme/enums.lh)

これらの機能は、エラー処理や、複数の型を受け取る処理の整理にも役立ちます。

### 構造と合成によるオブジェクト指向

メソッド、オブジェクトの定義、合成、委譲、演算子オーバーロードを言語組み込みで提供します。
**構造的型付け**により、必要なメンバを備えたオブジェクトはそのままインタフェースを満たします。
別々に開発した部品や、テスト用の代替オブジェクトを組み合わせやすい設計です。

`def^` でオブジェクトを定義し、`..` で定義を合成できます。
`delegate^` を使えば、転送用のメソッドを繰り返し書かずに、別のオブジェクトへ処理を委譲できます。
メンバの型やオーバーライドの整合性は、合成時に検査されます。
クラス階層を前提とせず、オブジェクト指向の設計を組み立てられます。

[合成のサンプル](sample/composition.lh)では、ストレージ、ログ、キャッシュを組み合わせ、
構造的に適合するテスト用オブジェクトで動作を検証しています。

### タスクとメッセージによる並行処理

`std.task` は、BEAM の軽量プロセスを思わせる、タスク単位の並行処理を提供します。
コルーチンをジョブとしてワーカー VM のプールへ渡し、タスクハンドルを通じて型付きの結果を受け取れます。
`std.channel` でワーカー間の値の受け渡しもできます。
OS スレッドを再利用するので、ジョブごとにスレッドを作る必要がありません。

実装はワーカープール方式で、各ワーカーは担当するジョブを完了まで実行します。
実行予算によって中断の機会を設けています。
コルーチン、`yield^`、`await^` は、協調的なスケジューリングやアプリケーションのイベントループとの連携にも使えます。

`std.task` の使用例は[ハノイの塔の並列実行](sample/hanoi3.lh)、
フレームループとの連携例は[L^ で書いたスケジューラ](sample/async.lh)を参照してください。

### 使う人の言語で読める診断メッセージ

コンパイラの診断、実行時のメッセージ、開発ツールは多言語対応しています。
英語と日本語のメッセージを用意しており、メッセージカタログで翻訳を追加できます。

```sh
lhat --messages messages/ja --language ja --check app.lh
```

表示言語はプログラムごとに選べます。
データ形式や数値表現は言語設定によらず一定なので、診断の表示言語を変えても、
アプリケーションのデータ保存や交換の方法は変わりません。

## 使い始める

[VS Code 拡張](https://marketplace.visualstudio.com/items?itemName=SAMtak.lhat)と
[Releases](https://github.com/SAM-tak/lhat/releases)のランタイムから始められます。
ソースからビルドする場合は、以下の手順を使ってください。

### ビルドに必要なもの

- C11 コンパイラ：MSVC、GCC、Clang など
- CMake 3.25 以降
- Ninja プリセットを使う場合は [Ninja](https://ninja-build.org/)。Windows では Visual Studio ジェネレータも利用できます。

### Windows：Ninja と MSVC

PowerShell で以下を実行します。最初のコマンドは、現在のシェルに MSVC のビルド環境を設定します。

```powershell
. .\scripts\devshell.ps1
cmake --preset debug
cmake --build --preset debug
.\build\debug\lhat.exe sample\factorial.lh
```

インタプリタをより速くしたい場合は、Visual Studio に同梱の Clang（「C++ Clang tools for Windows」コンポーネント。
`devshell.ps1` の後は `PATH` から使えます）でビルドしてください。
MSVC では書けないジャンプテーブルによる命令の振り分けが使えます。Windows 版の配布バイナリもこの方法でビルドしています。

```powershell
cmake --preset release -DCMAKE_C_COMPILER=clang-cl
cmake --build --preset release
```

Ninja プリセットは Visual Studio 2022 以降に対応しています。
Visual Studio 2026 のジェネレータを使う場合は、次のようにビルドできます。

```powershell
cmake --preset vs
cmake --build --preset vs-debug
.\build\vs\Debug\lhat.exe sample\factorial.lh
```

### Linux と macOS

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/lhat sample/factorial.lh
```

Ninja で最適化ビルドを作るには、`debug` を `release` に置き換えてください。

### 実行・型検査・対話環境

`lhat` に `PATH` を通すと、次のように使えます。

```sh
lhat                    # Start the interactive prompt
lhat app.lh             # Type-check and run a program
lhat --check app.lh      # Type-check without running
lhat --help             # Show all command-line options
```

引数なしで対話環境を起動し、ファイルを指定すると型検査の後に実行します。
`--check` は実行せずに型検査だけを行い、`--help` はすべてのコマンドラインオプションを表示します。

ファイルの検査は既定で strict モードです。対話環境では relaxed モードを使い、
型を決めきれない箇所は実行時に検査することで、試行錯誤をしやすくしています。
どちらのモードでもソースの書き方は同じです。

### サンプルを読む

| サンプル | 内容 |
| --- | --- |
| [factorial.lh](sample/factorial.lh) | `this^` による無名関数の自己再帰。冒頭のグラフ表示と見比べられます。 |
| [composition.lh](sample/composition.lh) | 構造的なインタフェース、委譲、部品の再利用。 |
| [vector.lh](sample/vector.lh) | インライン値型、演算、明示的なボックス化。 |
| [hanoi3.lh](sample/hanoi3.lh) | `std.task` による並列処理と、型付きのタスク結果。 |
| [async.lh](sample/async.lh) | イベントループと連携するコルーチンスケジューラ。 |
| [24.lh](sample/24.lh) | 式のパーサを備えた、対話型の 24 ゲーム。 |

設定やデータの保存には、L^ のテーブル構文を使う [LTON](DesignDocuments/08-lton.md) も利用できます。
純粋関数だけを呼び出せる文脈で、式を評価するデータ形式です。

## アプリケーションへの組み込み

L^ は、移植しやすく組み込みやすいという Lua の重要な長所を受け継いでいます。
**コアは C11 でコンパイルでき、依存は `libc` と `libm` だけです。**
大きなランタイム基盤や外部のパッケージ群を導入する必要はありません。
オプションのスレッド・デバッグ機能は OS のスレッド API やソケット API を利用し、
ツールが使う JSON のコードはリポジトリに同梱しています。

組み込み API は [`lhat.h`](include/lhat.h) から利用できます。ホスト側では、次のことができます。

- 関数、オブジェクト型、インライン値型、列挙体、エラーを登録し、その型情報を型検査やエディタ支援に利用する。
- 独自のアロケータとモジュールローダで、メモリ管理やソースへのアクセスを制御する。
- コルーチンの実行・再開や実行予算の設定を行い、アプリケーションのイベントループと連携する。
- 開発中にコードをリロードし、C API や DAP を通じてデバッグ機能を提供する。
- コンパイル済みバイトコードを配布し、パーサ・型検査器・コンパイラを省いた VM 専用構成で実行する。

組み込みの一例は [`tests/install_smoke/host.c`](tests/install_smoke/host.c) を参照してください。
API の登録、読み込み、ホットリロード、配布については、[ホスト API の仕様](DesignDocuments/05-modules.md)にまとめています。

## ビルドとテスト

既定では、ランタイム、標準ライブラリ、CLI、言語サーバ、デバッグアダプタ、テストをビルドします。

```sh
ctest --preset debug
ctest --test-dir build/debug -L check --output-on-failure
```

テストのラベルは `core`、`check`、`vm`、`stdlib`、`lsp`、`dap`、`e2e` です。
CI では MSVC・GCC・Clang によるビルドと、サニタイザを有効にした検証、JIT を有効にした Windows でのビルドを行っています。

### ビルド構成

| configure プリセット | 用途 | build プリセット |
| --- | --- | --- |
| `debug` | Ninja による開発用ビルド | `debug` |
| `release` | Ninja による最適化ビルド | `release` |
| `asan` | サニタイザ付きデバッグビルド | `asan` |
| `pgo` | プロファイルに基づく最適化のための計測 | `pgo` |
| `vs` | Visual Studio 2026 のプロジェクト | `vs-debug`、`vs-release` |
| `vmonly` | コンパイル済みプログラム用のランタイム | `vmonly` |

各構成は [`CMakePresets.json`](CMakePresets.json) で定義しています。
PGO ビルドは [`scripts/pgo.ps1`](scripts/pgo.ps1) で自動化できます。

### オプション機能

CMake の設定時にオプションを指定できます。たとえば、テストをビルド対象から外すには次のようにします。

```sh
cmake --preset release -DLHAT_BUILD_TESTS=OFF
```

| オプション | 既定値 | 用途 |
| --- | --- | --- |
| `LHAT_BUILD_CLI` | `ON` | コマンドラインインタプリタ |
| `LHAT_BUILD_STDLIB` | `ON` | 標準ライブラリ |
| `LHAT_BUILD_LSP` | `ON` | 言語サーバ |
| `LHAT_BUILD_DAP` | `ON` | CLI のデバッグアダプタ |
| `LHAT_BUILD_TESTS` | `ON` | テストスイート |
| `LHAT_BUILD_BENCH` | `OFF` | ベンチマーク |
| `LHAT_WITH_FRONTEND` | `ON` | パーサ・型検査器・コンパイラ。省く場合は `vmonly` を利用 |
| `LHAT_WITH_DEBUGGER` | `ON` | 実行時のデバッグ支援 |
| `LHAT_WITH_COMMENTS` | `ON` | ソース・グラフ表示用のコメント保持 |
| `LHAT_WITH_RESOLUTIONS` | `ON` | ツール用の名前解決情報 |
| `LHAT_SANITIZE` | `OFF` | AddressSanitizer と、対応環境での UBSan |
| `LHAT_PGO` | `OFF` | PGO モード：`OFF`、`GENERATE`、`USE` |
| `LHAT_JIT` | `OFF` | 試験的な JIT コンパイラ（x86-64 Windows の `clang-cl` のみ） |

言語サーバにはフロントエンドと名前解決情報が、デバッグアダプタには実行時のデバッグ支援が必要です。

L^ を CMake のサブディレクトリとして取り込むホストは、コアと一緒に自分のバイナリにも PGO を適用できます。
`add_subdirectory()` の前に `LHAT_PGO`（`USE` のときは `LHAT_PGO_PROFILE` も）を設定し、
L^ をリンクするライブラリまたは実行ファイルに対して `lhat_apply_pgo(<target>)` を呼んでください。

### 試験的な JIT

`LHAT_JIT=ON` で copy-and-patch 方式の JIT コンパイラを組み込みます。
あらかじめ用意した命令ごとの機械語の型紙から実行コードを組み立て、対応していない部分はインタプリタに任せるため、
プログラムの動作は JIT の有無で変わりません。
数値計算のループはおおむね 2〜3 倍、関数呼び出しを含むコードは 1.3〜1.8 倍ほど速くなります。
現在は `clang-cl` でビルドした x86-64 Windows のみに対応しています。

```powershell
cmake --preset release -DCMAKE_C_COMPILER=clang-cl -DLHAT_JIT=ON
cmake --build --preset release
```

環境変数 `LHAT_JIT=0` を設定すると、同じバイナリのまま JIT を使わずに実行します。
機械語の型紙 `jit/stencils_x86_64-windows.h` は生成済みのものをリポジトリに含めています。
[`jit/stencils.c`](jit/stencils.c) や、型紙が参照する値の構造を変更した場合は、
`python jit/gen_stencils.py --clang <clang のパス>` で再生成してください。
JIT を有効にしたビルド自体に Python は不要です。

## 拡張DLLの型情報を出力する

フル版CLIの `--dump-host-api` に `--extension` を追加すると、指定した共有ライブラリの定義もJSONへ含められる。

```powershell
lhat --dump-host-api lhat-host.json --extension ./native/eos_lhat.dll
lhat --dump-host-api --extension ./native/first.dll --extension ./native/second.dll
```

`--extension` は複数指定でき、現在は `--dump-host-api` 専用。拡張子を含めたパスを指定し、相対パスは作業ディレクトリから解決する。出力先を省略すれば標準出力へ書く。標準ライブラリ等の登録後、全拡張の型、全拡張のメンバを登録して出力する。拡張はホストのL^版数・拡張ABIに合わせ、依存するSDKライブラリもOSが解決できる場所へ配置する。

ゲームのスクリプトは実行しないが、DLLの入口と登録関数は実行する。終了フックはProgram・レジストリの破棄後に呼ぶ。

## ドキュメントとソース

- [言語仕様](DesignDocuments/02-syntax.md)：構文、型、オブジェクト、関数、コルーチン、パターンマッチ。
- [エラー処理](DesignDocuments/04-errors.md)：型付きエラーと、その扱い方。
- [モジュールと組み込み](DesignDocuments/05-modules.md)：モジュールの読み込みとホスト API。
- [設計文書の索引](DesignDocuments/README.md)：コンパイル、開発ツール、多言語対応を含む仕様書の一覧。設計文書は日本語で記述しています。

処理系の本体は [`src/`](src/)、公開 API は [`include/`](include/)、標準ライブラリは [`stdlib/`](stdlib/) にあります。
[`lsp/`](lsp/) と [`dap/`](dap/) はエディタ・デバッガ連携、[`jit/`](jit/) は試験的な JIT、[`sample/`](sample/) はサンプルプログラムです。

## ライセンス

Apache License 2.0。[LICENSE](LICENSE) を参照してください。
