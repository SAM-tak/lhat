# std.task

ワーカープールでの並行タスク。

## 取り込み

```lhat
import^std.task
```

## 使い方と規則

`start(n)` はワーカー数を指定し、引数なしならコア数を使います。`async` は未開始コルーチンか引数なしの閉包を受け取ります。

捕捉と引数はスナップショットとして渡します。結果を `await` で受け取れるのは一度だけで、2度目以降は `TaskError.Taken` です。

`await` は呼び出した機械を待たせます。自分のイベントループを持つ場合は `done()` や `awaitable()` を使います。`stop()` は未完了の仕事を破棄してワーカーを停止します。

使用例は[並行処理](../../guide/tasks.md)を参照してください。

## 型

### TaskError

各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。

- `TaskError.NotStarted`
- `TaskError.Refused`
- `TaskError.Failed`
- `TaskError.Taken`

### Task

ホストが公開する型です。生成・利用には以下の API を使います。

## 関数

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### start

ワーカープールを開始し、ワーカー数を返す。

```text
p^ -> number^|std.sys.Error.OutOfMemory;
p^number^ -> number^|std.sys.Error.OutOfMemory;
```

### stop

プールを停止する。

```text
p^;
```

### workers

ワーカー数を取得。

```text
f^ -> number^;
```

### async

仕事を投入して結果型を保持する `Task<T>` を返す。

```text
p^c^{->*->*} -> std.task.Task<ARG0.ResultType>|std.task.TaskError.NotStarted|std.task.TaskError.Refused|std.task.TaskError.Failed|std.sys.Error.OutOfMemory;
p^(p^;)|(p^->any^;) -> std.task.Task<ARG0.ReturnType>|std.task.TaskError.NotStarted|std.task.TaskError.Refused|std.task.TaskError.Failed|std.sys.Error.OutOfMemory;
```

### await

タスクの最終結果を一度だけ取得。

```text
p^std.task.Task -> ARG0.T0|std.task.TaskError.Taken|std.task.TaskError.NotStarted|std.task.TaskError.Refused|std.task.TaskError.Failed|std.sys.Error.OutOfMemory;
```

## メソッドと演算子

シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。

### Task.done

タスクが完了したかを確認。結果は消費しない。

```text
f^self^ -> bool^;
```

### Task.failed

失敗の説明文字列、または `nil^`。

```text
f^self^ -> string^|nil^;
```

### Task.awaitable

完了を待つための `std.async` の ID を取得。

```text
f^self^ -> number^;
```

### Task.dispose

資源への参照を解放します。

```text
p^self^;
```

## 実装と登録

[task.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/task.h) / [task.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/task.c)
