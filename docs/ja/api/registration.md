# ホスト API の登録

宣言: [program.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/program.h)、
コールバック型: [object.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/object.h)。
登録は最初の `lhat_program_check` より前に完了させます。

## 関数と初期束縛

```c
bool lhat_register_func(LhatProgram *program, const char *module,
                        const char *name, const char *signature,
                        LhatHostFn call, void *context);
```

`module` のメンバとして関数または手続きを登録します。`signature` は L^ の型文字列です。
`call` と `context` は実行時に使います。context は借用されるので、実行と解放フックが使い終わるまでホストが保持します。

| API | 用途 |
| --- | --- |
| `lhat_register_global` | `L^` 直下のホスト関数を登録 |
| `lhat_bind_initial` | `L^.<member>` を修飾なしの初期名へ結び付ける。関数登録とは別の操作 |
| `lhat_register_const_integer` / `real` / `bool` / `string` | モジュールまたは登録型の静的メンバとして定数を公開 |

定数の `type` 引数が NULL ならモジュール直下、型名ならその型のメンバです。
たとえば `lhat_register_const_real` が実際の関数名です。表中の短縮名は同じ接頭辞を持つ関数を表します。

## C コールバック

```c
typedef void (*LhatHostFn)(struct LhatMachine *machine, void *context,
                           const LhatValue *arguments, size_t count,
                           LhatValue *answers, int *answer_count);
```

引数は配列で渡されます。`self^` を取るメンバは受け手も引数として渡されます。
返す値を `answers` に書き、個数を `*answer_count` に設定します。
通常呼び出しの結果領域は GC が走査するため、結果を書いた後にほかの値を作れます。
解放フックでは結果領域の二つのポインタが NULL になるので書き込みません。

## ホスト型

| API | 戻り値・用途 |
| --- | --- |
| `lhat_register_hostdata_type` | `const LhatHostDataTag *`。ホストのポインタを保持する名目的な参照型 |
| `lhat_register_hostdata_subtype` | 基底を持つ参照型。ポインタが基底として読めることはホストの保証 |
| `lhat_register_member` | 参照型のメンバを登録。先頭の `self^` がインスタンスの受け手 |
| `lhat_register_hostdata_shared` | Machine 間でポインタを共有する契約と retain/release を登録 |
| `lhat_register_hostvalue_type` | `const LhatHostValueTag *`。指定サイズのインライン値型 |
| `lhat_register_hostvalue_member` | 値型のメソッドや演算子を登録 |
| `lhat_register_hostvalue_field` | バイトオフセットとフィールド種別を指定し、数値フィールドを公開 |

タグを返す登録の NULL、bool を返す登録の false は失敗です。
同じ宣言のタグはプロセス内で共有されますが、プログラムごとの callback context は別です。
同じ宣言名で値サイズや解放契約などが食い違う登録は拒否されます。

## 列挙体、エラー、アノテーション

| API | 用途 |
| --- | --- |
| `lhat_register_enum` / `lhat_register_enum_valued` | 連番または明示した整数値を持つ列挙体 |
| `lhat_register_error_kind` | 伝播できる型付きエラーの宣言と各 variant を登録 |
| `lhat_register_local_error_kind` | ローカルエラーの宣言を登録 |
| `lhat_register_annotation` | アノテーション名と適用先を登録 |
| `lhat_register_annotation_signature` | アノテーション引数の型を登録 |
| `lhat_register_annotation_exclusive` / `lhat_register_annotation_requisite` | 排他・必要条件を登録 |
| `lhat_register_instantiation_check_handler` | 型適用に対するホスト独自の追加検査を登録 |

型付きエラーは `lhat_machine_make_error` で値を作り、通常の戻り値として返します。
シグネチャにもエラーを返す契約を含めてください。
回復不能なホスト側の失敗を実行エラーにする API は `lhat_machine_panic` / `lhat_machine_panic_text` です。

実例と寿命の設計は[ホスト API と型](../embedding/host-api.md)を参照してください。
