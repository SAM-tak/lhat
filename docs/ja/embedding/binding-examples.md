# 標準ライブラリに見るバインドの実装

`stdlib/` は L^ の公開 C API を使って実装されています。
自分のアプリケーションの関数や型を L^ へ公開するときも、同じ登録 API とコールバックを使います。

| 公開したいもの | 参考になる実装 |
| --- | --- |
| ホスト関数 | [math.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/math.c)：数値引数の読み取り、戻り値、関数表による登録 |
| hostvalue（値型） | [mathcomplex.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathcomplex.c) と [mathvalue_impl.h](https://github.com/SAM-tak/lhat/blob/main/stdlib/mathvalue_impl.h)：複素数型、フィールド、演算子、コンストラクタ |
| hostdata（ホストのオブジェクトへの参照） | [io.c](https://github.com/SAM-tak/lhat/blob/main/stdlib/io.c)：`FILE *` を包む型、メソッド、`dispose` による解放 |

## 小さなモジュールの全体像

次は `stdlib/mathcomplex.c` の実コードです。

<<< @/../stdlib/mathcomplex.c{c}

このファイルは、モジュール名、型名、成分数、フィールド名を指定し、
`mathvalue_impl.h` にある共通のコールバックと登録処理を使っています。
`#define` と共通ヘッダへの分割は標準ライブラリ内で数学型を共有するための構成で、
ホストのバインドを書く際に必須の形式ではありません。

`lhatstdlib_mathcomplex_register(program)` を型検査前に呼ぶと、
`std.math.complex.Complex` と、そのメンバ、`new`、`zero`、`one`、`i` が登録されます。
`zero`、`one`、`i` は値を返す関数なので、スクリプトでは `complex.i()` のように呼びます。

以下では、実際の C API 呼び出しを担当する `mathvalue_impl.h` の記述を抜粋します。

## 1. C のデータに値型のタグを付ける

共通実装では、値そのものと登録用コンテキストを次の形で持ちます。

```c
typedef struct {
    const LhatHostValueTag *tag;
} MathModule;

typedef struct {
    float component[MATH_DIM];
} MathValue;
```

複素数では `MATH_DIM` が 2 なので、C のデータは実部と虚部の二つの `float` です。
`math_register` の冒頭では、このサイズと各成分を登録します。

```c
    module->tag = lhat_register_hostvalue_type(program, MATH_MODULE, MATH_TYPE,
                                               sizeof(MathValue));
    if (module->tag == NULL) {
        return false;
    }
    for (size_t i = 0; i < MATH_DIM; i++) {
        if (!lhat_register_hostvalue_field(program, MATH_MODULE, MATH_TYPE,
                                           math_names[i], i * sizeof(float),
                                           LHAT_HVFIELD_F32)) {
            return false;
        }
    }
```

この設定では `re` と `im` が、それぞれオフセット 0 と `sizeof(float)` のフィールドになります。
タグは、後でコールバックが値を読み書きするときに、どのホスト値型かを識別するために使います。

## 2. コールバックで引数を受け取り、値を返す

`new(re, im)` に対応するコールバックは次の記述です。

```c
static void math_new(LhatMachine *machine, void *context,
                     const LhatValue *args, size_t count,
                     LhatValue *answers, int *answer_count)
{
    if (count < MATH_DIM) {
        return;
    }
    MathValue v = { { 0 } };
    for (size_t i = 0; i < MATH_DIM; i++) {
        if (!lhat_is_number(args[i])) {
            return;
        }
        v.component[i] = (float)math_real(args[i]);
    }
    answers[0] = math_value(machine, (const MathModule *)context, v);
    *answer_count = 1;
}
```

`args` と `count` がスクリプトからの引数、`context` が登録時に渡した `MathModule *` です。
`math_real` は整数・実数のどちらの数値表現も読み取ります。
結果は `answers[0]` に書き、`*answer_count = 1` で戻り値が一つであることを伝えます。

ここで使う `math_value` は、C のデータを L^ の値型として返すためのヘルパです。

```c
static LhatValue math_value(LhatMachine *machine, const MathModule *module,
                            MathValue v)
{
    LhatValue out = lhat_nil();
    return lhat_make_hostvalue(machine, module->tag, &v, &out) ? out : lhat_nil();
}
```

`lhat_make_hostvalue` が C のデータを戻り値用の領域へコピーするので、局所変数 `v` のポインタを保持する必要はありません。

## 3. 名前・シグネチャ・実装を登録する

`math_register` の末尾にある `new` の登録は、次の C API 呼び出しです。

```c
           lhat_register_func(program, MATH_MODULE, "new", constructor_signature,
                              math_new, module);
```

| 引数 | 複素数モジュールでの意味 |
| --- | --- |
| `program` | この API を使って検査・実行する Program |
| `MATH_MODULE` | `std.math.complex` |
| `"new"` | スクリプト側の関数名 |
| `constructor_signature` | `f^number^, number^ -> std.math.complex.Complex;` |
| `math_new` | 実行時に呼ぶ C 関数 |
| `module` | 型のタグを持ち、コールバックへ渡されるコンテキスト |

演算子の登録も同じ対応付けです。`math_register` は `lhat_register_hostvalue_member` で `+` などの名前と型、
`math_add` などの C 関数を結び付けています。`self^` を取るメンバの受け手もコールバックの引数として渡されます。

自分のアプリケーションでは、この名前、シグネチャ、C の処理、コンテキストを、公開したい機能に置き換えます。
登録は型検査前、その登録を実行用 Machine へ配置する `lhat_program_install` は実行前に行います。

## hostdata の例も読む

`stdlib/io.c` の `lhatstdlib_io_register` は、`lhat_register_hostdata_type` で `std.io.File` を登録し、
`lhat_register_member` で `readLine`、`write`、`dispose` などを登録しています。
コールバックは `lhat_hostdata_pointer` でタグを検査して `File *` を取り出し、その中の `FILE *` を使います。
`file_dispose` にはストリームを閉じ、ラッパーのデータを解放する処理があります。

インラインの数値データには複素数の hostvalue、ホスト側のリソースを参照する場合には File の hostdata、
という二つの実装を、自分の型の寿命に合わせて参考にしてください。
