# インストール

L^ は、Lua と同じくライブラリとして自分のアプリケーションに組み込んで使う言語です。
スクリプトを実行するのは組み込み先のホストアプリケーションであり、CLI の `lhat`（Windows では `lhat.exe`）は参考実装です。
アプリケーションへの組み込み方法は[組み込みガイド](../embedding/index.md)を参照してください。

ここでは、L^ のソースを編集するための VS Code 拡張を導入します。

## VS Code 拡張をインストールする

1. VS Code の拡張機能ビューを開きます。
2. Marketplace で `lhat` を検索します。
3. **L^(Lhat) Language Support**（発行元 **SAMtak**）をインストールします。
4. 作業フォルダを開き、`.lh` ファイルを作成するか、既存のファイルを開きます。

[Marketplace の拡張ページ](https://marketplace.visualstudio.com/items?itemName=SAMtak.lhat)からもインストールできます。

Marketplace からインストールするプラットフォーム別パッケージには、言語サーバー **`lhatls` が付属しています**。
補完、ホバー、定義への移動、型検査などの編集支援を使うために、言語サーバーを別途インストールする必要はありません。
これらの編集支援には CLI の `lhat` も不要です。
編集・デバッグ・ビジュアルプログラミングの操作は[VS Code の使い方](../tools/vscode.md)を参照してください。

## 自分で用意した言語サーバーを使う

自分でビルドした `lhatls` など、付属のものとは別の言語サーバーを使う場合は、
VS Code の設定で **`lhat.serverPath`** に実行ファイルのパスを指定します。

Windows の `settings.json` の例です。

```json
{
  "lhat.serverPath": "C:\\path\\to\\lhat\\build\\release\\lhatls.exe"
}
```

Linux / macOS では、たとえば `/path/to/lhat/build/release/lhatls` を指定します。
明示したパスは同梱サーバーより優先されます。指定を空に戻すと同梱サーバーを使います。
同梱サーバーがないパッケージでは、設定が空なら `PATH` 上の `lhatls` を探します。

設定後はコマンドパレットから **L^: Restart Language Server** を実行してください。
言語サーバーのビルド手順や詳しい設定は[拡張の README](https://github.com/SAM-tak/lhat-vscode-extension/blob/main/README.ja.md)を参照してください。

## 参考実装の CLI で試す場合

ホストアプリケーションを用意せずにガイドの例を実行したい場合は、CLI の `lhat` を使えます。
CLI は VS Code 拡張には含まれません。単独での実行や、CLI を起動するデバッグを試す場合に用意してください。

1. [Releases](https://github.com/SAM-tak/lhat/releases)から、使用する OS と CPU に対応した `lhat` のアーカイブを取得します。
2. 展開し、`messages` ディレクトリが含まれている場合は実行ファイルと一緒に残します。
3. 展開先を `PATH` に追加するか、実行ファイルのパスを直接指定します。

```sh
lhat --help
```

展開先のフォルダから実行する場合は、PowerShell では `./lhat.exe`、Linux / macOS では `./lhat` とします。
対応する配布物がない場合やソースからビルドする場合は、[リポジトリの README](https://github.com/SAM-tak/lhat/blob/main/README.ja.md#ビルドとテスト)を参照してください。

以降のガイドの `lhat` は、この参考実装のコマンド名です。`PATH` に追加していない場合は、実行ファイルのパスに読み替えてください。
拡張のデバッグで CLI を使う場合は、必要に応じて設定 `lhat.runtimePath` にそのパスを指定します。言語サーバー用の `lhat.serverPath` とは別の設定です。

CLI で実行を試すには[最初のプログラム](./first-program.md)、ソースの記法を学ぶには[言語の基本](./basics.md)へ進みます。
