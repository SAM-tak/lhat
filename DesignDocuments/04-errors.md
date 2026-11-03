# 04 誤りの表現と伝播

[syntax.with.errors] が「誤りは値として返す」とだけ定め、
表現の形・伝播の記法・受け取り損ねの検出を本文書に委ねた。ここで決める。

節番号のうち「02 の」「01 の」と前置きしたものは他文書を指す。
未決事項には `E` を冠して [errors.open] にまとめる。

---

## 1. 前提 [errors.premise]

[syntax.with.errors] で確定していることを再掲する。

- **L^ は例外機構を持たない。** 失敗しうる操作は誤りを表す値を返す
- 誤りは暗黙に伝播しない。したがって
  「後始末の最中に例外が飛んだらどうするか」という問題が発生しない
- `dispose()` は誤りを返さない（[syntax.with.no-fail]）
- `finally^` は返り値を変更できない（[syntax.finally.result]）

本文書はこの上に、値の形と、それを扱う記法を載せる。

---

## 2. 誤りの種別は型である [errors.kind]

### 2.1 なぜテーブルではないのか［重要］ [errors.kind.not-table]

誤りを普通のテーブルで表す道は成立しない。構造的型付けを採る以上、
`t^{ kind : string^, message : string^ }` と書けば済むように見えるが、
理由は [syntax.obj.override] が多重定義のために定めた「互いに素」の規則にある。

> **構造どうしは、同名のメンバの型が互いに素でなければ、互いに素にならない。**

誤りを扱う記法は、`T|E` という値に対して
**「これは誤りか」を確実に判定できなければ成立しない**。
`E` がテーブル構造で `T` もテーブルなら、両方を満たす値が普通に作れてしまい、
この判定に答えがない。

```lhat
# もしテーブルで表したら
f^ load() -> t^{value : number^} | t^{kind : string^, message : string^};

# 両方を満たす値が作れる。どちらとして扱えばよいのか決まらない
{ value := 1, kind := "x", message := "y" }
```

言語の中核にある制御構造が、**型システムの強制できない命名規約** に乗ることになる。
`.kind` を持っていたら誤り、という約束は、破っても検査器が何も言わない。

### 2.2 `errordef^` — 種別を宣言する [errors.kind.errordef]

誤りの種別は **あらかじめ宣言する**。宣言形式であり、名前を伴う。

```lhat
errordef^ IOError {
    NotFound,
    Denied,
}
```

宣言語は2つある。`localerrordef^` は [errors.kind.local] が定めるもう一方の頂点の下に
置く。**綴りは以下すべて同じ**で、違うのは何の下に在るかだけである。

種別は付随情報を宣言できる。

```lhat
errordef^ ParseError {
    Syntax { line : number^, column : number^ },
    Eof,
}
```

#### フィールドは既定値を持てる

```lhat
errordef^ ParseError {
    Syntax { line = 0, column = 0 },
    Eof,
}
```

既定値を書いた場合、**型は書かなくてよい**。初期値の型がそのフィールドの型になる。
両方書いてもよい。

```text
フィールド = 名前 : 型
          | 名前 = 式
          | 名前 : 型 = 式
```

##### 綴りは [syntax.obj.keys] に従う

`=` が推奨形であり、`:=` も読む。**両者は同じ意味である。**

```lhat
errordef^ ParseError {
    Syntax { line := 0 },     # 許容形。推奨はしない
}
```

波括弧が開く5つの場所は [syntax.obj.keys] がまとめて定めており、
種別のフィールドと [errors.kind.new] の生成はそのうちの2つにあたる。

##### 既定値を持つフィールドは生成時に省ける

[errors.kind.new] のとおり、既定値のないフィールドは生成時にすべて与える。
既定値のあるフィールドは省いてよく、省けばその値になる。

[syntax.obj.new] が雛形について定めたのと同じ扱いである。
初期値は **生成のたびに評価される式** であり、格納された値ではない。

### 2.3 宣言が作るもの [errors.kind.made]

`errordef^ IOError { NotFound, Denied }` は次を作る。

- **`IOError.NotFound`、`IOError.Denied`** — それぞれが **型**
- **`IOError`** — その2つの合併
- どちらも `error^` の部分型

```text
IOError.NotFound  <:  IOError  <:  error^
```

**種別が型であることが本章の要になっている。** [syntax.repl] と [pipeline.open] がこれに乗る。

種別の集合は **1段** であり、入れ子は設けない。
名前が長くなる問題は階層ではなく **名前空間** の話であり、誤りに限らない。
[05-modules.md](05-modules.md) で扱う。取り違えは [errors.kind.nominal] が防ぐ（[modules.label.errors]）。

どの種別も次の2つを持つ。宣言に書く必要はない。

| メンバ | 型 | 意味 |
| --- | --- | --- |
| `message` | `string^` | 人間向けの説明。既定は空文字列 |
| `cause` | `error^`\|`localerror^`\|`nil^` | 連鎖元。既定は `nil^`（[errors.chain]） |

種別を表す別のメンバは持たない。**種別は型そのもの** であり、
`typeof^(e)` が `IOError.NotFound` を返す。

#### 腕が全部誤りの合併も、この2つを答える

**どの種別も持つ**以上、種別の合併も持っている。

```lhat
f^ read() -> string^|IOError.Eof|Memory.Out { … }

read() catch^ it^.message        # 通る。宣言が2つにまたがっていても
```

呼び出しが2通りに失敗しうるとき、`catch^` の `it^`（[errors.catch.it]）は
**別々の宣言の種別にまたがる合併**になる。そこで `message` が引けないと、
どちらの失敗かを先に絞らないと文言すら読めないことになる。

これは誤りのための特例ではなく、**合併が答えるのは全ての腕が答えるもの**
という読みをそのまま当てたものである。腕が全部種別なら、
腕は全部 `message` と `cause` を持っている。

**[lex.hat.one-use] が宣言したフィールドは別**である。あれは葉のものなので、
[syntax.op.framework] の絞り込みで葉まで降りてから引く。

```lhat
read() catch^ it^.path                                   # 誤り。葉のもの
read() catch^ if^ it^ fits^ IOError.Eof: it^.path el^: "";  # これでよい
```

#### 構造型とは比べない

`t^{ message : string^ }` のような構造型に誤りが適合することはない。
[syntax.juxta.call-binds] のとおり **誤りは公称型** であり、`message` を持つことは
「`message` を持つ何か」であることを意味しない。
構造で受ける手続きに誤りを渡したいなら、受け側を `error^` と書く。

### 2.4 構造的型付けの例外［重要］ [errors.kind.nominal]

```lhat
errordef^ IOError   { NotFound, Denied }
errordef^ UserError { NotFound, Invalid }
```

この2つの `NotFound` は **構造が同じ** である。
[syntax.op.semantic] と [syntax.obj.name] の規則をそのまま適用すれば同じ型になり、取り違えが起きる。

> **誤りの種別の同一性は、構造ではなく宣言の場所で決まる。**

**これは構造的型付けの明示的な例外である。**

例外が正当なのは、記述している対象が違うためである。

- **`def^` は「形」を記述する。** 同じ形なら通じ合うべきである。
  それが構造的型付けの利点そのものであり、[syntax.obj.name] が名前をラベルに留めた理由でもある
- **`errordef^` は「互いに異なる種別」を宣言する。**
  別々に宣言した種別が混ざらないことが目的そのものである

実際に書かれる種別名は `NotFound` `Invalid` `Timeout` `Denied` に集中する。
**衝突は例外的な事故ではなく通常の事態** であり、
構造で同一視すると標準ライブラリと利用者の種別が一致してしまう。

同一性は **原文の宣言の位置** に結び付く。

TypeScript が構造的型付けを採りながら `enum` だけを公称にしているのが同じ例外である。

#### 記法にも違いが出る

[syntax.obj.name] が `def^` を式にしたのは、名前が同一性に関与しないからだった。
名前がラベルにすぎないなら束縛から拾えばよい。

**`errordef^` では名前が同一性そのものである。**
ならば名前は、束縛される先から拾うものではなく宣言に書かれるべきものになる。

> **記法の違いが、意味の違いを表している。**

無名の種別は書けない。書く手段が存在しない。
これは [syntax.obj.structure] の要請でもある。
無名の種別には注釈として書ける名前がなく、構造で書き表すこともできないためである。

### 2.5 生成 — `new^種別{ … }`（従来の `error^種別{ … }` も可） [errors.kind.new]

共通の構築構文 `new^型{ … }`（[syntax.obj.new]）で、具体的なエラー種別の値を作れる。
既定値・必須フィールド・種別の識別・`message` と `cause` の扱いは従来と同じ。
`error^` は型の位置では引き続きエラーの頂点型を表す。
後方互換性のため、以下の `error^種別{ … }` による生成も引き続き受け付ける。
ただし、この旧生成構文は**将来削除予定（時期未定）**とし、新しいコードでは
`new^種別{ … }` を使う。頂点型としての `error^` は引き続き存続する。

```lhat
return^ new^IOError.NotFound{ message = "no such file" }
```

