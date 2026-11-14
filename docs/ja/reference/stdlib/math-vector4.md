# std.math.vector4

4次元ベクトル。

## 取り込み

```lhat
import^std.math.vector4
```

## 使用例

`std.math.vector4.Vector4` は値の型です。成分を指定して構築するには、モジュール直下の `std.math.vector4.new(...)` を使います。モジュールに別名を付ければ `別名.new(...)` と書けます。

型にも別名を付け、型注釈に使えます。構築関数の `new` はモジュールの関数であり、`std.math.vector4.Vector4.new(...)` や `new^std.math.vector4.Vector4{…}` では構築できません。

<<< @/../sample/docs/stdlib/math-vector4.lh{lhat}

リポジトリのルートから実行できます。

```sh
lhat sample/docs/stdlib/math-vector4.lh
```

## 使い方と規則

ベクトル同士の `*`・`/` は成分ごと、数値との演算はスカラーによる拡大・縮小です。`lerp` は係数を0〜1に制限しません。零ベクトルの正規化は零のままです。

## 型

### Vector4

ホスト値の成分は書き換え可能な32ビット実数です。L^ からは `number^` として読み書きします。通常のテーブルなどへ保持する場合は `box^` による箱を使います。

| フィールド | 型 |
| --- | --- |
| `x` | `number^` (f32) |
| `y` | `number^` (f32) |
| `z` | `number^` (f32) |
| `w` | `number^` (f32) |

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### new

成分を指定して値を作る。

```text
f^number^, number^, number^, number^ -> std.math.vector4.Vector4;
```

### zero

零の値。

```text
f^ -> std.math.vector4.Vector4;
```

### one

各成分が1の値。

```text
f^ -> std.math.vector4.Vector4;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Vector4.+

和。

```text
f^self^, std.math.vector4.Vector4 -> std.math.vector4.Vector4;
```

### Vector4.-

差、または単項の符号反転。

```text
f^self^, std.math.vector4.Vector4 -> std.math.vector4.Vector4;
f^self^ -> std.math.vector4.Vector4;
```

### Vector4.*

積。対応する型はシグネチャを参照。

```text
f^self^, number^ -> std.math.vector4.Vector4;
f^number^, self^ -> std.math.vector4.Vector4;
f^self^, std.math.vector4.Vector4 -> std.math.vector4.Vector4;
```

### Vector4./

商。対応する型はシグネチャを参照。

```text
f^self^, number^ -> std.math.vector4.Vector4;
f^self^, std.math.vector4.Vector4 -> std.math.vector4.Vector4;
```

### Vector4.lerp

`(相手, 係数)` の線形補間。

```text
f^self^, std.math.vector4.Vector4, number^ -> std.math.vector4.Vector4;
```

### Vector4.clamp

下限・上限へ成分を制限。

```text
f^self^, number^, number^ -> std.math.vector4.Vector4;
f^self^, std.math.vector4.Vector4, std.math.vector4.Vector4 -> std.math.vector4.Vector4;
```

### Vector4.dot

内積。

```text
f^self^, std.math.vector4.Vector4 -> number^;
```

### Vector4.abs

絶対値。

```text
f^self^ -> std.math.vector4.Vector4;
```

### Vector4.length

長さ。

```text
f^self^ -> number^;
```

### Vector4.normalized

正規化した値。

```text
f^self^ -> std.math.vector4.Vector4;
```

### Vector4.tostring

文字列表現。

```text
f^self^ -> string^;
```

## 実装と登録

[mathvector4.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector4.h) / [mathvector4.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector4.c)

[mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)
