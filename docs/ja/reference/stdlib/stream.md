# std.stream

メモリ上のバイトストリーム。

## 取り込み

```lhat
import^std.stream
```

## 使い方と規則

`MemoryReader.read(buffer)` は実際に読めたバイト数を返します。終端は0です。`MemoryWriter.toString()` で書き込み結果を取得します。

`dispose()` 後の使用は `Error.Disposed` です。

## 型

### Error

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `Error.Disposed`

### MemoryReader

ホストが公開する型です。生成・利用には以下の API を使います。

### MemoryWriter

ホストが公開する型です。生成・利用には以下の API を使います。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### reader

文字列または `Bytes` からリーダーを作る。

```text
f^string^|std.binary.Bytes -> std.stream.MemoryReader|std.stream.Error|std.sys.Error.OutOfMemory;
```

### writer

空のメモリライターを作る。

```text
f^ -> std.stream.MemoryWriter|std.sys.Error.OutOfMemory;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### MemoryReader.read

指定バッファへ読み込み、読めたバイト数を返す。

```text
p^self^,std.binary.Bytes -> number^|std.stream.Error;
```

### MemoryWriter.write

バイトバッファを追記。

```text
p^self^,std.binary.Bytes -> nil^|std.stream.Error|std.sys.Error.OutOfMemory;
```

### MemoryWriter.toString

書き込み済みデータを文字列として取得。

```text
f^self^ -> string^|std.stream.Error|std.sys.Error.OutOfMemory;
```

### MemoryReader.dispose

資源への参照を解放します。

```text
p^self^;
```

### MemoryWriter.dispose

資源への参照を解放します。

```text
p^self^;
```

## 実装と登録

[stream.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/stream.h) / [stream.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/stream.c)