```lhat
return^ error^IOError.NotFound{ message = "no such file" }

return^ error^ParseError.Syntax{
    message = "unexpected ')'",
    line = 3,
    column = 12,
}
```

波括弧が開くので `=` が推奨形であり、`:=` も同じ意味で読む
（[syntax.obj.keys]）。

**既定値のないフィールドはすべて与える。**
既定値のあるものは省いてよい（[syntax.juxta.command]）。`message` と `cause` は常に任意である。

値として組み立ててから返してもよい。

```lhat
let^ e = error^IOError.Denied{ message := "permission denied" }
return^ e
```

#### 構築を明示する前置は必要

`IOError.NotFound{ … }` という前置なしの形は採れない。
その形では **構文解析器が型の知識なしに判別できない**。
修飾された名前に `{` が続く形は、名前の式とテーブルリテラルが並んだ形とも読める。

`new^`（従来の綴りでは `error^`）が前に付けば、その1トークンで決まる。

［補足］ 前置を落とした `IOError.NotFound` は、値の位置に**型**を書いた形に
なる。種別の集合はスコープにある名前なので、これはメンバ参照として読めてしまう
——検査器はそこで「そんなメンバは無い」ではなく、**種別は型であって値ではない**
と言う。綴り間違いを探しに行かせないためである。

### 2.6 `error^` は他のあらゆる型と互いに素［重要］ [errors.kind.disjoint]

> **誤りの種別を満たす値は、誤り以外のどの型も満たさない。**

これが [syntax.juxta.free-error] の問題を解く。`T|IOError` はいつでも判別できる。

[syntax.obj.override] が定めた互いに素の判定に、次が加わる。

- **異なる種別どうし** — 互いに素
- **種別と誤り以外** — 互いに素
- **頂点と、その下の種別** — 互いに素にならない（上位型のため）
- **`error^` と `localerror^`** — 互いに素（[errors.kind.local]）

この性質は規則として置いたものであり、構造から導かれるものではない。
そこが宣言された型にする意味である。

### 2.7 `localerror^` — もう一つの頂点［重要］ [errors.kind.local]

> **`localerrordef^` が宣言する種別は `localerror^` の下に在り、
> `error^` とは互いに素である。**

```lhat
errordef^      IOError { NotFound, Denied }      # 伝播できる
localerrordef^ IOFatal { Interrupted, Torn }     # 伝播できない（2.7改）
```

```text
IOError.NotFound  <:  IOError  <:  error^
IOFatal.Torn      <:  IOFatal  <:  localerror^
```

宣言の綴りは [lex.hat.one-use] のまま——変種・フィールド・既定値・[errors.kind.new] の生成、すべて同じ。
違うのは**何の下に在るか**だけである。

#### なぜ `error^` の下に置かないか［重要］

[errors.kind.made] は `error^` を**全種別の上位**と定めている。伝播できない種別をその下に
置けば、`-> T|error^` がそれを含みうることになり、**`error^` 自体が返り値型
に書けなくなる**。上の表の `cause` も `error^` で書かれているので、
巻き添えで全種別が返せなくなる。

だから頂点を2つに分け、互いに素にする。**`error^` の規則は1つも変わらない。**
[errors.kind.disjoint] の表に行が1つ増えるだけである。

#### 宣言が単位であり、変種ごとではない

1つの `errordef^` の中に両方は入れられない。[errors.kind.made] が群を変種の合併と定めて
いるので、**合併が2つの頂点に跨がってしまう**からである。混ぜたければ
宣言を2つ書く。

#### 使用側に印は要らない［補足］

`IOFatal.Interrupted` と名指せば足りる。構造型に付く印なら型に乗せるほか
ないが、誤りの種別は [errors.kind.nominal] が **名前的**と定めており、同一性は宣言そのものである
（[modules.label.errors]）。名指せば
必ずその宣言に解決するので、**属性は宣言が運ぶ**。

#### `localerror^` の下は組込みの名前空間

言語が自分のために宣言する種別はここに置く。**登録も `import^` も要らず、
常に在る。**

```lhat
localerror^.CastFailure     # 11.6改3。as^ が外れたときの誤り
```

`errordef^` を書かないので群が無い——`localerror^` の直下に単独で在る。
`typeof^` はその名前をそのまま答える。

`error^` の側には同じ名前空間を設けない。**中核の演算子が失敗を答えるなら、
その失敗はその場で解決させたいものである**——伝播させたいなら、それは
書き手が自分の `errordef^` で名前を付けるべきものである。

［補足］この置き場所は層の問題でもある。`std.sys` は標準ライブラリの
モジュールなので、中核の演算子の失敗型がその登録に依存すると、
それを登録しないホストで演算子自体が書けなくなる。

#### 「誤り」は両方を指す

[errors.catch] ・[errors.try] ・[syntax.stmt.no-bare-expr] の「誤り」は、以降 `error^|localerror^` の意味である。
`catch^` はどちらも受け、`catch^:` の `it^` は両方の合併になる。
演算子の側に分岐は無い——**`try^` が一方を拒むだけ**である。

### 2.7+ 伝播を禁じる［重要］ [errors.kind.local-no-propagate]

> **`localerror^` に触れる型は、サブルーチンの返り値型にも yield 型にも
> 現れてはならない——書いたものにも、推論したものにも、その内側にも。**

強制点は既にある。[errors.try.result] の「`try^` は、今の手続きの返り値の型がその誤りを
含む場合にのみ書ける」の**手前**に一段置くだけである。[errors.try.result] と違い、
**署名を広げても通らない**——広げた署名そのものがこの規則で拒まれる。

```lhat
p^ {
    let^ v = try^ x as^ T                       # 誤り。伝播できない
    let^ v = x as^ T catch^ nil^                # 通る。その場で解決
    let^ v = x as^ T catch^ panic^ it^          # 通る。断言（4.1改）
    do^{
        let^ v = try^ x as^ T                   # 通る。腕が食う
    catch^ localerror^.CastFailure:
        …
    }
}
```

最後が通るのは [errors.catch.clause] の裏返しである——腕が取った誤りは外へ抜けない。
どの腕にも当たらなければ抜けるので、そこでこの規則が捕まえる。

#### 走査は名前的な型で止まる［重要］

`return^ { v = try^ … }` のように**内側に紛れる**形も落ちる。走査は合併・
タプルの位置・`t^{}` のフィールド・サブルーチンの結果まで潜る。

ただし **`def^`・誤りの種別・ホスト型は原子として扱う**（[errors.kind.nominal] 、[syntax.obj.name]）。
潜れば `cause` に行き当たり、[errors.kind.made] のとおりそれは両方の族を受けるので、
**すべての誤り種別が「伝播禁止に触れている」ことになって何も返せなくなる**。

原子にした穴は、書く側で閉じる。

> **`localerror^` を `def^`・`errordef^`・`localerrordef^` のフィールドの
> 型に書けない。**

引数の型には書ける。受け取った側も伝播できないので下へ渡ることは規則を
緩めず、そして引数は呼ぶたびに書き手が明示している。

#### `finally^` との関係［補足］

[errors.cleanup] が既に同じ性質を持つ枠を1つ持っている——`finally^` の中で受け取った
誤りは外へ伝播させる手段がない。**そこでは位置の性質だったものを、
ここでは種別の性質にした**。新しい概念ではない。

#### 逃げ道は連鎖ひとつ

[errors.chain] の `cause` に包んで返すことが、**伝播禁止の誤りを解決する唯一の
正規の出口**である。自分の誤りに包むことが「その場で解決した」ことであり、
原因は残る。

```lhat
let^ v = x as^ T catch^ error^ConfigError.Invalid{ cause = it^ }
```

`any^` に広げれば静的には見えなくなるが、[pipeline.repl.no-fork] のとおり実行時の追跡は
しない。明示的に広げた書き手の責任である。

---

## 3. 失敗しうる手続きの型 [errors.fallible]

### 3.1 合併で書く [errors.fallible.union]

失敗しうる操作は、成功時の型と誤りの型の合併を返す。
[syntax.op.precedence] の `|` をそのまま使う。新しい型構成子は要らない。

```lhat
f^ open(p:string^) -> file^|IOError;               # IOError のいずれか
p^ read(p:string^) -> string^|IOError.NotFound;    # これしか返さない
f^ risky() -> number^|error^;                      # どの誤りでもよい
p^ save(x:number^) -> IOError|nil^;
```

4つ目のように、**返す値がなく失敗だけしうる** 操作は誤りと `nil^` の合併を返す。
成功が `nil^` である。

多値を返す操作も同じ形で書く（[syntax.type.tuple]）。
タプルの隣に立てる選択肢は誤りと `nil^` — どちらも頭スロットのタグで判別でき、
判別する構文（`try^`/`catch^`、ループの終了判定）がある。

```lhat
f^ divmod(a:number^, b:number^) -> (number^, number^)|DivError;
c^{ f^-> (K, V)}               # テーブルの走査。resume は (K, V)|nil^
```

