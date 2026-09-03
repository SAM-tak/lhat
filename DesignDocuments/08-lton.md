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
要素 := 式                # 位置。1 から順に割り当てる
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

**第一弾では、外の名前は一つも見えない。**

05 の 8.2 の初期束縛も導入しない。ホストが自分の走らせる単位のために束ねた
ものは、設定ファイルが名指してよいものではない。`lhat_program_load_text_with`
の `LhatLoadOptions.initial_bindings` がその線である。

したがって現状の安全性は**二重**になっている: 呼べるものが無く、かつ仮に
あっても `p^` は呼べない。前者は次の未決が外す予定のもので、後者は残る。

### ［未決 T1］呼び出し側が名前を渡せる形

`std.lton.load` の引数で、暗黙に `import^` / `require^` される対象を指定
できるようにする。そうすれば LTON から定数や `f^` を使える。

```lhat
std.lton.load("conf.lton", { imports = {"std.math"} })   # 案
```

包みの前に `import^ std.math` を置くだけで済む見込み。決めることは引数の形と、
`require^` の相対パスを何から解くか。

### ［未決 T2］`initial_bindings` を他の入口にも及ぼすか

いまは `lhat_program_load_text_with` にだけある。データとして読む入口が他にも
できたとき、同じ選択肢を持たせるか。

## 6. `nil^` ［補足］

テーブルは `nil^` を保持しない——`nil^` を入れることが鍵を消すことだから
（04 の 11.3）。したがって `a = nil^,` は**鍵を置かない**。std.json が JSON の
`null` について同じ答えを出しているのと同じ帰結である。

## 7. 読み込み

```lhat
std.lton.parse : f^string^ -> t^{}|std.lton.LtonError|std.error.OutOfMemory;
std.lton.load  : f^string^ -> t^{}|std.lton.LtonError|std.error.OutOfMemory;
errordef^ LtonError { CannotRead, Rejected }
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

### 診断の位置

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

［補足］T1 が入って呼び出し側が `import^` を渡せるようになれば、その
モジュールが届いている machine であることが前提に加わる。

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
**回収が進むのは解釈器のループの中と `lhat_machine_collectgarbage` だけ**
なので、`lhat_machine_make_string` で鍵を作って `lhat_table_get` で引く、
という読み出しの最中に回収は起きない。**読み切ってから次を走らせる**、
だけで足りる。

またぐなら機械の届く所へ置く（`lhat_machine_set_global`）。

## 9. ［未決 T3］書き出し

LTON を直列化形式にするには書き出す側が要る。std.json で判ったことがそのまま
効く: **テーブルの走査順はハッシュの順であって書き手の順ではない**ので、
02 の 14.16 と同じく**並べ直す**ことになる。そうして初めて「等しいテーブルは
同じテキストになる」「読んで書けば元のテキストが返る」が言える。

決めることは、位置要素と名前つきをどう並べるか、入れ子の字下げ、そして
書けない値（閉包・コルーチン・ホスト値）をどうするか。

## 未決事項

- **T1 — 呼び出し側が名前を渡せる形**（5 節）
- **T2 — `initial_bindings` を他の入口にも及ぼすか**（5 節）
- **T3 — 書き出し**（9 節）
