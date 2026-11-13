# std.channel

機械間の値のキュー。

## 取り込み

```lhat
import^std.channel
```

## 使い方と規則

`new(T)` は `Channel<T>`、`named(name, T)` は名前付きの共有チャネルです。型引数を省略した形も使えます。

`demand`・`supply` の時間指定は秒です。`atomic` の内側ではこれらは待機せず、`pop`・`push` 相当として動きます。

受け渡しできない値や、既存の名前付きチャネルとの型の不一致は `ChannelError.Refused` です。

## 型

### ChannelError

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `ChannelError.Refused`

### Channel

ホストが公開する型です。生成・利用には以下の API を使います。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### new

独立したチャネルを作る。

```text
p^ -> std.channel.Channel|std.sys.Error.OutOfMemory;
p^type^ -> std.channel.Channel<ARG0.T0>|std.channel.ChannelError.Refused|std.sys.Error.OutOfMemory;
```

### named

同じ名前で共有するチャネルを取得。

```text
p^string^ -> std.channel.Channel|std.sys.Error.OutOfMemory;
p^string^, type^ -> std.channel.Channel<ARG1.T0>|std.channel.ChannelError.Refused|std.sys.Error.OutOfMemory;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Channel.push

値を積み、投入番号を返す。

```text
p^self^, self^.T0 -> number^|std.channel.ChannelError.Refused|std.sys.Error.OutOfMemory;
```

### Channel.supply

値を積み、受け取られるまで待つ。第2引数は待機秒数。

```text
p^self^, self^.T0 -> bool^|std.channel.ChannelError.Refused|std.sys.Error.OutOfMemory;
p^self^, self^.T0, number^ -> bool^|std.channel.ChannelError.Refused|std.sys.Error.OutOfMemory;
```

### Channel.pop

先頭を取得。不在なら `nil^`。

```text
p^self^ -> self^.T0|nil^;
```

### Channel.demand

値が届くまで待って取得。時間切れなどは `nil^`。

```text
p^self^ -> self^.T0|nil^;
p^self^, number^ -> self^.T0|nil^;
```

### Channel.awaitable

読める状態を待つ `std.async` の ID。

```text
f^self^ -> number^;
```

### Channel.take

受信を待つコルーチンを作る。

```text
f^self^ -> c^{f^ -> number^ -> self^.T0|nil^};
```

### Channel.peek

先頭を取り出さずに読む。

```text
p^self^ -> self^.T0|nil^;
```

### Channel.count

待機中の要素数。

```text
f^self^ -> number^;
```

### Channel.hasRead

投入番号の値が受け取られたかを確認。

```text
f^self^, number^ -> bool^;
```

### Channel.clear

キューを空にする。

```text
p^self^;
```

### Channel.atomic

チャネルをロックして手続きへ渡す。

```text
p^self^, p^std.channel.Channel<self^.T0>;;
```

### Channel.dispose

資源への参照を解放します。

```text
p^self^;
```

## 実装と登録

[channel.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/channel.h) / [channel.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/channel.c)