**返しうる誤りを署名に書けることが [errors.exhaustive] を成り立たせている。**
Zig の error set にあたるものが、既にある記法だけで表せる。

### 3.2 略記を設けない [errors.fallible.no-shorthand]

Zig の `!T` にあたる略記は **設けない**。`file^|IOError` と書く。

[syntax.obj.structure] が「`typeof^(x).signature` の出力は、そのまま型注釈として解析できる」と定めた。
型を書き表す言語が2つあると、この要請に条件が付く。
`|` だけで足りている以上、増やす理由がない。

`nil^` 側との釣り合いも保たれる。
`number^|nil^` に略記がないのだから、こちらにもない。

---

## 4. `catch^` — その場で代替する [errors.catch]

### 4.1 形 [errors.catch.form]

```lhat
let^ n = parse(s) catch^ 0
```

左の式が誤りを返したとき、**全体の値は右の式の値になる**。
誤りでなければ、全体の値は左の値から誤りを除いた型になる。

```lhat
let^ n = parse(s) catch^ 0        # n : number^
```

`catch^` は二項演算子であり、右辺は式である。文の並びは書けない。

左の式が失敗しえない場合、`catch^` は誤りとする。書いても働かないためである。

これは前置の `try^` とは区別する。`try^` はエラーを含まない式にも書け、
その場合は値も型もそのまま通す。`try^ 42` は `number^` の `42` である。
`try^ 42 catch^ 0` は左辺がエラーを返さないので、`catch^` が誤りになる。

型未決定は「エラーだけを返す」ことを意味しない。未宣言キーの参照などで
成功時の型が分からない場合、`try^`・`catch^`・`??` で失敗や欠落を扱っても
成功側の未決定部分は残る。例えば `let^ e = {aaa = "aaaa"}` に続く
`let^ i = try^ e[0] catch^ 0` は、右辺の `0` だけから `number^` とは推論せず、
strict では束縛の型未決定を報告する。さらに `try^` を正常に通過した値は
エラーを含まないため、外側の `catch^` は strict / relaxed ともに拒否する。
この「エラー除去済み」の情報は型未決定でも保持し、束縛を経由しても伝える。

#### 4.1+ `panic^` だけは書ける［重要］ [errors.catch.panic]

> **`catch^` の右辺に限り、`panic^ 式` を書ける。**

```lhat
let^ v = x as^ T catch^ panic^ it^      # 全体の型は T
let^ n = parse(s) catch^ panic^ "conf が壊れている"
```

全体の型は**左から誤りの腕を除いたもの**である。`panic^` は値を答えないので
合併に加わる腕が無い。[syntax.op.as-error] の断言がこの形になる。

`panic^` は **値を答えず、かつ戻ってこない**唯一の文である（[syntax.with.no-fail]）。
だから値が求められている場所に置いても穴が空かない。多値を返す操作でも
同じで、幅は問われない——腕は列に何も書かずに走行を終える。

**`return^` は認めない。** `x catch^ return^ 0` は書けそうに見えるが、
[errors.catch.clause] の `catch^` 節が**まさにそのために在る**（「代替値が無く、返す前に処理を
挟みたいとき」）。認めれば [errors.catch.clause] の存在理由を掘り崩す。

**`??` にも及ぼさない。** [syntax.op.semantic] が「欠落は誤りではない」と定めているので、
`?? panic^` は「無いこと」を異常と宣言する別種の主張になる。`catch^` の
右辺は [errors.catch.it] が `it^` を束ねている場所であり、`panic^ it^` はそこでしか
自然にならない。

#### 多値を返す操作にも `catch^` を書ける［確定］

```lhat
var^ q, r = divmod(7, 0) catch^ (0, 1)   # 誤りなら (0, 1) を受ける
var^ q, r = divmod(7, 0) catch^ 0        # 誤り。幅が合わない
var^ q, r = try^ divmod(7, 0)            # 呼び出し元へ渡す（5 章）
```

[syntax.type.tuple] の **タプルリテラル** が代替を書く形である。
左が答える型と同じ幅でなければならない——`(A, B)|(A, B)` は
合併が潰れて `(A, B)` になり、幅が違えば潰れずに残って誤りになる。

両腕は **1つの列を共有する**。左の答えを列に置くと、
頭スロットのタグが誤りか列かを判別し、`ISERROR` は `try^` と同じものを読む。
誤りの腕は右辺の位置を同じスロットへ書く。
**どちらの腕も確保しない。**

##### 括弧は省けない

`return^ a, b` と `yield^ a, b` が括弧なしで書けるのは **文** だからで、
その後ろに `,` を使う構文が無い。`catch^` は式の中にあるので、
`,` は外側のリスト区切りと必ず競合する。

```lhat
var^ q, r = f() catch^ 1, 2   # (f() catch^ 1) と 2 の2つ? f() catch^ (1,2)?
return^ f() catch^ 1, 2       # 2値返却? タプル返却?
g(f() catch^ 1, 2)            # 引数2つ? 引数1つ?
```

いずれも決定できない。[syntax.type.semicolon] が引数側で閉じたのと同じ問題であり、
境界を閉じる記号なしには解けない。

##### `try^ it^` は右辺に書ける

`{ … }` を **右辺に** 置いて文を並べる形は無く、`return^` も右辺には
書けない（[errors.catch.panic]）。だから `e catch^ { return^ it^ }` とは書けない。
文を並べて捕まえるのは [errors.catch.clause] の `catch^` 節である。

右辺に書けるのは **`try^ it^`** である。`it^` は誤りだけの型なので、
`try^` はそれを必ず外へ渡し、値を答えない。したがって
**`e catch^ try^ it^` は `try^ e` と同じ動きをする**。[errors.try] の前置の `try^` は、
これを1語で書く記法である。

### 4.2 誤りの名前は `it^` [errors.catch.it]

右辺からは、捕まえた誤りを `it^` で参照できる。

```lhat
let^ n = parse(s) catch^ fallbackFor(it^)
let^ n = parse(s) catch^ if^ it^ fits^ ParseError.Eof: 0 el^: -1 ;
```

[syntax.for.it] が `it^` を「焦点の既定の名前」と定めた。
`catch^` の右辺では誤りが焦点である。同じ語を同じ意味で使う。

名前を付け直す記法は設けない。`catch^` の右辺は式ひとつであり、
`it^` の指すものを見失う長さにならない。

#### 種別ごとに代替し、残りは外へ渡す

妥当な代替値が無い種別もある場合は、パターンマッチ（[syntax.match]）で
種別ごとに分け、残りの選択肢で `try^ it^` と書く。

```lhat
let^ n = parse(s) catch^ for^ it^: when^ fits^ ParseError.Eof: 0 other^: try^ it^ ;
```

`other^` の中の `it^` は、前の選択肢が取らなかった種別に絞り込まれている
（[syntax.match.form]）。`try^ it^` はそれを外へ渡して値を答えないので、全体の型は
成功側と代替値だけになる。`panic^` にはならない（[errors.catch.panic] との違い）。

外側を `try^` で包み、残りを値のまま返しても同じ動きになる。
前置の `try^` は単項で `catch^` より強いので（[errors.catch.precedence]）、括弧が要る。

```lhat
let^ n = try^ (parse(s) catch^ for^ it^: when^ fits^ ParseError.Eof: 0 other^: it^ ;)
```

### 4.3 優先順位 [errors.catch.precedence]

`catch^` は **すべての二項演算子より強く、単項演算子より弱い**。
[syntax.op.table] の表に加える。

```lhat
let^ total = base + parse(s) catch^ 0
#               ~~~~~~~~~~~~~~~~ ここが catch^ の範囲
```

失敗するのは `parse(s)` であって `base + parse(s)` ではない。
**代替値はその操作の値の代わり** なのだから、
`catch^` は周囲の計算にではなく操作に付くべきである。

これを最も弱くする案（Zig の位置）は採らない。
その場合 `base + parse(s)` が先に結合し、
`number^` に `number^|IOError` を足す型の誤りになる。
書いた形と読みが一致しない。

右辺も単項までなので、`f() catch^ 0 + 1` は `(f() catch^ 0) + 1` である。
代替値に計算が要る場合は括弧を書く。

### 4.4 意図的に無視する [errors.catch.ignore]

誤りを捨てる専用の記法は設けない。`catch^` で書ける。

```lhat
save(x) catch^ nil^      # 失敗しても構わない、と明示している
```

Go の `_` にあたるものは**置かない**。[syntax.type.discard] の `_^` は値を捨てる受け皿で
あって、**誤りは捨てられない**（[errors.dropped.answer]）。書いたことにはならず、`catch^` と書く。

```lhat
var^ _^ = save(x)        # 誤り。捨てているのは値ではなく失敗である
```

### 4.5 `catch^` 節 — 文を並べて捕まえる [errors.catch.clause]

> **ブロックの文の後に `catch^` 節（腕）を書くと、その前に書いた `try^` が
> 見つけた誤りは、呼び出し元ではなくその腕へ行く。腕には文を並べられる。**

