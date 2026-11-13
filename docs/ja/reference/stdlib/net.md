# std.net

待機しない UDP 通信。

## 取り込み

```lhat
import^std.net
```

## 使い方と規則

アドレスとポートを指定して送受信します。受信は待機せず、データがなければタプルの先頭が `nil^` です。

`Bytes` を使うオーバーロードには `std.binary` の登録も必要です。

## 型

### Error

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `Error.AddressInUse`
- `Error.Resolve`
- `Error.Unreachable`
- `Error.TooLarge`
- `Error.Closed`
- `Error.Failed`

### Udp

ホストが公開する型です。生成・利用には以下の API を使います。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### udp

UDP ソケットを作る。

```text
p^ -> std.net.Udp|std.net.Error;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Udp.bind

ローカルのアドレス・ポートへ結び付ける。

```text
p^self^, string^, number^ -> nil^|std.net.Error;
```

### Udp.setPeer

既定の送信先アドレス・ポートを設定。

```text
p^self^, string^, number^ -> nil^|std.net.Error;
```

### Udp.send

既定の相手へデータを送る。

```text
p^self^, string^ -> nil^|std.net.Error;
p^self^, std.binary.Bytes -> nil^|std.net.Error;
```

### Udp.sendTo

`(データ, アドレス, ポート)` を指定して送信。

```text
p^self^, string^, string^, number^ -> nil^|std.net.Error;
p^self^, std.binary.Bytes, string^, number^ -> nil^|std.net.Error;
```

### Udp.receive

データ・送信元アドレス・ポートのタプルを取得。

```text
p^self^ -> (string^|nil^, string^, number^)|std.net.Error;
```

### Udp.getLocal

ローカルのアドレス・ポートを取得。

```text
p^self^ -> string^, number^;
```

### Udp.setBroadcast

ブロードキャスト送信の有効・無効。

```text
p^self^, bool^ -> nil^|std.net.Error;
```

### Udp.dispose

資源への参照を解放します。

```text
p^self^;
```

### Udp.receiveInto

既存バッファで受信し、バッファ・送信元・ポートを返す。

```text
p^self^, std.binary.Bytes -> (std.binary.Bytes|nil^, string^, number^)|std.net.Error;
```

## 実装と登録

[net.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/net.h) / [net.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/net.c)
