# std.math.vector2

2次元ベクトル。

## 取り込み

```lhat
import^std.math.vector2
```

## 使い方と規則

ベクトル同士の `*`・`/` は成分ごと、数値との演算はスカラーによる拡大・縮小です。`lerp` は係数を0〜1に制限しません。零ベクトルの正規化は零のままです。

座標は右が+x、下が+yです。`angle()` は `atan2(y, x)`、`rotate(radians)` はラジアンで回転します。

## 型

### Vector2

ホスト値の成分は書き換え可能な32ビット実数です。L^ からは `number^` として読み書きします。通常のテーブルなどへ保持する場合は `box^` による箱を使います。

| フィールド | 型 |
| --- | --- |
| `x` | `number^` (f32) |
| `y` | `number^` (f32) |

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### new

成分を指定して値を作る。

```text
f^number^, number^ -> std.math.vector2.Vector2;
```

### zero

零の値。

```text
f^ -> std.math.vector2.Vector2;
```

### one

各成分が1の値。

```text
f^ -> std.math.vector2.Vector2;
```

### right

右方向の単位ベクトル。

```text
f^ -> std.math.vector2.Vector2;
```

### left

左方向の単位ベクトル。

```text
f^ -> std.math.vector2.Vector2;
```

### up

上方向の単位ベクトル。

```text
f^ -> std.math.vector2.Vector2;
```

### down

下方向の単位ベクトル。

```text
f^ -> std.math.vector2.Vector2;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Vector2.+

和。

```text
f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
```

### Vector2.-

差、または単項の符号反転。

```text
f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
f^self^ -> std.math.vector2.Vector2;
```

### Vector2.*

積。対応する型はシグネチャを参照。

```text
f^self^, number^ -> std.math.vector2.Vector2;
f^number^, self^ -> std.math.vector2.Vector2;
f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
```

### Vector2./

商。対応する型はシグネチャを参照。

```text
f^self^, number^ -> std.math.vector2.Vector2;
f^self^, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
```

### Vector2.lerp

`(相手, 係数)` の線形補間。

```text
f^self^, std.math.vector2.Vector2, number^ -> std.math.vector2.Vector2;
```

### Vector2.clamp

下限・上限へ成分を制限。

```text
f^self^, number^, number^ -> std.math.vector2.Vector2;
f^self^, std.math.vector2.Vector2, std.math.vector2.Vector2 -> std.math.vector2.Vector2;
```

### Vector2.angle

角度。

```text
f^self^ -> number^;
```

### Vector2.rotate

ベクトルを回転。

```text
f^self^, number^ -> std.math.vector2.Vector2;
```

### Vector2.dot

内積。

```text
f^self^, std.math.vector2.Vector2 -> number^;
```

### Vector2.cross

外積。

```text
f^self^, std.math.vector2.Vector2 -> number^;
```

### Vector2.abs

絶対値。

```text
f^self^ -> std.math.vector2.Vector2;
```

### Vector2.length

長さ。

```text
f^self^ -> number^;
```

### Vector2.normalized

正規化した値。

```text
f^self^ -> std.math.vector2.Vector2;
```

### Vector2.tostring

文字列表現。

```text
f^self^ -> string^;
```

## 実装と登録

[mathvector2.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector2.h) / [mathvector2.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector2.c)

[mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)