```lhat
p^ read(p:string^) -> string^ {
    do^{
        with^ h = try^ open(p) {
            return^ try^ h.readAll()
        }
    catch^ IOError.NotFound:
        log($"no such file: {it^.path}")
        return^ ""
    catch^:
        log(it^.message)
        return^ ""
    }
}
```

#### なぜ要るか

[errors.catch.form] のとおり `catch^` の右辺は式ひとつであり、[errors.fits.form] が「記録して続ける・
再試行する・別の誤りに包んで返す」を **合併のまま受けて絞り込む** 側へ回している。
それで足りない場合がひとつ残る——**代替値が無く、返す前に処理を挟みたい**とき。
`with^` の頭のように、値が無ければ先へ進めない位置ではとくにそうなる。

その場合これまでは **無名手続きで囲って即時呼び出し** するほかなかった。
書き心地の問題だけではない。その中の `return^` は無名手続きから出るだけであり、
囲ったことを忘れれば **書いたつもりの脱出が起きない**。

```lhat
let^ line = std.io.readLine() catch^ f^{
    print(it^.message)
    return^ ""          # この return^ はこの f^ から出るだけ
}()
```

腕はこれを構文にする。**腕の中の `return^`・`break^`・`continue^` は
今までどおりの意味** ——ブロックはサブルーチンではないので、脱出先は
それが属する関数・手続き・単位である。

#### どのブロックにも書ける

腕は [syntax.finally.block] の `finally^` と同じく、**ブロック一般の節**である。
置けるのは次の本体である。

- `do^{ … }`
- `if^` 文のブロック。全分岐の本体の `try^` を受ける
- 繰り返し文のブロック（下の「繰り返しでは1回分を受ける」）
- `with^` のブロック
- 手続き `p^` と関数 `f^` の本体

`finally^` と違い、**関数 `f^` の本体にも置ける**。[syntax.finally.not-in-f] が `finally^` を
除いたのは外から観測できないからであり、腕は回復した値を答えるので
観測できる。

```lhat
let^ parseOr0 = f^ s:string^ -> number^ {
    return^ try^ parse(s)
catch^:
    return^ 0
}
```

［補足］[syntax.sub.expr-body] は、本体が式1つなら `return^` を書かずにそれで答えるとした。
腕は本体のもう一つの終わり方なので、式1つの腕も同じくそれで答える。

```lhat
let^ parseOr0 = f^ s:string^ -> number^ { try^ parse(s) catch^: 0 }
```

節の順は [syntax.loop.order] に加わり、腕は `main^` の尾に立つ。

```text
prolog^  →  pre^  →  first^  →  main^  →  catch^  →  last^  →  epilog^  →  finally^
```

繰り返しでないブロックに残るのは本体・腕・`finally^` の順である。

#### 受けるのは波括弧の中の `try^`

腕が受けるのは、**同じ波括弧の中で腕より前に書いた `try^`** である。
構文の頭——`if^` の条件、`with^` の値、`for^` の対象——は波括弧の外に
あるので受けない。頭の失敗も受けるなら、冒頭の例のように `do^{ }` で囲む。

#### `try^` は書く

腕を書いただけでは何も捕まらない。**どの式が誤りを投げうるかは、
書かれた `try^` が言う**。[errors.try] の `try^` の意味は変わらない——変わるのは
行き先だけであり、腕があればその腕へ、無ければ呼び出し元へ行く。

前に `try^` がひとつも無い腕は誤りとする。
[errors.catch.form] が「失敗しえない式への `catch^`」を誤りとするのと同じ線である。

#### 腕

- `catch^ 型:` — [syntax.type.fits] の `fits^` と同じ判定。宣言全体（`IOError`）も、
  合併（`IOError.NotFound|IOError.Denied`）も書ける。種別は名前で書く（[lex.hat.one-use]）
- `catch^:` — 残り全部。**最後に1つだけ**。後ろに腕は書けない
- `it^` は捕まえた誤り（[errors.catch.it] と同じ語）。腕の型に絞り込まれるので、
  その種別が宣言したフィールドが見える（[errors.fits.form]）
- 腕は上から順に問われる。最初に当たった1つだけが走る

#### どの腕にも当たらなかった誤り

**外へ抜ける。** 裸の `try^` を書いたのと同じであり、その誤りは
関数の返り値に加わる（[errors.try.result] ・[pipeline.infer]）。外側のブロックに腕があれば
そちらへ行く。

返り値に含められない誤りは [errors.try.result] がその場で報告するので、
**黙って消えることはない**。全部を受け取りたければ `catch^:` を書く。

#### 繰り返しでは1回分を受ける

繰り返しのブロックでは、腕は `main^` の尾であり、**1回の反復で起きた誤りを
受ける**。腕が済めば次の反復へ進む。腕の中の `break^`・`next^`・`return^` は
書いた場所どおりに働き、`break^` はこの繰り返しを抜ける。

```lhat
for^ line in^ lines {
    total += try^ parse(line)
catch^ ParseError:
    log(it^.message)       # この行だけ飛ばして次へ
}
```

`prolog^`・`pre^`・`first^`・`last^`・`epilog^` に書いた `try^` は受けない。
繰り返し全体を捕まえるなら `do^{ }` で囲む。

#### 腕から見える名前

腕からは、波括弧の外で見えていた名前と `it^` が見える。**そのブロックの文が
束ねた名前は見えない**——腕に来たとき、それが束ねられた後かどうか
分からないためである。`with^` の名前は頭で束ねられるので、`with^` の
ブロックの腕からは見える。

#### 後始末

腕へ行くのは **文の並びからの脱出** であって関数からの脱出ではないので、
それまでに走るのは文の中で開いた後始末だけである。冒頭の例の `h.dispose()` は
腕に入る前に走る。ブロック自身の `finally^` と、腕を書いた `with^` の
`dispose()` は腕の後に走る。[syntax.loop.exit] の `break^` が通り抜けた繰り返しに対して
行うのと同じ引き方である。

#### 二項の `catch^` との見分け

腕の頭は `catch^:` か `catch^ 種別:` の形をしている。二項の `catch^`（[binary.net.api]）が
**文を締めくくる式**——`let^`・`var^` の右辺、`:=` の右辺、`return^` の値、
呼び出し文——の後ろでこの形になることは無い。その後ろに `:` が続く構文が
無いためである。したがって **この形の `catch^` が文を締めくくる位置に来れば腕、
それ以外は二項** と決まり、括弧は要らない。

```lhat
do^{
    let^ n = parse(s) catch^ 0             # 二項。後ろに : が無い
    let^ v = try^ fetch(n)
catch^:                                    # 腕
    return^ 0
}
```

構文の頭は、自分の `:` や `{` が後ろに続くので文を締めくくる位置ではない。
そこでは `catch^ 名前:` の形でも二項である。

```lhat
let^ k = f^ { if^ ready() catch^ fallback: 1 el^: 2 ; }   # 条件の中の二項
```

---

## 5. `try^` — 外へ渡す [errors.try]

### 5.1 形 [errors.try.form]

```lhat
let^ h = try^ open(p)
```

左の式が誤りを返したとき、**その誤りをそのまま今の手続きの返り値として返す**。
後ろに `catch^` 節があれば、呼び出し元ではなくその腕へ渡す（[errors.catch.clause]）。
誤りでなければ、`try^` 全体の値は誤りを除いた型になる。

```lhat
p^ read(p:string^) -> string^|IOError {
    with^ h = try^ open(p) {
        return^ try^ h.readAll()
    }
}
```

`with^`（[syntax.with]）と組み合わさる。`try^` が誤りを返して抜けた場合も
`h.dispose()` は呼ばれる。[errors.try.exit] のとおり `return^` と同じ側の脱出だからである。

`finally^` を直接書く場合は、[syntax.loop.order] が定める順序に従い **最後に置く**。
`finally^:` より後ろに書いた文はその節の本体になる。

```lhat
p^ read(p:string^) -> string^|IOError {
    let^ h = try^ open(p)
    return^ try^ h.readAll()
    finally^: h.dispose()
}
```

`try^ e` は `e catch^ try^ it^` と同じ動きをする（[errors.catch.form]）。
前置の1語で書けるよう、記法として設ける。

値を捨ててよい場合は文としても書ける。

```lhat
try^ save(x)
```

`try^` は単項演算子であり、[syntax.op.table] の単項の水準に置く。
`try^ f() + 1` は `(try^ f()) + 1` である。

### 5.2 `return^` と同じ側の脱出 [errors.try.exit]

[syntax.loop.exit] が既にこの場合を予告している。

> そうした脱出は `return^` と同じ側に属し、`finally^` のみを実行する。

したがって `try^` による離脱は次のように振る舞う。

| 脱出の仕方 | `last^` | `epilog^` | `finally^` |
| --- | --- | --- | --- |
| `try^` が誤りを返した | 実行しない | 実行しない | 実行 |

繰り返しの中で `try^` が働いた場合、`last^` も `epilog^` も走らない。
繰り返しにとって正常な終了ではないためである。

### 5.3 返り値が誤りを含むこと [errors.try.result]

