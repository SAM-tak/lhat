# std.math.complex

複素数。

## 取り込み

```lhat
import^std.math.complex
```

## 使い方と規則

`new(re, im)` は実部と虚部を取ります。複素数同士の `*` は複素積です。零の正規化は零のままです。

## 型

### Complex

ホスト値の成分は書き換え可能な32ビット実数です。L^ からは `number^` として読み書きします。通常のテーブルなどへ保持する場合は `box^` による箱を使います。

| フィールド | 型 |
| --- | --- |
| `re` | `number^` (f32) |
| `im` | `number^` (f32) |

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### new

成分を指定して値を作る。

```text
f^number^, number^ -> std.math.complex.Complex;
```

### zero

零の値。

```text
f^ -> std.math.complex.Complex;
```

### one

実部1、虚部0の値。

```text
f^ -> std.math.complex.Complex;
```

### i

実部0、虚部1の虚数単位。

```text
f^ -> std.math.complex.Complex;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Complex.+

和。

```text
f^self^, std.math.complex.Complex -> std.math.complex.Complex;
```

### Complex.-

差、または単項の符号反転。

```text
f^self^, std.math.complex.Complex -> std.math.complex.Complex;
f^self^ -> std.math.complex.Complex;
```

### Complex.*

積。対応する型はシグネチャを参照。

```text
f^self^, number^ -> std.math.complex.Complex;
f^number^, self^ -> std.math.complex.Complex;
f^self^, std.math.complex.Complex -> std.math.complex.Complex;
```

### Complex.conjugate

共役。

```text
f^self^ -> std.math.complex.Complex;
```

### Complex.length

長さ。

```text
f^self^ -> number^;
```

### Complex.normalized

正規化した値。

```text
f^self^ -> std.math.complex.Complex;
```

### Complex.tostring

文字列表現。

```text
f^self^ -> string^;
```

## 実装と登録

[mathcomplex.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathcomplex.h) / [mathcomplex.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathcomplex.c)

[mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)
