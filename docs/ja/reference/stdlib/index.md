# 標準ライブラリ

ホストが登録して公開する標準モジュールのリファレンスです。各ページに用途、型、関数、メソッド、定数と登録シグネチャを掲載します。

この一覧は CLI が登録するモジュールを対象にしています。自作ホストでは必要なモジュールを登録してください。ビルド設定によって利用可能なモジュールが変わる場合があります。

| モジュール | 用途 |
| --- | --- |
| [std.math](./math.md) | 数学関数と定数 |
| [std.math.complex](./math-complex.md) | 複素数 |
| [std.math.vector2](./math-vector2.md) | 2次元ベクトル |
| [std.math.vector3](./math-vector3.md) | 3次元ベクトル |
| [std.math.vector4](./math-vector4.md) | 4次元ベクトル |
| [std.math.quaternion](./math-quaternion.md) | クォータニオン |
| [std.io](./io.md) | 標準入出力とファイル |
| [std.json](./json.md) | JSON の保存と復元 |
| [std.lton](./lton.md) | L^ の記法を使う設定データ |
| [std.regex](./regex.md) | 正規表現 |
| [std.random](./random.md) | 乱数と再現可能な状態 |
| [std.task](./task.md) | ワーカープールでの並行タスク |
| [std.thread](./thread.md) | 個別の OS スレッド |
| [std.channel](./channel.md) | 機械間の値のキュー |
| [std.async](./async.md) | 期限と外部完了の待機 |
| [std.binary](./binary.md) | バイナリ保存と固定ビット形式 |
| [std.stream](./stream.md) | メモリ上のバイトストリーム |
| [std.net](./net.md) | 待機しない UDP 通信 |
| [std.load](./load.md) | 実行時のソース読み込み |
| [std.sys](./sys.md) | 共通のシステムエラー |
| [std.debug](./debug.md) | デバッグ出力とスタック情報 |

## シグネチャの読み方

シグネチャの `f^`・`p^` は関数・手続き、`self^` は受け手です。`A|B` は成功値とエラーなどの合併で、エラーは `try^`・`catch^` などで扱います。同名の複数の署名はオーバーロードです。

`ARG0`・`ARG1` は第0・第1実引数の型です。`.T0` は第0型引数、`.ResultType` はコルーチンの最終結果型、`.ReturnType` はサブルーチンの戻り値型を表します。`self^.T0` は受け手の第0型引数です。

言語コアの規則は[概覧](../index.md)、利用例は[ガイド](../../index.md)を参照してください。
