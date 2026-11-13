# std.thread

個別の OS スレッド。

## 取り込み

```lhat
import^std.thread
```

## 使い方と規則

`spawn(手続き, 引数…)` は新しいスレッドで手続きを実行します。引数と捕捉はコピー可能な形で受け渡します。

`sleep` の単位は秒です。`join()` は完了を待ち、未取得のハンドルの `dispose()` もスレッドの完了を待ちます。

## 型

### ThreadError

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `ThreadError.NotSpawnable`
- `ThreadError.BadArgument`
- `ThreadError.SpawnFailed`
- `ThreadError.BadResult`
- `ThreadError.AlreadyJoined`

### ThreadHandle

ホストが公開する型です。生成・利用には以下の API を使います。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### spawn

手続きと引数から新しいスレッドを開始。

```text
f^p^... -> any^;, ...:any^ -> std.thread.ThreadHandle|std.thread.ThreadError.NotSpawnable|std.thread.ThreadError.BadArgument|std.thread.ThreadError.SpawnFailed|std.sys.Error.OutOfMemory;
```

### sleep

現在のスレッドを指定秒数休止。

```text
p^number^;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### ThreadHandle.join

完了まで待って戻り値を取得。再取得は `AlreadyJoined`。

```text
f^self^ -> any^|std.thread.ThreadError.AlreadyJoined|std.thread.ThreadError.BadResult|std.sys.Error.OutOfMemory;
```

### ThreadHandle.done

スレッドの終了を確認。

```text
f^self^ -> bool^;
```

### ThreadHandle.awaitable

完了を待つ `std.async` の ID。

```text
f^self^ -> number^;
```

### ThreadHandle.failed

失敗の説明文字列、または `nil^`。

```text
f^self^ -> string^|nil^;
```

### ThreadHandle.dispose

資源への参照を解放します。

```text
p^self^;
```

## 実装と登録

[thread.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/thread.h) / [thread.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/thread.c)
