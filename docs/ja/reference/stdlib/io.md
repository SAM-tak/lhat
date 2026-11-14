# std.io

標準入出力とファイル。

## 取り込み

```lhat
import^std.io
```

## 使用例

<<< @/../sample/docs/stdlib/io.lh{lhat}

リポジトリのルートから実行できます。

```sh
lhat sample/docs/stdlib/io.lh
```

## 使い方と規則

`open(path, mode)` のモードは C のファイルモード（`"r"`・`"w"`・`"rb"` など）です。ファイルは `with^` または `dispose()` で閉じます。

## 型

### IOError

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `IOError.NotFound`
- `IOError.Denied`
- `IOError.Eof`

### File

ホストが公開する型です。生成・利用には以下の API を使います。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### print

値を標準出力へ表示。

```text
p^...;
```

### readLine

標準入力から1行を読む。入力終端は `IOError.Eof`。

```text
f^ -> string^|std.io.IOError.Eof|std.sys.Error.OutOfMemory;
```

### open

パスとモードを指定してファイルを開く。

```text
f^string^, string^ -> std.io.File|std.io.IOError.NotFound|std.io.IOError.Denied|std.sys.Error.OutOfMemory;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### File.readLine

ファイルから1行を読む。入力終端は `IOError.Eof`。

```text
f^self^ -> string^|std.io.IOError.Eof|std.sys.Error.OutOfMemory;
```

### File.readAll

ファイルの残りを文字列として読む。

```text
f^self^ -> string^|std.sys.Error.OutOfMemory;
```

### File.write

文字列を書き込む。

```text
p^self^, string^;
```

### File.dispose

資源への参照を解放します。

```text
p^self^;
```

## 実装と登録

[io.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/io.h) / [io.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/io.c)