`try^` は、**今の手続きの返り値の型が、その誤りを含む場合にのみ書ける**。

```lhat
p^ read(p:string^) -> string^|IOError {
    let^ h = try^ open(p)      # open が返すのは IOError のいずれか。含まれる
}

p^ read(p:string^) -> string^|IOError.NotFound {
    let^ h = try^ open(p)      # 誤り。IOError.Denied を返せない
}
```

報告は `try^` を書いた場所で行う。[syntax.obj.override] 、[lex.ambig.call-line] と同じ原則である。
返せない誤りを返そうとしているのは `try^` の側であり、
呼び出し元が悪いわけではない。

診断では代替を示す。

```text
try^ が返そうとする IOError.Denied は、この手続きの返り値に含まれていない。
返り値を string^|IOError にするか、catch^ でここで処理する。
```

**この検査は [pipeline.strict.per-unit] で署名に誤りの集合を書けることの帰結である。**
どの誤りが素通りするかが署名から決まるため、その場で確かめられる。

#### 返り値型を書いていないとき

問う相手が無いので、**通した誤りが推論した返り値型に加わる**（[pipeline.infer]）。
`try^` は `return^` と同じ脱出であり、書かなければ推論が拾う、
書けばこの節が確かめる、という関係になる。

```lhat
let^ read = f^ { return^ try^ open() }     # number^|IOError に推論される
```

**伝播禁止の種別はここを通らない**（[errors.kind.local-no-propagate]）。書いた返り値型が無いことは
逃げ道にならず、推論が拾おうとした所で拒まれる。

---

## 6. `fits^` による判別と絞り込み [errors.fits]

### 6.1 形 [errors.fits.form]

`catch^` の右辺は式ひとつなので、
記録して続ける・再試行する・別の誤りに包んで返すといった処理は書けない。
その場合は合併のまま受けて絞り込むか、[errors.catch.clause] の `catch^` 節で捕まえる。
**誤りでない値のほうを続けて使う**なら絞り込みが読みやすい。

```lhat
let^ r = parse(s)
if^ r fits^ ParseError.Syntax {
    report(r.line, r.column)      # 絞り込まれているのでフィールドが見える
    return^ r
}
# ここでは r : number^|ParseError.Eof
```

`fits^` の意味と絞り込みの規則は [syntax.type.fits] で定めた。
右辺には型を書き、`T` と書かれた位置に左辺を置けるかを問う。
完全一致を問う演算子は設けない。

**種別が型であること（[errors.kind.made]）と、種別が互いに素であること（[errors.kind.disjoint]）から、
この判定は常に決定できる。**

絞り込みにより、宣言したフィールドが見えるようになる。
`ParseError.Syntax` と分かって初めて `.line` があると言えるためである。

### 6.2 3つの使い分け [errors.fits.three]

| 書きたいこと | 記法 |
| --- | --- |
| 代替値で続ける | `expr catch^ v` |
| 呼び出し元へ返す | `try^ expr` |
| 文を並べて捕まえる | ブロックの尾の `catch^ 型: …` `catch^: …`（[errors.catch.clause]） |
| それ以外 | 合併のまま受けて `fits^` で絞り込む |

上の2つは式であり、3つ目はブロックの節である。合併のまま受ける形は、
**誤りでない値のほうを** 続けて使う場合に読みやすい。

---

## 7. 網羅性 [errors.exhaustive]

「この手続きが返しうる誤りをすべて処理したか」を検査できる。

[errors.fallible.union] により署名に誤りの集合が書かれているので、
**特別な機構は要らない。合併の絞り込みがそのまま網羅性の検査になる。**

```lhat
f^ open(p:string^) -> file^|IOError;

let^ r = open(p)                        # r : file^|IOError.NotFound|IOError.Denied

if^ r fits^ IOError.NotFound {
    create(p)
    elseif^ r fits^ IOError.Denied:
        fallback()
    else^:
        use(r)
}
```

両方の種別を尽くしたので、最後の節では `r : file^` である。
尽くしていなければ、`r` を `file^` として使う場所で型の誤りになる（[syntax.stmt]）。

Zig の error set にあたるものを、
**総称型なしで** 実現している。合併と部分型だけで足りる。

種別を **値**（列挙体の要素など）として持たせる形では、これが成立しない。
署名に書ける情報が `kind : 何らかの列挙体の要素` までで止まり、
どの種別が返りうるかを型に載せられないためである。
径数化された誤り型が必要になり、L^ に総称型がないため行き止まりになる。
**種別を型にしたことが、この行き止まりを消している。**

---

## 8. 取りこぼしの検出 [errors.dropped]

### 8.1 新しい機構は要らない［重要］ [errors.dropped.no-new]

**型検査がそのまま検出になる。**

```lhat
let^ n = parse(s)      # n : number^|ParseError.Syntax|ParseError.Eof
let^ m = n + 1         # 誤り。number^ の位置に誤りを含む合併がある
```

Rust の `#[must_use]` にあたる仕組みは要らない。
誤りを処理しなければ、値が使えないだけである。

### 8.2 誤りは値の周りに置く［重要］ [errors.dropped.around-value]

Go の典型的な失敗は、`v, err := f()` の `err` を無視できることである。
静的検査では捕まらず、専用の検査器が後付けで警告している。

**L^ にはこの形が存在しない。**

> 誤りは、別に返る値ではなく、**必要な値そのものの中にある**。

これを保つ規則が次である。

> **誤りは値の周りに置き、値の間には置かない。**

```lhat
f^ divmod(a:number^, b:number^) -> (number^, number^)|DivError;   # 書ける
f^ divmod(a:number^, b:number^) -> (number^, DivError);           # 誤り
```

書けないのは2つ目の形である。
`(A, B)` の位置に誤りの型を書くこと——直接でも、その位置の合併に紛れていても
——を静的に拒む。合併の選択肢としての誤りは [syntax.repl.incomplete] のとおり書ける。

値全体が誤りか否かであり、
**傍らに置いて落とせる第2の値が存在しない。**
Rust の `#[must_use]` にあたる仕組みは、[errors.dropped.no-new] のとおり要らない。

### 8.3 答えを落とすと失敗も落ちる［重要］ [errors.dropped.answer]

> **失敗しうる呼び出しの答えを、文として落とすことはできない。
> `_^` に束縛して落とすこともできない。**

[errors.dropped.no-new] の「値が使えないだけ」は、**値が欲しいとき**にしか効かない。

```lhat
std.task.start(1)        # 誤り。答えは number^|OutOfMemory で、誰も見ない
```

`p^` は効果のために呼ぶ形であり、答えは捨てられる。誰も値を欲しがっていないので、
型検査は何も言うことがない。[syntax.stmt.no-bare-expr] が「L^ には存在しない」と言った Go の失敗が、
**第二の返り値ではなく、捨てられた第一の返り値**として戻ってくる。

だから [syntax.stmt.problem] に一行足す。**型が検出になるのは、値が使われるときだけである。**
使われないときは、使われないこと自体を言う。

#### 書き方は既にある

```lhat
try^ std.task.start(1)                 # 呼び出し元へ返す（5 章）
std.task.start(1) catch^ nil^          # 構わない、と明示する（4.4）
std.task.start(1) catch^ panic^ it^    # 失敗したら止まる（4.1改）
let^ r = std.task.start(1)             # 束縛すれば 8.1 が効く
if^ r fits^ number^ { … }
```

#### `_^` も塞ぐ

[syntax.type.discard] の `_^` は値を捨てる受け皿である。**失敗は値ではない**——
[modules.public.let] が「Go の `_` にあたるものが不要」と書いたのはこの意味であり、
`var^ _^ = save(x)` は書いたことにならない。

手本の Zig も同じ線を引いている。`bar();` は `error: error union is ignored`
であり、`_ = bar();` でも黙らない。**値は捨てられるが誤りは捨てられない**、
捨てるなら `catch` と書け、という一本の規則である。

#### 落ちない場所

- 対話の答えの位置（[pipeline.machine]）。その文が答えであり、値は表示される
- 誤りの腕を持たない答え。`f^ -> number^` も `p^;` も今までどおり落とせる
- 束縛。`let^ r = f()` は落としていない。そこからは [errors.dropped.no-new] が引き継ぐ

---

## 9. 後始末との関係 [errors.cleanup]

[syntax.with.no-fail], [syntax.with.errors] で決着している。ここでは帰結だけを確認する。

- `finally^` の中で失敗しうる操作を呼べば誤り値を受け取るが、
  **それを外へ伝播させる手段がない**。`try^` は使えない（[errors.try.result]）。
  `finally^` は返り値を変更できないため（[syntax.finally.result]）、返り値の型に誤りを足せない
- `dispose()` は誤りを返さない（[syntax.with.no-fail]）ので、
  解放の途中で誤りが起きることはない
- したがって「抑制された例外」「巻き戻し中の失敗」にあたる分類が存在しない

`finally^` の中で誤りを受け取った場合にできるのは、記録するか、無視することである。

```lhat
finally^: flush() catch^ nil^
```

---

## 10. 誤りの連鎖 [errors.chain]

