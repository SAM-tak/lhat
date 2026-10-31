# ホスト API と型を公開する

API は `lhat_program_check` より前に登録します。
型検査器が読むシグネチャと、実行時に呼ぶ C 関数を、一つの登録で対応付けます。
型が互いを参照する大きな API は、**全型の登録 → 関数とメンバの登録**の二段階にすると整理できます。

実践的な記述は `stdlib/` が参考になります。
ホスト関数は `math.c`、hostdata は `io.c`、hostvalue は `mathcomplex.c` と `mathvalue_impl.h` で実装されています。
[標準ライブラリのバインド実装](./binding-examples.md)では、その実コードを引用して登録とコールバックの対応を説明します。

## 関数と手続き

[最小ホスト](./getting-started.md)は、次の手続きを公開しています。

```c
lhat_register_func(program, "app", "report", "p^number^;", report, &app);
```

`module` と `name` はスクリプト側の `app.report` に対応し、`context` はコールバックにそのまま渡されます。
登録結果を確認し、失敗した場合は型検査や実行へ進まないでください。

| シグネチャ例 | 契約 |
| --- | --- |
| `p^number^;` | 数値を一つ受け取る手続き |
| `f^number^ -> number^;` | 数値を受け取り数値を返す関数 |
| `p^self^, number^;` | 受け手と数値を受け取るインスタンスメソッド |

外部の状態を変更する操作や入出力は `p^` にします。C の実装がシグネチャを守ることはホストの責任です。
`f^` と登録したコールバックの C コードを、L^ が解析して副作用を禁止するわけではありません。

```c
static void callback(LhatMachine *machine, void *context,
                     const LhatValue *arguments, size_t count,
                     LhatValue *answers, int *answer_count);
```

戻り値は `answers` に書き、`*answer_count` を設定します。通常の `p^` は戻り値を書きません。
`number^` には整数・実数の両表現があるため、一般の数値は `lhat_number_as_real`、
整数として必要な値は `lhat_number_as_whole` などで読みます。変換前に値の種類と必要な範囲を確認してください。
C++ の例外はコールバック内で捕捉し、L^ のエラー値や `lhat_machine_panic_text` へ変換します。

## エンジンのオブジェクトは hostdata にする

ノード、ウィンドウ、テクスチャなど、ホストが所有するオブジェクトへの参照は hostdata として公開できます。

```c
const LhatHostDataTag *node_tag =
    lhat_register_hostdata_type(program, "app", "Node");
/* Register node_tag's members after checking that it is non-NULL. */
lhat_register_member(program, "app", "Node", "move",
                     "p^self^, number^;", move_node, context);
```

実行時は `lhat_machine_make_hostdata(machine, node_tag, pointer, &value)` でラッパーを作ります。
コールバックでは `lhat_hostdata_pointer(arguments[0], node_tag)` で受け手を取り出します。
結果の NULL や、エンジン側ですでに無効になった参照も処理してください。

ラッパーの GC とエンジンのオブジェクトの寿命は別です。
エンジン側で先に削除できるオブジェクトは、安定した ID と世代番号などで参照し、アクセス時に存在を確認する設計にできます。
参照カウントを持つオブジェクトなら、ラッパー作成時の retain と `dispose` メンバでの release を対応させます。
`dispose` を登録しなければ、L^ はポインタの指すものを解放しません。

解放フックは GC から呼ばれることがあり、L^ API に再入できません。
GC 経由では `answers` と `answer_count` が NULL になることにも注意してください。
同じポインタに複数のラッパーを作る場合は二重解放を避ける所有方針が必要です。
ラッパーを再利用したい場合は、独自の生ポインタキャッシュではなく [VM の弱参照キャッシュ](../api/machine.md#ホスト側のルートと弱参照)を使えます。

## ベクトルなどは hostvalue にする

小さな数値データは hostvalue として VM の連続スロットへ配置できます。
標準ライブラリの [mathvector3.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvector3.c)もこの方式で実装されています。

```c
typedef struct { float x, y, z; } Vec3;
const LhatHostValueTag *vec3_tag =
    lhat_register_hostvalue_type(program, "app", "Vec3", sizeof(Vec3));
/* After checking vec3_tag, register each field and check every result. */
lhat_register_hostvalue_field(program, "app", "Vec3", "x",
                              offsetof(Vec3, x), LHAT_HVFIELD_F32);
```

`y`、`z` も同様に登録します。演算子やメソッドは `lhat_register_hostvalue_member` で公開します。
値型はテーブルやクロージャのキャプチャ、`any^` などへそのまま保存できません。
長く保持する必要がある場合は、ホスト側でボックス化する API を設計してください。

## 標準ライブラリを追加する

`print` や `std.io` など、CLI が提供する登録がホストへ自動で追加されるわけではありません。
必要なモジュールを選んで登録します。たとえば数学ライブラリなら次の形です。

```c
#include <lhatstdlib/math.h>
/* Before checking source; check the return value. */
lhatstdlib_math_register(program);
```

インストール済み SDK では `lhat::lhatstdlib` もリンクします。
API の詳細は[登録のリファレンス](../api/registration.md)を参照してください。
