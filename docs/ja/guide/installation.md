# インストール

L^ のプログラムを実行するには、CLI の `lhat` を使います。
VS Code 拡張は編集支援を提供しますが、実行やデバッグのための `lhat` は別途用意します。

## 配布バイナリを使う

1. [Releases](https://github.com/SAM-tak/lhat/releases)を開きます。
2. 使用する OS と CPU に対応した `lhat` のアーカイブをダウンロードします。`lhatls` は言語サーバです。
3. アーカイブを展開します。診断の翻訳に使う `messages` が含まれている場合は、実行ファイルと一緒に残します。
4. 展開先を `PATH` に追加するか、実行ファイルのパスを直接指定します。

ターミナルで次のコマンドを実行し、ヘルプが表示されることを確認してください。

```sh
lhat --help
```

PowerShell で展開先のフォルダから実行する場合は、`lhat` の代わりに `./lhat.exe` を使います。
Linux や macOS では `./lhat` を使います。実行権限がない場合は `chmod +x ./lhat` で付与してください。

対応する配布物がない場合や、最新のソースを試したい場合は、次の手順でビルドできます。

## ソースからビルドする

必要なものは Git、C11 コンパイラ、CMake 3.25 以降、Ninja です。
Windows では Visual Studio の C++ ビルドツールも用意してください。

まずリポジトリを取得します。

```sh
git clone https://github.com/SAM-tak/lhat.git
cd lhat
```

### Windows

PowerShell で以下を実行します。`devshell.ps1` は現在のシェルに MSVC のビルド環境を設定します。

```powershell
. ./scripts/devshell.ps1
cmake --preset debug
cmake --build --preset debug
./build/debug/lhat.exe --help
```

### Linux / macOS

GCC または Clang と、CMake、Ninja を用意してから実行します。

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/lhat --help
```

### ビルドした実行ファイルを使う

以降のページでは、実行ファイルを `lhat` と表記します。
`PATH` に追加していない場合は、Windows なら `./build/debug/lhat.exe`、
Linux / macOS なら `./build/debug/lhat` に読み替えてください。
これらの相対パスはリポジトリのルートから実行する場合のものです。

最適化ビルドでは、configure と build の両方のプリセットを `release` に変更します。
ビルドオプションの詳細は[リポジトリの README](https://github.com/SAM-tak/lhat/blob/main/README.ja.md#ビルドとテスト)を参照してください。

準備ができたら、[最初のプログラム](./first-program.md)へ進みます。
