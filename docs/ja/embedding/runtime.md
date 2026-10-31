# フレームループと値の寿命

## 初期化と毎フレームの処理を分ける

`lhat_program_check` と `lhat_program_compile` は、通常は起動時またはスクリプト変更時に実行します。
`lhat_run` でモジュールを初期化し、公開されたコールバックや生成したインスタンスを保持します。
毎フレームは `lhat_machine_call` または `lhat_machine_call_member` で処理を呼び出します。

`lhat_machine_call_member` は受け手にある名前を解決し、`self^` を取るメンバには受け手を渡します。
単に名前でテーブルを読んで `lhat_machine_call` する方法では、インスタンスメソッドの受け手を正しく扱えない場合があります。

## C の変数は GC のルートにならない

`LhatValue callback` を C の構造体に保存しても、GC はその構造体を走査しません。
フレームをまたいで保持するコールバック、インスタンス、コルーチンは、Machine のホスト用ルートテーブルに入れます。

```c
LhatTable *roots = lhat_machine_host_root(machine);
bool refused = false;
bool stored = roots && lhat_machine_table_set(
    machine, roots, lhat_integer(0), callback, &refused);
/* Use callback later only if stored && !refused. */
```

不要になった値は、同じキーへ `lhat_nil()` を書いてルートを外します。
ホストが持つテーブルへの書き込みは `lhat_machine_table_set` を通してください。
GC が途中まで進んでいる場合も書き込みバリアが働きます。`lhat_table_set` への直接書き込みで代用しません。

Machine を破棄すると、そのヒープ上の値は無効になります。
Program が所有するコードと定数は、Machine より長く生かします。

## コルーチンと実行予算は別の中断

| 状態 | ホストの操作 |
| --- | --- |
| コルーチンが `yield^` した | `lhat_machine_resume` で次のステップへ進める |
| 実行予算で `LHAT_RUN_SUSPENDED` になった | `lhat_machine_continue` で同じ実行を続ける |

yield 可能な手続きへの呼び出しはコルーチンを返します。これをルートへ保持し、`lhat_machine_resume` で実行します。
resume が返す値は yield または最終 return の値です。終了は `lhat_machine_coroutine_done` で区別します。

`lhat_machine_set_budget(machine, turns)` はループの後方分岐回数を基準に実行を区切ります。
0 は無制限です。時間、命令数、ホストの C 関数の実行時間の上限ではありません。
ホストから L^ へ再入した入れ子の実行も、この予算では中断されません。

::: warning Machine 全体が予算で中断している場合
その Machine で別のイベントや別のコールバックを実行してはいけません。
次のフレームでは `lhat_machine_continue` で継続し、再び中断したら次回へ持ち越します。
継続する実行を捨てる場合は Machine を破棄します。
:::

複数のタスクを独立に進めたい場合は、コルーチンによるスケジューリングか、用途ごとに Machine を分ける設計を選びます。
同じ Machine を複数スレッドから同時に動かさないでください。
Program を複数スレッドから更新する場合は、実行前に `lhat_program_set_lock` でホストの mutex を渡します。
Program の作成と API 登録は直列に行います。

## 実行に失敗したら

すべての `LhatRunResult.status` を確認します。予算の中断は通常のエラーと分けます。
それ以外の失敗は、次の実行を始める前に `lhat_run_error_write` や `lhat_machine_traceback` で記録します。
型付きエラーを通常の戻り値として返す API では、`LHAT_RUN_OK` でも結果がエラー値である場合があります。

## 終了の順序

1. 新しいイベントの投入を止め、ワーカーを停止・join する。
2. スクリプトが必要とする後始末を、エンジンの状態が有効な間に行う。
3. ホストが保持する値やキャッシュを片付ける。
4. `lhat_machine_dispose` を呼ぶ。
5. `lhat_program_free` を呼ぶ。
6. Machine の解放フックが参照していたエンジンの状態を破棄する。

`lhat_registry_dispose` はプロセス内の全 Program がなくなった後にだけ呼べます。
プラグインの一つが、自分の Program を破棄しただけで呼ぶものではありません。
API の詳細は[Machine と値](../api/machine.md)を参照してください。
