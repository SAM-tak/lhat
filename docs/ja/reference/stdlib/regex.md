# std.regex

正規表現。

## 取り込み

```lhat
import^std.regex
```

## 使い方と規則

繰り返し使うパターンは `new(pattern)` でコンパイルします。モジュールの `match`・`gmatch`・`gsub` は `(パターン, 対象文字列, …)` の順です。

捕捉の0番はマッチ全体、1番以降はグループです。`gsub` の置換文字列では `$1` などを使えます。探索予算の超過は `Error.Exhausted`、`gmatch` では走査終了になります。

## 型

### Error

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `Error.BadPattern`
- `Error.Exhausted`

### Regex

ホストが公開する型です。生成・利用には以下の API を使います。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### new

パターンをコンパイルする。

```text
f^string^ -> std.regex.Regex|std.regex.Error.BadPattern;
```

### match

最初のマッチ、または不一致の `nil^` を返す。

```text
f^string^, string^ -> string^|nil^|std.regex.Error.BadPattern|std.regex.Error.Exhausted;
```

### gmatch

マッチの序数と文字列を返す走査を作る。

```text
f^string^, string^ -> c^{f^ -> number^, string^ -> nil^}|std.regex.Error.BadPattern;
```

### gsub

マッチを文字列または関数で置換する。

```text
f^string^, string^, string^|f^string^, number^, t^{string^[]} -> string^|nil^; -> string^|std.regex.Error.BadPattern|std.regex.Error.Exhausted;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Regex.match

コンパイル済みパターンで最初のマッチを探す。

```text
f^self^, string^ -> string^|nil^|std.regex.Error.Exhausted;
```

### Regex.captures

マッチ全体と捕捉グループを返す。不一致は `nil^`。

```text
f^self^, string^ -> t^{string^[]}|nil^|std.regex.Error.Exhausted;
```

### Regex.gmatch

コンパイル済みパターンの全マッチを走査する。

```text
f^self^, string^ -> c^{f^ -> number^, string^ -> nil^};
```

### Regex.gsub

コンパイル済みパターンによる置換。関数にはマッチ、序数、捕捉を渡す。

```text
f^self^, string^, string^|f^string^, number^, t^{string^[]} -> string^|nil^; -> string^|std.regex.Error.Exhausted;
```

### Regex.split

マッチ間の文字列を配列にする。空の要素も保持。

```text
f^self^, string^ -> t^{string^[]}|std.regex.Error.Exhausted;
```

### Regex.dispose

資源への参照を解放します。

```text
p^self^;
```

## 実装と登録

[regex.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/regex.h) / [regex.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/regex.c)
