# std.json

JSON の保存と復元。

## 取り込み

```lhat
import^std.json
```

## 使用例

<<< @/../sample/docs/stdlib/json.lh{lhat}

リポジトリのルートから実行できます。

```sh
lhat sample/docs/stdlib/json.lh
```

## 使い方と規則

空でない配列部分だけのテーブルは JSON 配列、それ以外はオブジェクトです。空テーブルは `{}` になります。

循環、対応しない値、非有限数などはエラーです。`pretty` の字下げ幅は0〜16の整数です。

## 型

### JsonError

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `JsonError.BadText`
- `JsonError.Unsupported`
- `JsonError.TooDeep`

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### encode

テーブルを JSON 文字列にする。

```text
f^t^{} -> string^|std.json.JsonError|std.sys.Error.OutOfMemory;
```

### pretty

テーブルと字下げ幅から整形済み JSON を作る。

```text
f^t^{}, number^ -> string^|std.json.JsonError|std.sys.Error.OutOfMemory;
```

### decode

JSON 文字列をテーブルへ復元。

```text
f^string^ -> t^{}|std.json.JsonError|std.sys.Error.OutOfMemory;
```

## 実装と登録

[json.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/json.h) / [json.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/json.c)
