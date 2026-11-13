# std.math.quaternion

クォータニオン。

## 取り込み

```lhat
import^std.math.quaternion
```

## 使い方と規則

`new(x, y, z, w)` の `w` はスカラー部です。クォータニオン同士の `*` はハミルトン積です。`fromAxisAngle(axis, radians)` はラジアンを取り、零の軸では `nil^`。`std.math.vector3` の登録が必要です。

## 型

### Quaternion

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
f^number^, number^, number^, number^ -> std.math.quaternion.Quaternion;
```

### identity

単位回転。

```text
f^ -> std.math.quaternion.Quaternion;
```

### fromTo

2つの方向ベクトルを結ぶ回転。零方向では `nil^`。

```text
f^std.math.vector3.Vector3, std.math.vector3.Vector3 -> std.math.quaternion.Quaternion|nil^;
```

### fromAxisAngle

軸と角度から回転を作る。

```text
f^std.math.vector3.Vector3, number^ -> std.math.quaternion.Quaternion|nil^;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Quaternion.+

和。

```text
f^self^, std.math.quaternion.Quaternion -> std.math.quaternion.Quaternion;
```

### Quaternion.-

差、または単項の符号反転。

```text
f^self^, std.math.quaternion.Quaternion -> std.math.quaternion.Quaternion;
f^self^ -> std.math.quaternion.Quaternion;
```

### Quaternion.*

積。対応する型はシグネチャを参照。

```text
f^self^, number^ -> std.math.quaternion.Quaternion;
f^number^, self^ -> std.math.quaternion.Quaternion;
f^self^, std.math.quaternion.Quaternion -> std.math.quaternion.Quaternion;
```

### Quaternion.slerp

`(相手, 係数)` の球面線形補間。

```text
f^self^, std.math.quaternion.Quaternion, number^ -> std.math.quaternion.Quaternion;
```

### Quaternion.conjugate

共役。

```text
f^self^ -> std.math.quaternion.Quaternion;
```

### Quaternion.dot

内積。

```text
f^self^, std.math.quaternion.Quaternion -> number^;
```

### Quaternion.length

長さ。

```text
f^self^ -> number^;
```

### Quaternion.normalized

正規化した値。

```text
f^self^ -> std.math.quaternion.Quaternion;
```

### Quaternion.tostring

文字列表現。

```text
f^self^ -> string^;
```

### Quaternion.rotate

ベクトルを回転。

```text
f^self^, std.math.vector3.Vector3 -> std.math.vector3.Vector3;
```

## 実装と登録

[mathquaternion.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathquaternion.h) / [mathquaternion.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathquaternion.c)

[mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)