下位の誤りを包んで返す場合、`cause` に元の誤りを入れる。

```lhat
let^ r = parse(s)
if^ r fits^ ParseError {
    return^ error^ConfigError.Invalid{
        message := "config could not be read",
        cause := r,
    }
}
```

`cause` は `error^|nil^` なので、辿る側は `?.` で書ける（[lex.op.list]）。

```lhat
let^ depth = e.cause?.cause?.message
```

連鎖は **包む側が明示的に入れたときだけ** できる。自動では付かない。
自動で付けると、どこまで遡れるかが呼び出しの深さに依存し、実行時にしか分からなくなる。

`cause` の型が `error^` である以上、辿った先の種別は上位型までしか分からない。
必要なら `fits^` で絞り込む。

---

## 11. 実行時の異常 [errors.runtime]

### 11.1 原則 [errors.runtime.principle]

> **実行時に検出される失敗は誤りとして返す。`catch^` で拾えない停止は設けない。**

ただし「実行時の異常」と括られるものは同じ性質ではない。
それぞれ別の答えになり、**そのほとんどは実行時の問いですらない**。

この原則が指しているのは **誤り**（本章が形を定める値）の範囲であって、
[syntax.with.no-fail] が別分類とした **panic**（検査器の約束が実行時に破れたもの）
はそもそも範囲の外にある。`t.foo` が実際には無かった、
演算に想定外の型が渡った、といった panic は依然として `catch^` では
拾えない — [errors.runtime.panic] に記す。

なお実行時の機構そのものが誤り値を返す箇所は無い。
誤り値は `error^…{ }` の生成（プログラムとホスト）だけから生まれる。

### 11.2 ゼロ除算は実数へ広がる [errors.runtime.div-zero]

[syntax.obj.number] が Lua 5.3 の数値模型を採ると定め、[syntax.obj.number-widen] が桁溢れを
「誤りにせず実数へ広げる」と定めた。同じ理由がゼロ除算にも当てはまる。
`//` と `%` は、ゼロを割った時点で整数の計算をやめて実数として計算し直す。
実数の除算は元から失敗しないので、そこから先は `/` と同じ答えになる。

```lhat
a / b        # number^。失敗しない
a // b       # number^。ゼロで割れば inf/nan（実数へ広がる）
a % b        # number^。同じく inf/nan
```

#### 汚染が起きない

誤りを返す操作はその返り値を合併にする。
ゼロ除算がそちらに合流していたら、`//` `%` を使う **すべての式** が
合併に触れることになる。**実数へ広げる方式ならそれが起きない** —
`//` と `%` は常に `number^` のままである。

```lhat
let^ mean = total / count            # 触れない
let^ half = total // count           # ゼロを渡しても number^ のまま
```

これは本章のために作った例外ではなく、[syntax.obj.number-widen] の決定から従っている。

### 11.3 表の添字 [errors.runtime.index]

**「範囲外」という概念が存在しない。** 表は写像であり、鍵があるかないかしかない。

そして構造的型付けにより、2つの記法が**必要な線でちょうど分かれている**。

- **`t.foo`** — 静的なメンバ。型検査器が構造を知っているので `T` である。
  型にないメンバを書けば **静的な誤り** になる。実行時の問いにならない
- **`t[k]`** — 動的な鍵。`T|nil^` である

```lhat
t.foo        # T。静的に解決する
t[k]         # T|nil^
let^ v = t[k] ?? 0     # 欠落したときの既定値は ?? で書く（02 の 11.7）
```

#### `T` は何か

> **その種類の鍵で届くと型が言っているものの合併。**

[syntax.obj] が表を列と写像の両方としているので、**鍵の種類で見る半分が変わる**。

- **数の鍵** — 列の側。並べた位置の型と、[syntax.type.variadic] の可変長の尾
- **文字列の鍵** — 名前つきの側。[syntax.obj] が `t.a` と `t["a"]` を同じ入口としている
- **鍵の型が決まっていない** — どちらもありうるので両方の合併。
  片方に決めるのは「どちらか分かっている」と主張することになる

```lhat
let^ t = { 10, 20, 30 }
t[i]              # number^|nil^
t[2]              # number^。書いた鍵が型の言う位置に当たれば静的な半分

let^ u = { 10, a = "x" }
u[i]              # number^|nil^      i は数
u[s]              # string^|nil^      s は文字列
```

書いた鍵が型の言うメンバに当たればそれは**静的な半分**であり、`nil^` は付かない。
当たらなければ動的な鍵と同じ扱いになる——型がその位置について何も約束していない。

##### 範囲が位置に収まるなら、書いた鍵と同じ

[syntax.type.fits-word] が、分岐の中で名前が取りうる整数の範囲を覚える。
その範囲が型の宣言した位置から出ないなら、鍵が無いことは起こりえない
——[syntax.obj.structure] が「少なくともこれらを持つ」である以上、宣言した位置は
適合する値が必ず持っているためである。

```lhat
let^ f = f^ t:t^{ number^[9] } {
    for^ i from^0 to^8 { print(t[i] + 1) }      # number^
}
```

**静的な半分に届く道がもう1つある**、という形であり、`T|nil^` の規則自体は
変わらない。範囲が分からない鍵は今までどおり `T|nil^` を答える。

##### 型がその種類について何も言っていなければ判断しない［補足］

`t^{}` のように、その鍵で届くものを型が1つも挙げていなければ、
`T` が無いので `T|nil^` も作れない。**検査は何も言わない。**

[syntax.obj.structure] は「少なくともこれらを持つ」であって、値はもっと持ちうる。
だが読み取りの側は上の `t.foo` のとおり**宣言が縛る**——型にないメンバを
読めば誤りである。列の側もそれに揃えている。

素の写像（`var^ c = { }` に文字列の鍵で読み書きする形）をここで誤りにすると
逃げ道が無くなる。列なら `var^ a : t^{ number^[] } = { }` と書けるが、
**名前つきの側を開く綴りが今は無い**。閉じるならその綴りを先に決める必要がある。

##### 書き戻す形は [syntax.stmt.nil-assign] で書く

`t[k] += 1` は読む側に `nil^` が混じるため書けない。
数え上げのように読んで書き戻す形は `?+=` を使う。

```lhat
count[k] ?+= 1        # 在れば数える。無ければ何もしない
```

#### 欠落は失敗ではない

`t[k]` が誤りを返さないのは、**鍵がないことが写像に問うたことの普通の結果** だからである。
失敗と呼ぶと、存在を確かめるだけの問い合わせが毎回誤り値を作ることになる。

Lua の振る舞いを型に載せた形にあたる。

#### Lua にない利点

Lua では `t.foo` も実行時の索引であり、綴りを誤れば `nil` になる。
L^ は構造的型付けを採るため **`t.foo` が静的に解決する**。

その結果、`nil^` との合併に触れるのは
**本当に動的な添字だけ** に閉じ込められる。

### 11.4 `nil^` の参照 [errors.runtime.nil]

strict では **静的に防がれる**。`x : T|nil^` に対する `x.foo` は型の誤りであり、
`fits^` で絞り込む（[syntax.type.fits]）か `?.` を書く（[syntax.op.nil-safe]）しかない。
**検査されたコードには、実行時検査が残らない。**

relaxed（[pipeline.strict.per-unit]）では `T|nil^` への参照が **そのまま通る**
——検査器は退き、実際に `nil^` が来ていれば仮想機械の命令が
その場所で **panic する**（[errors.runtime.panic] のとおり行番号と演算子名を持って返る）。

relaxed には検査挿入がない（[pipeline.strict.relaxed]）。挿入検査がないなら、
`nil^` を踏むのは「静的な約束が破れた」側ではなく、
単に **動的なコードが動的に失敗した** だけであり、それは他のあらゆる
型不一致と同じく命令の panic が答える。専用の誤り種別も、そのための
検査も、設けない。`nil^` を予期するコードは `??` か `fits^ nil^` か
`?.` で **書き手が** 訊く——それが [syntax.op.semantic] との一貫でもある：
予期される欠落は書き手が問い、予期しない欠落は panic する。

### 11.5 費用 [errors.runtime.cost]

[errors.runtime.principle] の選択による実行時の費用は小さい。内訳は次のとおり。

- **`//` と `%`** — 除算の前に比較と分岐が1つ増える。
  整数除算は数十サイクルを要し、予測の当たる分岐は実質ゼロに近い。
  **除算自体に埋もれる**
- **`t[k]`** — 検査は **既に存在する**。
  配列部か連想部かの判定と境界の確認は、返す値の種類によらず必要である。
  欠落時に何を返すかを変えても検査の回数は変わらない
- **`nil^` の参照** — 実行時検査そのものが生じない（[errors.runtime.nil] 。relaxed でも挿入せず、踏めば命令の panic）

加えて、L^ はバイトコード仮想機械であり、
**値は既に型タグを持つ**（[pipeline.value.tags]）。合併を返すために箱詰めが増えることはない。
命令ディスパッチの費用に対して、分岐1つは計測に現れない。

