# エラー

## 失敗は値で表す

失敗する操作は `成功型|エラー型` を返します。エラー値は暗黙に伝播しません。
通常の返り値として受け取り、判別・代替・伝播のいずれかを明示します。
`nil^` による不在とエラーは別です。

## 種別の宣言と生成

```text
errordef^ 名前 {
    種別,
    情報付き種別 { フィールド:型, 既定値付き = 値 },
}
new^名前.情報付き種別 { フィールド = 値 }
```

種別は公称型です。同じフィールドを持つ通常のテーブルでは代用できません。
`名前.種別` は各エラー型、`名前` はその合併で、`error^` の部分型です。
既定値のないフィールドは生成時に与え、既定値のあるものは省略できます。
追加のフィールドも持てます。従来の `error^種別{…}` は互換用の生成記法です。

| 共通メンバ | 型・既定値 |
| --- | --- |
| `message` | `string^`、空文字列 |
| `cause` | `error^\|localerror^\|nil^`、`nil^` |

## その場で処理する

`式 catch^ 代替式` は、左がエラーのときだけ右を評価します。
右の式の中では `it^` がエラー値です。成功時は左の値が結果になります。
失敗しない式への `catch^` は型エラーです。
`式 catch^ nil^` は失敗を不在に置き換えます。
`式 catch^ panic^it^` は失敗時に停止することを明示します。

`do^{… catch^ 型: …}` のようなブロックの `catch^` 節は、
本体で `try^` などにより渡されたエラーを型で選んで処理します。
成功・失敗の戻り値を束縛した場合は、`fits^` やパターンマッチでも判別できます。

## 伝播する

`try^式` は、成功ならその値を返し、エラーなら現在のサブルーチンからエラーを返します。
対応する `catch^` 節があればその節へ渡します。
返り値の型は伝播するエラーを含む必要があります。
エラーを含まない式への `try^` は値・型をそのまま通します。
伝播でスコープを抜けるときも `finally^`・`with^` の後始末は実行します。

## 複数の値とエラー

複数の値か、エラーのどちらかを返すサブルーチンは、戻り値の型を `(型1, 型2)|エラー型` と書きます。

```lhat
errordef^MathError { DivisionByZero }

let^divmod = f^a:number^, b:number^ -> (number^, number^)|MathError {
    if^b = 0 {
        return^new^MathError.DivisionByZero{}
    }
    return^a // b, a % b
}

let^q, r = divmod(7, 0) catch^(0, 0)
let^show = p^a:number^, b:number^ -> nil^|MathError {
    let^x, y = try^divmod(a, b)
    print(x, y)
}
```

- `,` は `|` より弱く結合します。括弧のない `-> number^, number^|MathError` は
  `number^` と `number^|MathError` の2位置のタプルです。タプルの位置にはエラー型を置けないため、型エラーになります。
  エラーは位置の1つとしてではなく、タプル全体の代わりに返ります。
- 括弧が要るのは、合併がタプル全体を覆うときだけです。エラーを含まない戻り値は `-> number^, number^` と裸で書きます。
- タプル全体と合併できるのはエラー型と `nil^` だけです。`(number^, number^)|number^` のような合併は書けません。
  `nil^` との合併は[関数と手続き](./subroutines.md#nil-との合併は括弧で意味が変わる)を参照してください。
- `try^呼び出し()` は、成功ならタプルを分解束縛へ渡し、エラーなら伝播します。
- `catch^` の代替は同じ幅のタプルで書きます（`catch^(0, 0)`）。`catch^panic^it^` も使えます。
- タプルを含む合併は、タプルと同じく1つの名前に束縛できません。
  結果を束縛してから `fits^` で判別する形はとれないため、`try^` か `catch^` で受けます。

## ローカルエラー

`localerrordef^` は `localerror^` の下の種別を宣言します。
これは `error^` とは互いに素で、サブルーチンの戻り値型や yield 型に含められません。
合併や構造の内側に含めて外へ運ぶこともできません。
`as^` の `localerror^.CastFailure` などは、その場の `catch^` や対応する節で処理します。

## 取りこぼしと panic

成功値しか許さない型へエラーを含む値を渡すと型エラーです。
戻り値を捨てる呼び出し文も、エラーが残る場合は明示的な処理が必要です。
エラーは通常の値なので、宣言した合併と絞り込みによって網羅性を確認します。

panic は、存在すると約束したメンバが実際にはないなど、実行時に契約が破れた異常です。
通常のエラー値とは異なり、L^ の `catch^` では拾えません。実行結果としてホストへ報告されます。
整数演算の桁溢れやゼロ除算は実数へ広がり、それ自体をエラー値にはしません。

参照元：[エラー](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/04-errors.md)
（[errors.kind]、[errors.catch]、[errors.try]、[errors.exhaustive]、[errors.dropped]、[errors.runtime]）、
[構文](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/02-syntax.md)（[syntax.type.tuple]、[syntax.type.tuple-parens]）。
