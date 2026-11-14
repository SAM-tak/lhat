# std.random

乱数と再現可能な状態。

## 取り込み

```lhat
import^std.random
```

## 使用例

<<< @/../sample/docs/stdlib/random.lh{lhat}

リポジトリのルートから実行できます。

```sh
lhat sample/docs/stdlib/random.lh
```

## 使い方と規則

`new(seed)` は0を含む同じシードから同じ系列を作ります。引数なしは OS からシードを得ます。暗号用途の乱数ではありません。

保存状態は整数のまま保持してください。実数を経由するとビットを失うことがあります。状態の互換性は乱数アルゴリズムの版に依存します。

## 型

### Error

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `Error.InvalidState`
- `Error.InvalidArgument`
- `Error.SeedUnavailable`

### Random

ホストが公開する型です。生成・利用には以下の API を使います。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### new

OS または指定したシードで生成器を作る。

```text
f^ -> std.random.Random|std.random.Error.SeedUnavailable|std.sys.Error.OutOfMemory;
f^number^ -> std.random.Random|std.sys.Error.OutOfMemory;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Random.reseed

シードから系列を再開始。

```text
p^self^, number^;
```

### Random.save

生成を進めず現在の整数状態を取得。

```text
f^self^ -> number^;
```

### Random.load

保存した整数状態を復元。

```text
p^self^, number^ -> nil^|std.random.Error.InvalidState;
```

### Random.next

`[0, 1)` の一様実数乱数を取得し状態を進める。

```text
f^self^ -> number^;
```

### Random.rangeint

`(下限, 上限)` の両端を含む整数乱数。

```text
f^self^, number^, number^ -> number^|std.random.Error.InvalidArgument;
```

### Random.uniform

`[下限, 上限)` の一様実数乱数。

```text
f^self^, number^, number^ -> number^|std.random.Error.InvalidArgument;
```

### Random.normal

`(平均, 標準偏差)` の正規乱数。

```text
f^self^, number^, number^ -> number^|std.random.Error.InvalidArgument;
```

### Random.dispose

資源への参照を解放します。

```text
p^self^;
```

## 実装と登録

[random.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/random.h) / [random.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/random.c)
