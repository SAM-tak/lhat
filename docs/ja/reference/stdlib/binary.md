# std.binary

バイナリ保存と固定ビット形式。

## 取り込み

```lhat
import^std.binary
```

## 使い方と規則

モジュールの `encode/decode` はスキーマ不要のデータ保存、`Format.encode/decode` は型とビット幅を決める固定形式です。

`format(型, 設定)` の型引数は結果の `Format<T>` に保持します。`decode` は値と次の位置のタプルを返します。

使用例は[バイナリ保存とストリーム](../../guide/binary-data.md)を参照してください。

## 型

### Error

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `Error.Truncated`
- `Error.Malformed`

### DataError

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `DataError.Truncated`
- `DataError.Malformed`
- `DataError.Unsupported`
- `DataError.Cycle`
- `DataError.Limit`
- `DataError.IO`

### Kind

ホストが公開する型です。生成・利用には以下の API を使います。

### Format

ホストが公開する型です。生成・利用には以下の API を使います。

`std.binary.Kind` — 基底型から継承するメンバも利用できます。

### Bytes

ホストが公開する型です。生成・利用には以下の API を使います。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### uint

指定ビット幅の符号なし整数の種類。

```text
f^number^ -> std.binary.Kind;
```

### int

指定ビット幅の符号付き整数の種類。

```text
f^number^ -> std.binary.Kind;
```

### bool

真偽値の種類。

```text
f^ -> std.binary.Kind;
```

### range

下限・上限を指定した整数の種類。

```text
f^number^, number^ -> std.binary.Kind;
```

### fixed

下限・上限・刻みを指定した固定小数の種類。

```text
f^number^, number^, number^ -> std.binary.Kind;
```

### enum

列挙体の種類。

```text
f^any^ -> std.binary.Kind;
```

### array

要素数と種類を指定した固定配列。

```text
f^number^, std.binary.Kind -> std.binary.Kind;
```

### format

型とメンバの設定から固定形式を作る。

```text
f^type^, t^{} -> std.binary.Format<ARG0.T0>;
```

### bytes

再利用できるバイトバッファを作る。

```text
f^ -> std.binary.Bytes;
```

### encode

テーブルをスキーマ不要のバイト列へ保存。

```text
f^t^{} -> string^|std.binary.DataError|std.sys.Error.OutOfMemory;
```

### decode

スキーマ不要のバイト列からテーブルを復元。

```text
f^string^|std.binary.Bytes -> t^{}|std.binary.DataError|std.sys.Error.OutOfMemory;
```

### serialize

書き込みストリームとテーブルを指定して保存。

```text
p^t^{write:p^self^,std.binary.Bytes -> nil^|error^;},t^{} -> nil^|error^;
```

### deserialize

読み込みストリームからテーブルを復元。

```text
p^t^{read:p^self^,std.binary.Bytes -> number^|error^;} -> t^{}|error^;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Kind.bits

必要なビット数。

```text
f^self^ -> number^;
```

### Kind.dispose

資源への参照を解放します。

```text
p^self^;
```

### Format.size

固定形式のサイズ。

```text
f^self^ -> number^;
```

### Format.encode

値を固定形式の文字列へ符号化。

```text
f^self^, self^.T0 -> string^;
```

### Format.encodeInto

既存の `Bytes` へ符号化。

```text
p^self^, self^.T0, std.binary.Bytes;
```

### Format.decode

データと開始位置から値と次の位置を返す。

```text
f^self^, string^|std.binary.Bytes, number^ -> (self^.T0, number^)|std.binary.Error;
```

### Format.decodeInto

既存のテーブルへ復号し次の位置を返す。

```text
p^self^, string^|std.binary.Bytes, number^, t^{} -> number^|std.binary.Error;
```

### Bytes.size

バッファのバイト数。

```text
f^self^ -> number^;
```

### Bytes.toString

バッファを文字列にする。

```text
f^self^ -> string^;
```

### Bytes.dispose

資源への参照を解放します。

```text
p^self^;
```

### Bytes.resize

バッファのサイズを変更。

```text
p^self^, number^;
```

## 実装と登録

[binary.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/binary.h) / [binary.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/binary.c)

[binary_data.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/binary_data.c)
