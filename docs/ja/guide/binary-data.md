# バイナリ保存とストリーム

`std.binary` は、スキーマを指定せずテーブルをバイト列へ保存・復元できます。
既存の `Format.encode/decode` は固定ビット形式用で、ここで使うのはモジュールの
`std.binary.encode/decode` です。フロントエンドなしの VMOnly でも利用できます。

## メモリに保存する

<<< @/../sample/docs/binary-data.lh{lhat}

実行結果は `player 12`、`player 12`、`next 13` です。
`encode` はバイナリの `string^` を返し、`decode` は `string^` と `std.binary.Bytes` を受け付けます。
復元結果は `t^{}` なので、利用する前に `fits^` で期待する形を確認します。

## Reader と Writer

`serialize(writer, table)` は順次書き、`deserialize(reader)` は1件だけ読みます。
ストリームを終了させないため、同じストリームへ複数件を続けて保存できます。
共通基底クラスは不要で、次のメソッドを持つテーブルやホスト型を渡せます。

| メソッド | 契約 |
| --- | --- |
| `read(buffer)` | buffer の現在の長さまで読む。長さを実際のバイト数に変更し、同じ数を返す。0 は EOF |
| `write(buffer)` | 全バイトを書いてから `nil^` を返す。失敗ならエラー値を返す |

バッファは `std.binary.Bytes` です。`resize(n)` で要求量を指定でき、拡張部は0になります。
読み込みは要求量より短くても構いません。メソッドはバッファを処理後に保持しないでください。
どちらも状態を進める `p^` です。

`std.stream.writer()` はメモリへ蓄積し、`toString()` で全体のコピーを返します。
`std.stream.reader(bytes)` は入力のコピーを保持します。
実装済みのストリームはこのメモリ版です。ファイルや独自の保存先はホスト側の
Reader／Writer、または C API のコールバックで接続できます。
圧縮・暗号化は将来、同じ契約を持つラッパーとして接続する予定です。

## 保存できる値と失敗

素のテーブル、整数、有限の実数、真偽値、`nil^`、任意のバイト文字列に対応します。
配列と辞書を区別し、配列の `nil^` の位置も保持します。
辞書キーは数値・真偽値・文字列です。定義・インスタンス・enum・関数・エラー・ホスト値は保存できません。
循環参照は拒否し、共有された子テーブルは別々のコピーとして復元します。
オブジェクト間の関係を保存する場合は、ゲーム側で ID に置き換える方法があります。

失敗は `std.binary.DataError`（`Truncated`、`Malformed`、`Unsupported`、`Cycle`、`Limit`、`IO`）
または `std.sys.Error.OutOfMemory`。Reader／Writer が返すエラー値もそのまま伝播します。
標準の上限は1件256 MiB、配列位置・辞書項目の合計100万、テーブルの深さ96です。

保存中は対象を変更しないでください。失敗した Writer には途中までのデータが残る場合があります。
ストリーム版はデータ全体の中間バイト列を作りませんが、元データと復元先のテーブル自体はメモリに必要です。

## C から利用する

`stdlib/binary.h` に `lhatstdlib_binary_serialize` / `lhatstdlib_binary_deserialize` があります。
Machine と read/write コールバックを渡し、Program・モジュール登録なしで使えます。
`LhatBinaryDataLimits` でサイズ・項目数の上限を指定できます。
L^ に `std.stream` を公開する場合は、`lhatstdlib_stream_register(program)` を呼びます。
