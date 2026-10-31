# Machine と値

宣言: [vm.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/vm.h)、
[value.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/value.h)、
[object.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/object.h)。

## 作成と呼び出し

```c
LhatMachine *lhat_machine_new(void);
void lhat_machine_dispose(LhatMachine *machine);
LhatRunResult lhat_run(LhatMachine *machine, const LhatProto *proto);
LhatRunResult lhat_machine_call(LhatMachine *machine, LhatValue callee,
                                const LhatValue *arguments, size_t count);
LhatRunResult lhat_machine_call_member(LhatMachine *machine,
    LhatValue receiver, const char *name, size_t length,
    const LhatValue *arguments, size_t count);
```

| API | 用途 |
| --- | --- |
| `lhat_machine_new_with_size` | 最大フレーム数とスロット数を指定。0 は既定値 |
| `lhat_run_arguments` | ユニットを引数付きで実行 |
| `lhat_machine_call` | クロージャまたはホスト関数を呼ぶ。オーバーロード群そのものは対象外 |
| `lhat_machine_call_member` | 名前でメンバを解決し、必要な受け手とオーバーロードを処理 |

結果の `status == LHAT_RUN_OK` を確認してから `value` を使います。
複数の戻り値は `positions` と `position_count` で返ります。
後続の実行まで保持する GC 管理の値は、ホスト用ルートへ保存します。

## コルーチンと実行予算

| API | 用途 |
| --- | --- |
| `lhat_machine_resume` | コルーチンを開始または再開し、yield / return の値を取得 |
| `lhat_machine_coroutine_done` | コルーチンの終了を確認 |
| `lhat_machine_make_coroutine` | C の step 関数を持つコルーチンを作成 |
| `lhat_machine_set_budget` | 後方分岐回数で実行予算を設定。0 は無制限 |
| `lhat_machine_is_suspended` | Machine が予算で中断しているか確認 |
| `lhat_machine_continue` | 予算で中断した同じ実行を新しいスライスで継続 |

`LHAT_RUN_SUSPENDED` 中は別の呼び出しを始められません。
コルーチンの yield と Machine 全体の予算中断の違いは[実行と寿命](../embedding/runtime.md)を参照してください。

## ホスト側のルートと弱参照

| API | 用途 |
| --- | --- |
| `lhat_machine_host_root` | Machine が GC ルートとして保持する、ホスト専用テーブルを取得 |
| `lhat_machine_table_set` | 書き込みバリア付きでテーブルを更新。false と `refused == true` を両方処理 |
| `lhat_machine_weak_cache_get` | ホストポインタをキーに、生存しているラッパーを取得。なければ nil |
| `lhat_machine_weak_cache_put` | ラッパーを弱く記憶。生存を保証するルートにはならない |
| `lhat_machine_weak_cache_forget` | ホストのオブジェクト破棄などに合わせてキャッシュから除去 |

ホスト用ルートから値を外すには、そのキーに nil を書きます。
`L^.modules` の登録済みモジュールは書き換えできないため、ホスト用の保持領域には使いません。

## 値の生成と読み取り

| API | 用途・注意点 |
| --- | --- |
| `lhat_nil` / `lhat_bool` / `lhat_integer` / `lhat_real` | スカラー値を作成 |
| `lhat_is_*` / `lhat_as_*` | 種類の確認と対応する表現の読み取り |
| `lhat_number_as_real` / `lhat_number_as_whole` | `number^` の整数・実数表現を考慮して変換 |
| `lhat_machine_make_string` / `lhat_machine_make_table` | Machine のヒープに文字列・テーブルを作成 |
| `lhat_table_get` / `lhat_table_get_bytes` | テーブルの値を読む |
| `lhat_machine_make_hostdata` / `lhat_hostdata_pointer` | ホストのポインタを包む / タグを検査して取り出す |
| `lhat_make_hostvalue` / `lhat_hostvalue_data` | 値型を戻り値にする / コールバックの引数から読む |
| `lhat_place_hostvalue` | ホスト所有の `LhatHostValueRoom` に引数の値型を配置 |
| `lhat_machine_make_error` | 登録したエラー種別からエラー値を作る |
| `lhat_value_text` | 値をテキスト表示。必要サイズを測定できる |

`lhat_hostvalue_data` のポインタはコールバック中だけ有効です。
`lhat_make_hostvalue` は単一の戻り値用の scratch 領域を使います。
複数の値型引数をホストから渡す場合は、引数ごとに `LhatHostValueRoom` を用意し、呼び出しが終わるまで保持します。

## GC と診断

| API | 用途 |
| --- | --- |
| `lhat_machine_gc_collect` | 完全な回収を要求。戻り値は生存オブジェクト数で、バイト数ではない |
| `lhat_machine_gc_settings` / `lhat_machine_gc_configure` | GC の進行設定を取得・変更 |
| `lhat_machine_gc_stats` / `lhat_machine_gc_step` / `lhat_machine_gc_pause` | GC の統計、手動ステップ、一時停止 |
| `lhat_machine_pending_disposals` | 後始末が残るコルーチンの数 |
| `lhat_machine_panic` / `lhat_machine_panic_text` | ホストコールバックから実行エラーを発生させる |
| `lhat_run_error_write` / `lhat_machine_traceback` | 実行失敗のレポートとトレースバック |
| `lhat_machine_fault_depth` / `lhat_machine_fault_frame` | 失敗した実行のフレームを取得 |

GC は、破棄されたコルーチンの後始末として L^ コードを実行する場合があります。
GC と Machine の破棄が終わるまで、ホストのコンテキストと登録内容を有効にしておきます。
