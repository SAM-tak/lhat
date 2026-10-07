# LTON — L^ Table Object Notation

L^ のテーブルをテキストで書くための綴り。読み書きの形式であり、言語の一部では
ない。実装は `stdlib/lton.c`（標準ライブラリの見本）にあり、言語の規則は
02・05 のものをそのまま使う。

## 1. 動機

設定はデータである。ところが L^ でそれを書こうとすると、テーブルリテラルを
返すチャンクになる。

```lhat
# conf.lh
return^ {
    identity = "lhatove-suite",
    window = { title = "test suite", width = 480 },
}
```

`return^ {` と `}` は書き手の言いたいことに何も足していない。**外側を外す**と
これになる。

```lton
# conf.lton
identity = "lhatove-suite",
window = { title = "test suite", width = 480 },
```

これが LTON である。JSON が JavaScript のオブジェクトリテラルに対して持つ関係
を、L^ のテーブルリテラルに対して持つ——ただし後述のとおり、**閉じたリテラル
文法ではない**。

## 2. LTON とは

**ファイル全体がテーブルリテラルの要素の列である。** 波括弧は書かない。

要素の形は 02 の 14.14 のままで、3つある。

```text
要素 := 式                # 位置。0 から順に割り当てる
      | 名前 = 式         # 名前のキー
      | [ 式 ] = 式       # 任意の値のキー
```

区切りは `,`、**末尾の `,` は許す**。`=` が推奨形で `:=` も読む（14.14改）。
空のファイルは空のテーブルになる。

新しい規則は1つも無い。**LTON はテーブルリテラルの中身であって、それ以上の
ものではない。**

## 3. 綴りは L^ のものである

コメント（`#` と `#[ ]#`）、文字列のエスケープ、数値の形式（`0x10`、`1.5e2`）
——すべて L^ と同じである。**似せているのではなく、同じ字句解析器が読んでいる**
（`stdlib/lton.c` は本文を包んで L^ の前段へ渡す）。

だから 01 の変更は自動的に LTON に及ぶ。両者が食い違うことはありえない。

## 4. 何が書けるか［重要］

**本文は `f^` の本体として読まれる。**

02 の 15.1 は「`f^` は `f^` しか呼べない」と定めている。したがって:

- 算術・比較・連結・入れ子のテーブル・条件式 — **通る**
- **`p^` の呼び出しはすべて誤り**

そして 15 章により、**効果を持つものはすべて `p^` である**。ゆえに:

> **LTON のテキストは効果を持てない。**

これが LTON を安全に読める理由の全部であり、**LTON のために書かれた検査は
1つも無い**。言語が既に持っていた規則が、そのまま境界になっている。

計算済みの値を書かせるのではなく式として書けることは、この形の目的である。

```lton
width = 480 * 2,        # 960 と書かなくてよい
name = "lhat" .. "ove",
```

## 5. 名前

**ホストが束ねた名前は見えない。** 05 の 8.2 の初期束縛は導入しない。ホストが
自分の走らせる単位のために束ねたものは、設定ファイルが名指してよいものでは
ない。`lhat_program_load_text_with` の `LhatLoadOptions.initial_bindings` が
その線である。

**LTON が名指せるのは、自分で取り込んだものだけである**（5.1）。何も取り込ま
なければ、外の名前は一つも見えない。

安全性は変わらない。取り込んだモジュールの `p^` も、4 節のとおり `f^` の本体
からは呼べない——読み込みと事前コンパイルは本文を検査する（7 節）。

### 5.1 取り込みは LTON の中に書く［確定・未実装］

定数や enum^ を LTON から使えるように、**本文の先頭に `import^` / `require^`
を書けるようにする**。

```lton
import^ game.code

{ op = game.code.Instruction.Push, arg = 1 },
{ op = game.code.Instruction.Load, arg = "x" },
```

