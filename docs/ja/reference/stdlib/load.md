# std.load

実行時のソース読み込み。

## 取り込み

```lhat
import^std.load
```

## 使い方と規則

`file(path)` と `text(source, name)` は呼び出せる手続きを返します。読み込んだ本体の実行は、その手続きを呼んだときです。

`require^` と異なり読み込みごとにコンパイルします。モジュールの実行結果は公開メンバのテーブル、スクリプトの結果はその `return^` です。

`text` の第2引数は診断用の名前で、内側の相対 `require^` の基準にもなります。

## 型

### Error

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `Error.CannotRead`
- `Error.Rejected`

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### file

実行時のパスからソースを読み込む。

```text
p^string^ -> p^... -> any^; | std.load.Error;
```

### text

ソース文字列と名前から読み込む。

```text
p^string^, string^ -> p^... -> any^; | std.load.Error;
```

## 実装と登録

[load.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/load.h) / [load.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/load.c)
