# 最小のホストアプリケーション

アプリケーション自身がループを持ち、毎フレーム `update(dt)` を呼ぶ例を作ります。
CLI にスクリプトを渡す場合と違い、利用できる API はホストが登録したものになります。

## 1. ライブラリの取り込み方を選ぶ

L^ のインストールは必須ではありません。次のどちらかを使います。

| 方法 | 必要なもの | ホストのビルドで行うこと |
| --- | --- | --- |
| ソースを一緒にビルドする | L^ のソース | CMake の `add_subdirectory` で取り込み、ホストと同時にビルド |
| ビルド済み SDK を使う | 公開ヘッダ一式とリンク用ライブラリ | `find_package` で SDK を読み込み、ライブラリへリンク |

ビルド済み SDK があれば、ホストのプロジェクトに L^ のソースを取り込む必要はありません。
L^ はヘッダオンリーではないため、ヘッダに加えてライブラリのリンクが必要です。
Windows の標準ポートを使う静的ビルドでは、`lhat.lib` と `lhatport.lib` を使います。

アプリケーションの C / C++ ソースは `#include "lhat.h"` と書きますが、
インクルード先には `lhat.h` だけでなく `lhat/` 以下の公開ヘッダと、生成済みの `lhat/version.h` も必要です。
`src/` 内のプライベートヘッダは不要です。

## 2. ホストをビルドする

サンプルは両方の方法に対応しています。実際の CMake 設定は次のとおりです。

<<< @/../sample/embedding/CMakeLists.txt{cmake}

### ソースを一緒にビルドする場合

[ビルド環境](../guide/installation.md)を用意します。
Windows では先に PowerShell で `. ./scripts/devshell.ps1` を実行してください。
以下は L^ リポジトリのルートで実行します。
`/absolute/path/to/lhat` はチェックアウトの絶対パスに置き換えます。Windows のパスにも `/` を使えます。

```sh
cmake -S sample/embedding -B build/embedding -G Ninja -DCMAKE_BUILD_TYPE=Release -DLHAT_SOURCE_DIR="/absolute/path/to/lhat"
cmake --build build/embedding
```

この構成ではサンプルのビルドが L^ 自体のビルドも行います。
先に L^ を単独でビルドしたり、`cmake --install` を実行したりする必要はありません。

### ビルド済み SDK を使う場合

`LHAT_SOURCE_DIR` を指定せず、SDK のルートを `CMAKE_PREFIX_PATH` に指定します。
二つの方法では別のビルドディレクトリを使ってください。

```sh
cmake -S sample/embedding -B build/embedding-sdk -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/absolute/path/to/lhat-sdk"
cmake --build build/embedding-sdk
```

SDK の `lib/cmake/lhat` のパッケージ設定が `lhat::lhat` と `lhat::lhatport` を提供します。
対応するヘッダ、ライブラリ、CPU アーキテクチャ、ビルド構成を揃えた SDK を使ってください。
SDK 方式の場合、以降の実行パスの `build/embedding` を `build/embedding-sdk` に読み替えます。

<details>
<summary>SDK を自分で作る場合</summary>

SDK が手元にない場合は、ソースを一緒にビルドする方法を使うか、一度 SDK を作成できます。
次を L^ リポジトリのルートで実行すると、`build/docs-sdk` にヘッダ、ライブラリ、CMake 設定が揃います。

```sh
cmake --preset release
cmake --build --preset release
cmake --install build/release --prefix build/docs-sdk
```

このリポジトリのリリースワークフローは CLI と言語サーバを梱包しており、SDK を梱包する構成ではありません。
CLI の配布アーカイブとは区別してください。

</details>

## 3. スクリプトの入口を決める

<<< @/../sample/embedding/main.lh{text}

`module^demo` はスクリプトをモジュールとして宣言し、`public^let^update` は外部に公開するコールバックです。
`app` はホストが登録するモジュールです。`app.report` は標準の組み込み関数ではありません。

ホストは型検査後に次の契約を確認します。

```c
lhat_unit_export_conforms(unit, "update", "p^number^;")
```

単にファイルの型検査が通ることと、アプリケーションが必要とする入口が揃っていることは別です。
名前や引数の型を起動前に確認し、誤ったスクリプトをフレームループへ入れないようにします。

## 4. ホストから実行する

Windows:

```powershell
./build/embedding/embedding_host.exe sample/embedding/main.lh
```

Linux / macOS:

```sh
./build/embedding/embedding_host sample/embedding/main.lh
```

```text
update 1: 0.016667
update 2: 0.033333
update 3: 0.050000
```

ホストは `app.report` を登録してから型検査し、コンパイルしたモジュールを `lhat_run` で初期化します。
その戻り値が公開メンバのテーブルです。これを GC のルートへ保存してから、`lhat_machine_call_member` で `update` を呼びます。
例では各フレーム前に意図的に GC を実行し、コールバックとそのキャプチャが保持されることも確認しています。

<details>
<summary>ホスト側の全コード</summary>

<<< @/../sample/embedding/host.c{c}

</details>

## 5. 自分のアプリケーションへ移す

この例の `App` をアプリケーションの状態に置き換え、3回のループを自分のフレームループへ移します。
`LhatProgram` と `LhatMachine` はフレームごとに作り直さず、セッションの間保持します。
ファイルシステム以外から読む場合は、`lhat_load_file` を独自ローダに置き換えます。

テストは `ctest --test-dir build/embedding --output-on-failure` で実行できます。
次は[ホスト API と型](./host-api.md)で、アプリケーションの機能を公開します。