取り込みは要素より前にだけ書ける。包み（3 節）は先頭の取り込みを前置きの
外へ出して置く。2 行目以降の行番号が書き手の見ている行と一致する性質（7 節）
は保つ。

**読み込む側から名前を渡す形は採らない**——`std.lton.load("conf.lton",
{ imports = {"std.math"} })` のような形である。何が見えるかが呼び出しの時点まで
決まらず、同じファイルが呼び出し元ごとに違う名前を見うる。言語サーバーも
事前コンパイルも、ファイルだけを見て名前の行き先を知る必要がある。

#### 言語サーバーは参照表だけ作る

言語サーバーは今までどおり `.lton` を型検査しない（7 節「診断の位置」）。
ただし**取り込んだ名前から始まる連鎖**——`game.code.Instruction.Push` の形——
に限って、名前解決だけを行い、使用箇所から宣言への参照表を作る。

- 連鎖の先は、取り込んだモジュールが公開している宣言（enum^ とその選択肢、
  定数など）を順に引けば決まる。**型推論は要らない**。型推論が要るのは値の
  メンバ（`x.foo` の `x` が局所の値）だが、データである LTON には現れない
- 参照表は `.lh` の検査器が記録するものと同じ形にする。そうすれば、宣言側の
  改名（rename）と「すべての参照の検索」が LTON の使用箇所も拾う

これが要る理由: 命令種を `"I"` `"V"` のような文字列で持つのをやめて enum^ で
書けるようにしても、改名したときに LTON 側が置き去りになるのでは意味が無い。

［補足］表に載るのは取り込んだ名前への参照だけで、数値・文字列・鍵には
1 件も作らない。1 件は使用箇所の範囲と宣言の位置・ファイルだけで足り、
数万行のファイルに 10 万件あっても数 MB に収まる見込み。型の領域を作らない
ので、型検査をしたときの大きさにはならない。さらに削るなら、表を常駐させず、
改名・参照の要求が来たときに対象の名前を本文から文字列で探して、その連鎖だけを
解決する形もとれる。どちらにするかは実装時に測って決める。

### 5.2 初期束縛は LTON に及ぼさない［確定］

`initial_bindings` は `lhat_program_load_text_with` の選択肢に留め、LTON の
入口（`std.lton.parse` / `load`、ホストの `lhatstdlib_lton_*`）には持たせない。
ホストが実行時に束ねる名前は、ファイルだけを見ても行き先が分からない——
5.1 と同じ理由である。

## 6. `nil^` ［補足］

**鍵の側**（ハッシュ部）は `nil^` を保持しない——`nil^` を入れることが鍵を
消すことだから（04 の 11.3）。したがって `a = nil^,` は**鍵を置かない**。
std.json が JSON の `null` について同じ答えを出しているのと同じ帰結である。

**位置の側**（配列部）は `nil^` を保持する（02 の 14.18、03 の 2.2）。`1, nil^, 3,` は
長さ 3 で、1 番目の位置は `nil^` のまま残る。末尾の `nil^` も同じで、
`1, 2, nil^,` の長さは 3 である。書き出し（9 節）もその位置を `nil^,` として
出すので、往復で長さは変わらない。

## 7. 読み込み

```lhat
std.lton.parse : f^string^ -> t^{}|std.lton.LtonError|std.error.OutOfMemory;
std.lton.load  : f^string^ -> t^{}|std.lton.LtonError|std.error.OutOfMemory;
errordef^ LtonError { CannotRead, Rejected, Unsupported, Cycle, TooDeep, CannotWrite }
```

- `parse` が原型。ファイル系に一切触れない
- `load` は **program の loader を通す**（05 の 8.9）。ホストが loader を
  渡していなければ何も読めない
- `Rejected` は検査・コンパイルが通らなかったこと。`p^` を呼んだ場合もここへ
  来て、**検査器の診断文がそのまま誤りの本文に乗る**
- どちらも `f^`。LTON を読むこと自体は効果ではない