誤りを値として返す方式は、例外機構に比べて
**成功する経路にも分岐が残る** という取引を伴う。Go と Zig が同じ取引をしており、
ネイティブ生成に進んだ段階でも許容されている範囲にある。

### 11.6 panic は行番号と演算子名を持って返る [errors.runtime.panic]

[syntax.with.panic] の panic は `catch^` で拾えないが、無言で落ちるわけではない。
`lhat_run` は必ず `LhatRunResult{ status, line, op_name, ... }` を
普通に返す — C レベルで異常終了することはなく、ホストが受け取って
次にどうするかを選ぶ（REPL なら次の入力へ進む、ファイル実行なら
終了する、など）。

- **`line`** — その命令が生まれた元のソース行。`LhatChunk.lines[]` という、
  命令列と並ぶ配列（命令ごとに1個、圧縮なし）から引く。列は持たない
  — 実行時の診断は「file:line」程度で十分とする（Lua も列を持たない）。
  検査時の診断（offset/line/column 全部）とは別物で、そちらの精度を
  落とすものではない
- **`op_name`** — 落ちた命令が演算子（[syntax.op.define]: `+` `-` `*` `/` `//` `%`
  `**` `..`）だったときのその綴り。演算子でない命令(呼び出し・添字等)
  では `NULL`

行表は単純な形（命令ごとに1ワード）とする。Lua 5.4 はここを
差分圧縮＋疎な絶対値アンカーで詰めているが、[errors.try.form] の「まず汎用形、
特殊化は後」により、メモリが実測で問題になった時点まで見送る。

#### 11.6+2 panic は finally^ を駆動しない［重要］ [errors.runtime.panic-finally]

> **fault と panic^ は後始末を走らせない。** どちらも巻き戻さずその場で
> 返り、フレームは立ったまま残る（下のトレースバックはそれを読む）。
> `finally^` が走るのは値の経路——`return^`・`try^`・yield の終端——
> だけである。

**これは既定の二層の下側である。** 他言語も同じ線を引いている:
回復できる層（Go の `panic`/`defer`、Rust の unwind と Drop、C++/Java/
C#/Python/JS の例外と finally、Lua の `error` と `<close>`）では後始末が
走り、**致命の層では走らない**（Go の runtime fatal throw、Rust の
`panic = abort` と二重パニック、C# の `StackOverflowException` と
`FailFast`、C++ の捕まらなかった例外の `terminate`、Lua の
`lua_atpanic`）。

L^ が上側の層を**巻き戻しではなく値**で持っている（[modules.require.load]）だけで、
位置は同じである。誤りは値なので `finally^` は普通に走る。
`panic^` は [syntax.with.panic] が `catch^` から外した——拾えないものの後始末を
言語が代わりに走らせる筋はない。

**資源は漏れない。** ホストのものは [modules.hostdata] の三契機（明示の
`dispose()`・回収・機械の破棄）で必ず返る。走らないのは `finally^` に
書いた L^ 側の後始末だけであり、それは「致命の後は何も信用しない」と
いう宣言そのものである。

**走らせる案を採らない理由**もここに書いておく。倒れたフレームの
`finally^` を drain するなら、その本体もまた L^ のコードであり、
**その最中にまた落ちたらどうするか**（Go の二重パニック問題）を
背負い込む。踏んだ場所を保存したまま返すほうが、トレースバック
（[errors.runtime.traceback]）にとっても素直である。

#### 11.6+ トレースバック [errors.runtime.traceback]

fault は巻き戻さないので、**フレーム配列は落ちた瞬間のまま残っている**。
機械はそれを次の実行（または破棄）まで読ませる:

```c
size_t lhat_machine_fault_depth(const LhatMachine *);
bool   lhat_machine_fault_frame(const LhatMachine *, size_t level,
                                LhatFrameInfo *out);  // 0 = 最内
size_t lhat_machine_traceback(const LhatMachine *, char *out, size_t cap);
```

- `LhatFrameInfo` は `source`（単位のパス）・`name`（束縛/メンバ名）・
  `line`・`top_level`・`coroutine`・`disposing`。粒度は [errors.runtime.panic] のまま
  **file:line、列なし**
- `source` と `name` は**コンパイル時に proto へ刻む debug ラベル**。
  [syntax.obj.name] はそのまま — どちらも型にも同一性にも一切参加しない。
  無名のラムダは `f^`、セッションの入力は `source` 無しと表示される
- fault の記録が無いときは**いま立っているフレーム**を答える —
  ホスト関数が「自分はどこで呼ばれたか」を訊く形で、
  `std.debug.traceback` はこれに乗る
- 入れ子（LhatHostFn の中の `lhat_machine_call` が落ちた場合）は
  **そのホスト関数が戻る前に**読む — 外の実行が積み直せば上書きされる
- **末尾呼び出しは痕跡を残さない** — フレームを取って代わるので、
  取られた側はトレースに現れない（Lua が `(...tail calls...)` と省略
  するのと同じ穴。印を持たせるかは見送り）
- 文字列形は1フレーム1行・最内から:

```text
traceback:
  main.lh:12: in gen (coroutine)
  main.lh:40: at the top level
```

［補足］最内フレームの行はそのフレームの保存された `pc` から引くので、
ホスト境界をまたぐ前に `pc` を書く（さもないと `std.debug.traceback` を
ホストから読むと最内行が一つ前の呼び出しにずれる）。この API はデバッガの
フックの中でも同じフレームを読む（[09-debugger.md](09-debugger.md) [errors.fallible.union] 、[errors.kind.disjoint]）。

エラー**値**（error^）は今までどおり何も刻まない — [errors.chain] の「連鎖は
明示だけ」と同じ線で、欲しければ `std.debug.traceback()` を message に
書き込む。値の出自を機構で問うかは 05 の M4 と一緒に別途。

#### `panic^` はプログラム自身が起こす panic

[syntax.with.panic] に書いたとおり、検査器が検出する panic の他に
`panic^ EXPR` という文がある。Zig の `@panic()`、Rust の `panic!()` と
同じ位置づけ — 「ここへは来ないはず」を書いた本人が言明する。

```lhat
panic^ "実装されていない経路"
```

`EXPR` は `any^`（文字列に限らない）。`LHAT_BC_PANIC` は他の panic と
まったく同じ経路（`finish()` → `LhatRunResult{ status = LHAT_RUN_PANIC,
line, value }`）を通る。他の panic は `value` を常に `nil^` で捨てるが、
これだけは **書いた値がそのまま乗る** — メッセージは検査器の知らない
ことなので、行番号や演算子名のような機械的な情報では足りない。

値を答えないので `return^`/`break^` と同じ「戻ってこない」文であり、
`always_exits`（[pipeline.infer] の網羅性判定）もその2つと同列に扱う —
`panic^` だけで終わる `f^` の本体は「値を返さないまま抜ける」エラーに
ならない。

なお本節が費用として扱っているのは実行時の話であり、
**設計上の費用は返り値が合併になること** のほうが大きい。
[syntax.op.concat] と [syntax.op.semantic] がその汚染を限定しているのは、そちらが本題だからである。

---

## 12. 文法のまとめ [errors.grammar]

### 12.1 追加する語 [errors.grammar.words]

`errordef^`、`localerrordef^`、`try^`、`catch^`。どれも [lex.hat.no-reserved] により、
**字句解析器には何も足さない**。`^` を伴う語は既に1種のトークンであり、
意味を知るのは構文解析器だけである。

### 12.2 形 [errors.grammar.forms]

```text
文 := errordef^ 名前 { 種別, … }
    | localerrordef^ 名前 { 種別, … }   # 2.7。中身の綴りは同じ
    | try^ 呼び出し                # 5.1。値を捨てる場合

本体 := { 文… 腕… 節… }            # 4.5。腕は main^ の尾、finally^ より前

腕 := catch^ 種別名 : 文…          # 4.5。上から順に問う。合併は | でつなぐ
    | catch^ : 文…                 # 残り全部。最後に1つだけ

種別 := 名前
      | 名前 { 名前 : 型, … }      # 付随情報を宣言する場合

式 := 式 catch^ 式                 # 4 章
    | try^ 式                      # 5 章
    | error^ 修飾名 { 要素, … }    # 2.5
    | new^ 型 { 要素, … }          # 2.5、02 の 14.11
```

### 12.3 優先順位 [errors.grammar.precedence]

[syntax.op.table] の表に2行が加わる。弱い順に、

- `catch^` — `as^` より強く、単項より弱い。左結合
- `try^` — 単項の水準

[errors.catch.clause] の腕は節なので優先順位を持たない。腕の形の `catch^` が文を締めくくる
位置に来たときだけ腕を開き、それ以外は二項である。

### 12.4 型 [errors.grammar.types]

```text
型 := … | error^ | localerror^ | 種別の集合の名前 | 修飾された種別名
```

```lhat
error^                # 伝播できる誤りならどれでもよい
localerror^           # 2.7 のもう一方。error^ とは互いに素
IOError               # IOError のいずれか
IOError.NotFound      # この種別
```

