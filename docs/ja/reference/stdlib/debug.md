# std.debug

デバッグ出力とスタック情報。

## 取り込み

```lhat
import^std.debug
```

## 使用例

<<< @/../sample/docs/stdlib/debug.lh{lhat}

リポジトリのルートから実行できます。

```sh
lhat sample/docs/stdlib/debug.lh
```

## 使い方と規則

`log` はデバッグのため `f^` として公開され、関数内からも呼べます。出力先は標準出力です。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### log

文字列を改行付きでデバッグ出力。

```text
f^string^ -> nil^;
```

### traceback

現在の呼び出しスタックを文字列で取得。

```text
f^ -> string^;
```

## 実装と登録

[debug.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/debug.h) / [debug.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/debug.c)