返るのは `t^{}`——中身は検査時には判らないので、**添字で読む**。

```lhat
let^ conf = try^ std.lton.load("conf.lton")
print(conf["window"]["width"])       # conf.window は 03 の 3.1 が拒む
```

### バックグラウンドで読み込む

```lhat
import^std.task
import^std.lton

try^std.task.start(1)
let^job = try^std.task.async(p^{
    return^std.lton.load("conf.lton")
})
# メイン側で別の処理を進める
let^conf = try^std.task.await(job)
std.task.stop()
```

読み込み・解析・テーブル構築はワーカーで行う。`await` は完了を待ち、結果の
オブジェクト群をコピーせず呼び出し元の GC 管理へ移譲する（05 の std.task）。
描画ループなどでは `job.done()` を確認してから取得できる。同じ Task の結果は
一度限りで、2回目の `await` は `TaskError.Taken`。受信時の GC 登録の走査は残る。

### 診断の位置

言語サーバーで `.lton` を編集する場合は、同じ包みと字句・構文解析器を使い、
**文法の検査だけ**を行う。テーブルの型情報、名前解決結果、バイトコードは生成しない（5.1 の参照表は除く）。
解析対象はエディターで開いている `.lton` だけとし、ワークスペースの走査では
未オープンの LTON を解析対象に追加しない。閉じたら解析結果を解放する。
補完などのための最新テキストの解析にもこの方針を適用する。
これにより大きなデータファイルに比例して意味解析の情報が膨らむことを避ける。
実際の `std.lton.parse` / `load` と事前コンパイルでは、引き続き意味検査も実施する。

本文は1行に収まる前置きの直後に置かれる。したがって **2行目以降の行番号は
書き手の見ている行と一致する**。1行目だけ桁がずれる。

### 7改 先にコンパイルしたものを読む

`parse` と `load` は、テキストの代わりに**コンパイル済みのバイト列**（05 の
10 章）も受ける。先頭のバイトで見分ける（10.1）ので、綴りも入口も増えない。

```text
lhat --compile conf.lton -o out      # out/conf.lton にバイト列
```

包んで前段に通した結果——`f^` の本体を呼んで表を返すスクリプト——が
そのまま書き出される。読む側は前段を持たなくてよい: **VM のみビルド
（05 の 10.8）が LTON を読む道はこれだけ**で、そこにテキストを渡せば
`Rejected`（「前段なし」）になる。書く側は `lhatstdlib_lton_write`（lton.h、
CLI もこれを呼ぶ）。

4 節の境界はそのまま——検査は書き出す側で済んでおり、バイト列には
その結果しか無い。手で書き換えられるのはテキストの側だけ、というのも
VM のみビルドの性質そのものである。

## 8. ホストから直接読む

同じ2つの読みは C からも名指せる（`stdlib/lton.h`）。設定はデータで
あって、それを読むためにホストが「テーブルを返す単位」を書いて走らせる、
というのは回り道である。

```c
LhatLtonStatus lhatstdlib_lton_parse(LhatMachine *machine, LhatProgram *program,
                                     const char *name, const char *text,
                                     size_t length, LhatValue *out);
LhatLtonStatus lhatstdlib_lton_load(LhatMachine *machine, LhatProgram *program,
                                    const char *path, LhatValue *out);
```

```cpp
LhatValue conf;
if (lhatstdlib_lton_load(machine, program, "conf.lton", &conf) == LHAT_LTON_OK) {
    settings.identity = fieldString(machine, conf, "identity", settings.identity);
    settings.console  = fieldBool(machine, conf, "console", settings.console);
}
```

**登録は要らない。** program を明示的に受け取るので、設定を読みたいだけの
ホストが `std.lton` をスクリプトから見える所に置く必要はない。単位として
検査していない program でも、`lhat_program_install` していない machine でも
通る——LTON の本文は外の名前を一つも名指さないからである（5 節）。