`localerror^` に触れる型を返り値型・yield 型・宣言されたフィールドの型に
書くことは [errors.kind.local-no-propagate] が拒む。書ける場所は引数の型・局所の注釈・`catch^` の腕である。

修飾された名前を型として書けることが要る。[syntax.type] の型の文法に加わる。

### 12.5 [syntax.obj.def-form] の族との関係 [errors.grammar.def-family]

[syntax.obj.template] の `self^{ … }` は
「組み込みの識別子に `{ … }` が直に続く形」である。

`error^ 修飾名 { … }` は **この族ではない**。間に名前が入る。
構文解析器は `error^` の次のトークンを見るだけで区別できる。

- `error^` の次が名前 — 生成（[errors.kind.new]）
- `error^` の次が `{` — 誤り。種別を書くよう促す
- 型の位置の `error^` — 上位型（[syntax.type.param-names]）

---

## 13. 未決事項 [errors.open]

**現在なし。** 決定済みの番号は欠番とする。

---

## 改定履歴（要約）

- **[errors.catch.clause]（2026-09-19）: `catch^` 節はブロックの節。** `try^{ … }` を廃し、
  腕は `finally^` と同じくブロックの尾に書く節にした（`do^`・`if^`・繰り返し・
  `with^`・`p^` と `f^` の本体）。前置の `try^` と囲みの `try^{` が同じ語を
  2つの意味で使っていたため。腕と二項の `catch^` は `:` で分かれる——文を
  締めくくる式の後ろに `:` が続く構文は無い。見分けられないとして置いた
  括弧の規則は撤廃した（分けられなかったのは、構文の頭を文の式と同じ深さで
  読んでいた誤りと、かつて存在した `f^:` の式形のためで、後者は廃止）。
  繰り返しでは反復ごとに受け、頭の `try^` は受けない。腕から本体の名前は
  見えず、`finally^` は腕の後に走る。[binary.net.api] の [errors.try.form] の説明を `e catch^ try^ it^` に
  改め、[errors.catch.it] に残りの種別を外へ渡す書き方を足した
- **[errors.dropped.answer]（2026-09-05）: 答えを落とすと失敗も落ちる。** [lex.scope.step] は「型検査がそのまま
  検出になる」と書いたが、その論証は**値が欲しいとき**にしか効かない。`p^` を
  効果のために呼んで答えを捨てると、[modules.host.initial] が「存在しない」と言ったはずの
  「無視できる誤り」が、第二の返り値ではなく**捨てられた第一の返り値**として
  戻っていた。`std.task.start(1)` がそれで、実際に通っていた。文として落とすのも
  `_^` に束縛するのも拒む。[errors.catch.ignore] の「Go の `_` にあたるものが不要」は
  「`_^` では誤りを捨てられない」の意味だと明記した。手本の Zig が
  `error union is ignored` で塞いでいるのと同じ線

- **[errors.catch.panic]（2026-08-26）: `catch^` の右辺に `panic^` を許す。**
  [syntax.op.as-error] で `as^` が合併を答えるようになり、断言（旧 `as^`）が [syntax.type.fits] の
  絞り込みで2行になっていた。`panic^` は**値を答えず、かつ戻ってこない**
  唯一の文なので、値の求められる位置に置いても穴が空かない——それだけを
  例外として認めた。全体の型は左から誤りの腕を除いたものになり、それが
  そのまま断言になる。`return^` は認めない（[errors.catch.clause] のブロック末尾の `catch^` 節が
  まさにその用途であり、掘り崩すことになる）。`??` にも及ぼさない
  （[syntax.op.semantic] の「欠落は誤りではない」により別種の主張になる）

- **[errors.kind.local] ・[errors.kind.local-no-propagate]（2026-08-26）: `localerror^` — 呼び出し元へ返せない誤りの
  一族。** `errordef^` に対する `localerrordef^` を足し、その宣言する種別を
  **`error^` とは互いに素な第二の頂点**の下に置いた。強制は
  「`localerror^` に触れる型は返り値型にも yield 型にも現れてはならない」
  の1本で、[pipeline.bc.call] の関門の手前に一段置くだけで足りる——署名を広げる逃げ道は
  署名自体が拒まれることで塞がる。頂点を分けたのは、`error^` の下に置くと
  `-> T|error^` が含みうることになり `error^` 自体と `cause` が返せなく
  なるためである。走査は名前的な型で止め（`cause` が両族を受けるため潜れ
  ない）、代わりに宣言されたフィールドの型に書くことを禁じた。
  [errors.cleanup] が `finally^` について既に持っていた「その場で解決するしかない枠」を、
  位置の性質から種別の性質へ移したもの。
  類例は調べた範囲では無い——Java/Rust/Swift/Nim の「呼び出し元の宣言で
  そうなる」形は属性の持ち主が関数であり、Midori の abandonment ほかは
  「回復可能か」で切っていて「回復可能だがここだけ」の位が無い

- **[errors.runtime.traceback]（2026-08-20）: トレースバック API。** fault が巻き戻さない
  設計をそのまま資産化 — フレーム配列を次の実行まで読ませる3本の C API
  と、コンパイル時に proto へ刻む debug ラベル（[syntax.obj.name] の線は不動）。
  **[errors.runtime.panic-finally] として「panic は finally^ を駆動しない」を正式な仕様に**
  （既定の二層の下側。他言語も致命の層では走らせない——Go の fatal
  throw、C# の FailFast、C++ の terminate ほか。ホスト資源は [modules.hostdata]
  の三契機で返るので漏れず、走らせる案は二重パニック問題を背負う）。
  エラー値への出自付与は見送り（[errors.chain] の線・M4 と別途）

- E1〜E8 は決定済みとして本文に取り込み欠番
  （種別は型・署名に合併で書く・`fits^` 絞り込み・停止は設けない・
  フィールド既定値・階層なし・`??`・多値への `catch^`）
- 種別の表現は「テーブル」→「`kind : string^`」→「汎用列挙体の要素」を経て
  **宣言された型**（[debug.hook]）に決着。値では網羅性が型に載らないため。
  汎用の列挙体 `enum^` も用途消滅により導入せず——のち [syntax.match.other]（bool
  網羅）が「公称宣言なら値照合でも網羅性は型に載る」前例を作ったため、
  2026-09-01 に [syntax.enum] として再導入。errordef^ は構築時フィールドを
  持つ和型としてこの文書の領分に残る
- ゼロ除算は当初 `//` `%` が誤り値（`ArithError.DivideByZero`）を返す設計だったが、
  [syntax.obj.number-widen]（桁溢れは実数へ広がる）と同じ手で解消し撤回（[syntax.op.concat]）。
  その検討で設計した「言語コア組み込みの誤り種別」の経路
  （プロセスで1回だけ確保し破棄しない専用ヒープ＋`resolve_kind` への特別な1件）は
  現在客がいないが設計としては有効。必要になった時点で起こす
- relaxed の `nil^` 参照は当初「誤り値を返す」だったが、[pipeline.strict.relaxed]
  （relaxed への検査挿入の撤回）に伴い命令の panic に改めた（[errors.runtime.nil]）
- 誤りのフィールドの綴りは当初 `:=` のみ → [syntax.obj.keys] で `=` 推奨・`:=` 許容に統一
- 腕が全部種別の合併も `message` と `cause` を答えることを明記（[errors.kind.made]）。
  構造型とは比べないことも併記——公称であることは [errors.kind.nominal] のまま
- 返り値型を書いていない本体では、`try^` が通した誤りが推論した返り値型に
  加わることを明記（[pipeline.bc.call] 、[pipeline.infer]）。[pipeline.bc.call] の検査は書いた場合の話である
- 旧4.5で `try^{ … catch^ 型: … catch^: … }` を追加した（[errors.catch.clause] で廃止し、
  ブロック末尾の `catch^` 節へ移行）。文を並べて捕まえる形が
  無かったため、無名手続きで囲って即時呼び出しするほかなく、その中の
  `return^` が囲いから出るだけという取り違えが起きていた。ブロックの中の
  `return^` は今までどおり関数・手続き・単位から出る。腕に当たらなかった
  誤りは裸の `try^` と同じく外へ抜ける。本体と腕の直下では `catch^` が
  腕を開く語になるので、そこで二項の `catch^` を書くなら括弧で囲む
- [syntax.op.semantic] の `T` が何であるかを書き下した（「`T` は何か」新設）。その種類の鍵で届くと
  型が言っているものの合併であり、[syntax.obj] が表を列と写像の両方にしている以上
  数の鍵と文字列の鍵では見る半分が変わる。型がその種類について何も挙げていなければ
  `T` が無いので判断しない——名前つきの側を開く綴りが無く、閉じると素の写像が
  書けなくなるため。規定そのものは変わっていない
- [syntax.op.semantic] に「範囲が位置に収まるなら、書いた鍵と同じ」を足した。[syntax.type.fits-word] が
  覚えた整数の範囲が型の宣言した位置から出ないなら、鍵が無いことは起こりえない
  ——静的な半分に届く道がもう1つ増えた形であり、`T|nil^` の規則自体は変わらない
