# std.math.vector3

3次元ベクトル。

## 取り込み

```lhat
import^std.math.vector3
```

## 使い方と規則

ベクトル同士の `*`・`/` は成分ごと、数値との演算はスカラーによる拡大・縮小です。`lerp` は係数を0〜1に制限しません。零ベクトルの正規化は零のままです。

+Zが前、−Zが後です。

## 型

### Vector3

ホスト値の成分は書き換え可能な32ビット実数です。L^ からは `number^` として読み書きします。通常のテーブルなどへ保持する場合は `box^` による箱を使います。

| フィールド | 型 |
| --- | --- |
| `x` | `number^` (f32) |
| `y` | `number^` (f32) |
| `z` | `number^` (f32) |

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### new

成分を指定して値を作る。

```text
f^number^, number^, number^ -> std.math.vector3.Vector3;
```

### zero

零の値。

```text
f^ -> std.math.vector3.Vector3;
```

### one

各成分が1の値。

```text
f^ -> std.math.vector3.Vector3;
```

### right

右方向の単位ベクトル。

```text
f^ -> std.math.vector3.Vector3;
```

### left

左方向の単位ベクトル。

```text
f^ -> std.math.vector3.Vector3;
```

### up

上方向の単位ベクトル。

```text
f^ -> std.math.vector3.Vector3;
```

### down

下方向の単位ベクトル。

```text
f^ -> std.math.vector3.Vector3;
```

### forward

前方向の単位ベクトル。

```text
f^ -> std.math.vector3.Vector3;
```

### back

後方向の単位ベクトル。

```text
f^ -> std.math.vector3.Vector3;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Vector3.+

和。

```text
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

### Vector3.-

差、または単項の符号反転。

```text
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
f^self^ -> std.math.vector3.Vector3;
```

### Vector3.*

積。対応する型はシグネチャを参照。

```text
f^self^, number^ -> std.math.vector3.Vector3;
f^number^, self^ -> std.math.vector3.Vector3;
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

### Vector3./

商。対応する型はシグネチャを参照。

```text
f^self^, number^ -> std.math.vector3.Vector3;
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

### Vector3.lerp

`(相手, 係数)` の線形補間。

```text
f^self^, std.math.vector3.Vector3, number^ -> std.math.vector3.Vector3;
```

### Vector3.clamp

下限・上限へ成分を制限。

```text
f^self^, number^, number^ -> std.math.vector3.Vector3;
f^self^, std.math.vector3.Vector3, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

### Vector3.dot

内積。

```text
f^self^, std.math.vector3.Vector3 -> number^;
```

### Vector3.cross

外積。

```text
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

### Vector3.abs

絶対値。

```text
f^self^ -> std.math.vector3.Vector3;
```

### Vector3.length

長さ。

```text
f^self^ -> number^;
```

### Vector3.normalized

正規化した値。

```text
f^self^ -> std.math.vector3.Vector3;
```

### Vector3.tostring

文字列表現。

```text
f^self^ -> string^;
```

## 実装と登録

[mathvector3.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector3.h) / [mathvector3.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector3.c)

[mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)