［補足］5.1 が入れば、LTON が取り込むモジュールが届いている machine である
ことが前提に加わる。

### 失敗は3つに割れる

L^ 側の `LtonError` は2つだが、C 側は**読み先が違うので**分ける。

- `LHAT_LTON_CANNOT_READ` — loader が何も返さなかった
- `LHAT_LTON_REJECTED` — 検査・コンパイルが拒んだ。`p^` を呼んだ本文が
  来るのもここ → `lhat_program_load_failure(program)`
- `LHAT_LTON_FAULTED` — 読めて走って、止まった →
  `lhat_machine_traceback(machine, ...)`
- `LHAT_LTON_OUT_OF_MEMORY`

L^ 側では `REJECTED` と `FAULTED` がどちらも `LtonError.Rejected` になる。
違うのは本文だけで、7 節の署名は動かない。

### 返るテーブルの寿命［補足］

vm.h の「WHAT A HOST IS HOLDING IS NOT A ROOT」がここでも効く。ただし
**回収が進むのは解釈器のループの中と `lhat_machine_gc_collect`・`lhat_machine_gc_step` だけ**
なので、`lhat_machine_make_string` で鍵を作って `lhat_table_get` で引く、
という読み出しの最中に回収は起きない。**読み切ってから次を走らせる**、
だけで足りる。

またぐなら機械の届く所へ置く（`lhat_machine_set_global`）。

## 9. テキストへの書き出し

```lhat
std.lton.stringify : f^t^{} -> string^|std.lton.LtonError|std.error.OutOfMemory;
std.lton.save : p^string^, t^{} -> nil^|std.lton.LtonError|std.error.OutOfMemory;

let^text = try^std.lton.stringify(table)
try^std.lton.save("path/to.some.lton", table)
```

`stringify` はテーブルを LTON テキストにする。`save` は同じテキストを UTF-8 で
ファイルへ上書き保存し、成功時は `nil^` を返す。書き込みは副作用なので `p^`。
保存先の親ディレクトリは自動作成しない。読み込み用の program loader ではなく
ファイルシステムに直接書く。VM-only ビルドでも両方の出力機能を使用できる。

- 最上位の波括弧は省略し、入れ子は4スペース、改行は LF、各要素に末尾カンマを付ける。
- 0から連続する配列部分を位置要素として先に出す。残りのキーは数値・真偽値・文字列の
  順に並べ、数値順、`false^`→`true^`、文字列のバイト順で安定した出力にする。
- ASCII の通常の識別子キーは `name = value`、それ以外は `[key] = value` とする。
- 文字列はエスケープして復元可能にする。有限の実数は往復に必要な17桁で出す。
- 通常のテーブル、文字列、真偽値、有限の数値を扱う。キーは文字列・真偽値・有限の数値。
  定義・インスタンス、閉包、コルーチン、ホスト値などは `Unsupported`。
- 循環参照は `Cycle`、入れ子が96テーブルを超えると `TooDeep`。
  循環していない共有テーブルは各位置に展開する。共有関係、コメント、元の整形は復元しない。
- 保存先を開く前に変換を完了するため、変換失敗では既存ファイルを変更しない。
  オープン・書き込み・クローズの失敗は `CannotWrite`。書き込み中の失敗に対する
  原子的な置換は行わない。

C API の `lhatstdlib_lton_write` は従来どおりソースのバイトコード化であり、
このテキストへの直列化とは別の機能である。

## 改定履歴（要約）

- **T1・T2 を閉じた（5 節）。** T1「呼び出し側が名前を渡せる形」は、読み込む
  側から渡す形を採らず、LTON の先頭に `import^` / `require^` を書く形に決めた
  （5.1、未実装）。言語サーバーは型検査をしないまま、取り込んだ名前の参照表
  だけを作る。T2「`initial_bindings` を他の入口にも及ぼすか」は及ぼさない（5.2）
