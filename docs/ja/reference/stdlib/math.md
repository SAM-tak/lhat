# std.math

数学関数と定数。

## 取り込み

```lhat
import^std.math
```

## 使い方と規則

三角関数の角度はラジアンです。`atan2` は `(y, x)`、底を指定する `log` は `(値, 底)` の順です。

`lerp(a, b, t)` は `a + (b - a) * t`。`t` を0〜1に制限しません。

## 定数

| 名前 | 値 |
| --- | --- |
| `pi` | `3.141592653589793` |
| `tau` | `6.283185307179586` |
| `e` | `2.718281828459045` |

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### sin

正弦。

```text
f^number^ -> number^;
```

### cos

余弦。

```text
f^number^ -> number^;
```

### tan

正接。

```text
f^number^ -> number^;
```

### asin

逆正弦。

```text
f^number^ -> number^;
```

### acos

逆余弦。

```text
f^number^ -> number^;
```

### atan

逆正接。

```text
f^number^ -> number^;
```

### atan2

座標 `(y, x)` の偏角。

```text
f^number^, number^ -> number^;
```

### deg

ラジアンを度へ変換。

```text
f^number^ -> number^;
```

### rad

度をラジアンへ変換。

```text
f^number^ -> number^;
```

### sqrt

平方根。

```text
f^number^ -> number^;
```

### cbrt

立方根。

```text
f^number^ -> number^;
```

### exp

自然指数関数。

```text
f^number^ -> number^;
```

### log

自然対数、または第2引数で指定した底の対数。

```text
f^number^ -> number^;
f^number^, number^ -> number^;
```

### log2

底2の対数。

```text
f^number^ -> number^;
```

### log10

底10の対数。

```text
f^number^ -> number^;
```

### hypot

2成分のユークリッド距離。

```text
f^number^, number^ -> number^;
```

### fmod

実数の剰余。

```text
f^number^, number^ -> number^;
```

### min

1個以上の数値の最小値。

```text
f^number^, ...:number^ -> number^;
```

### max

1個以上の数値の最大値。

```text
f^number^, ...:number^ -> number^;
```

### lerp

2つの値の線形補間。

```text
f^number^, number^, number^ -> number^;
```

## 実装と登録

[math.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/math.h) / [math.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/math.c)
