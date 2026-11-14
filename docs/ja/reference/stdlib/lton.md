# std.lton

L^ の記法を使う設定データ。

## 取り込み

```lhat
import^std.lton
```

## 使用例

<<< @/../sample/docs/stdlib/lton.lh{lhat}

リポジトリのルートから実行できます。

```sh
lhat sample/docs/stdlib/lton.lh
```

## 使い方と規則

LTON はテーブルリテラルの内側を書く形式です。先頭のインポートも使えます。読み込みでは手続きの呼び出しを許しません。

書き出しは通常のデータテーブル、有限数、文字列、真偽値、公開された列挙体に対応します。循環はエラーで、共有する子は展開して書きます。

書き方と `.lh` との使い分けは[データファイル（LTON）](../../guide/data-files.md)を参照してください。

## 型

### LtonError

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `LtonError.CannotRead`
- `LtonError.Rejected`
- `LtonError.Unsupported`
- `LtonError.Cycle`
- `LtonError.TooDeep`
- `LtonError.CannotWrite`

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### parse

文字列からテーブルを読む。

```text
f^string^ -> t^{}|std.lton.LtonError|std.sys.Error.OutOfMemory;
```

### load

パスを指定してテーブルを読む。

```text
f^string^ -> t^{}|std.lton.LtonError|std.sys.Error.OutOfMemory;
```

### stringify

テーブルを LTON 文字列にする。

```text
f^t^{} -> string^|std.lton.LtonError|std.sys.Error.OutOfMemory;
```

### save

パスとテーブルを指定してファイルへ保存。

```text
p^string^,t^{} -> nil^|std.lton.LtonError|std.sys.Error.OutOfMemory;
```

## 実装と登録

[lton.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/lton.h) / [lton.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/lton.c)
