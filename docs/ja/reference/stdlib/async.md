# std.async

期限と外部完了の待機。

## 取り込み

```lhat
import^std.async
```

## 使用例

<<< @/../sample/docs/stdlib/async.lh{lhat}

リポジトリのルートから実行できます。

```sh
lhat sample/docs/stdlib/async.lh
```

## 使い方と規則

ID は機械ごとに管理されます。期限と待機上限の単位は秒です。`wait(0)` はスリープせず準備済みの完了を取得します。

`external()` の完了はホスト側から通知します。タスクの順番を決めるスケジューラ自体はこのモジュールに含めません。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### timer

指定秒数後に準備できる待機 ID を作る。

```text
f^number^ -> number^;
```

### external

ホストから完了通知を受ける待機 ID を作る。

```text
f^ -> number^;
```

### drop

待機 ID を破棄。

```text
p^number^;
```

### wait

上限秒数まで待ち、準備済み ID または `nil^` を返す。

```text
f^number^ -> number^|nil^;
```

### next

次の期限までの秒数、または `nil^`。

```text
f^ -> number^|nil^;
```

### pending

残っている待機の数。

```text
f^ -> number^;
```

## 実装と登録

[async.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/async.h) / [async.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/async.c)
