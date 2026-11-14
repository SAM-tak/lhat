# std.sys

共通のシステムエラー。

## 取り込み

```lhat
import^std.sys
```

## 使用例

エラーの処理方法を示すため、例ではエラー値を明示的に作っています。実際にメモリを使い切る操作は行いません。

<<< @/../sample/docs/stdlib/sys.lh{lhat}

リポジトリのルートから実行できます。

```sh
lhat sample/docs/stdlib/sys.lh
```

## 使い方と規則

`std.sys.Error.OutOfMemory` は複数のライブラリが共有するメモリ不足の型です。このモジュールには独立した関数はありません。

## 型

### Error

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `Error.OutOfMemory`

## 実装と登録

[sys.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/sys.h) / [sys.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/sys.c)
