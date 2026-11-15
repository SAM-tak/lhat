# VS Code の使い方

## 拡張をインストールする

VS Code の拡張ビューで `lhat` を検索するか、
[L^ Language Support](https://marketplace.visualstudio.com/items?itemName=SAMtak.lhat)からインストールします。

Marketplace のプラットフォーム別パッケージには、言語サーバ `lhatls` が同梱されています。
編集支援のために CLI を用意する必要はありません。

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

## ファイルを編集する

作業フォルダを VS Code で開き、`.lh` ファイルを作成します。
[最初のプログラム](../guide/first-program.md)の `hello.lh` から始められます。

拡張は次の編集支援を提供します。

- コード補完とシグネチャヘルプ
- ホバーによる情報の表示
- 定義への移動
- 型検査などの診断
- 意味に基づくハイライト

診断が出た場合は、該当箇所にカーソルを合わせるか、VS Code の「問題」ビューで内容を確認してください。

## ターミナルから実行する

VS Code のターミナルを開き、ファイルのあるフォルダで実行します。
以下は参考実装の CLI を使う場合の例です。[CLI の準備](../guide/installation.md#参考実装の-cli-で試す場合)を済ませてください。

```sh
lhat hello.lh
lhat --check hello.lh
```

`lhat` が見つからない場合は、実行ファイルのパスを直接指定してください。
`PATH` を変更した後は VS Code を再起動する必要がある場合があります。

## デバッグする

拡張には、ブレークポイント、ステップ実行、変数の確認、式の評価などのデバッグ機能があります。
実行ファイルの指定や起動構成は、[拡張の README](https://github.com/SAM-tak/lhat-vscode-extension)を参照して設定してください。
ホストアプリケーションへの組み込みでは、そのホストが提供する型情報やデバッグ接続の設定も必要になります。

## ビジュアルプログラミング

拡張では `.lh` のソースをグラフとして表示・編集できます。
グラフの見せ方はまだ改善中で、実用性には制約があります。
現在の対応範囲と操作方法は、[拡張のドキュメント](https://github.com/SAM-tak/lhat-vscode-extension)を確認してください。
