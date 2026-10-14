# L^ メモ

## 目標

Modern & Better Lua with Visual Programming.

型システムが後付のLuauよりも良いものになってほしい。

適応領域はLuaと同じ組み込み言語。

- あまり受けの良くないPASCAL風の見た目をC風の {} でブロックを示す構文に。
- 配列が1ベースではなく0ベース

ビジュアルエディターのシリアライズフォーマットがプログラミングソースそのものであり、
ビジュアルプログラミングとテキストプログラミングを相互に行き来できる、AIに生成させた
ソースコードを直接ビジュアルエディターで閲覧、修正できる。

## 影響を受けた言語

Lua / Zig / Rust / Go / TypeScript / Python / Luau / Ruby / OCaml / Elixir

## 言語コア

「変数はすべて参照」は撤回。指す値の型によっていかようにも変容する。
効率のために真偽値と数値は値型で、ユーザー定義値型をも導入した今、維持できない方針なので。
ただAIの回答、値型は同一インスタンス、という返答には納得できていないが。
＞ 結果、値型にはis^の適用はエラー、ということになった。オブジェクトの同一性を現状問えないので。

### nil^ の非対称性

nil^は終了時返す値がなかったら代わりに積まれるので、
p^; と p^->nil^; は**全く一緒**である。
しかし、**p^; と p^nil^; は違う**。p(nil^) は p() と違って実際に nil^ をつむ。
p()は実際に積まない。実行時に「引数の型が違う」と言われて落ちるので、呼び出し側は区別する必要がある。

### 多値返却を採用した意義

#### 出力はすべて返り値

```cpp
void Foo(int &blah) { ... }
```

```cs
void Foo(out int blah) { ... }
```

こうした、「引数に返り値が返却されることを示すマーク」みたいなものを導入したくないから。

出力はすべて返り値に、引数は入力、としたいから。

```lhat
let^p = p^t:^t{} { var^t.blah = 1 }
```

こういうのはいくらでもできちゃいますけどね、現状。f^ であれば拒否れるので、十分という判断。

```lhat
let^p = p^mutable^t:^t{} { var^t.blah = 1 }
```

と明示できる、これがなかったらtへの書き込みアクセスは拒否（f^に準ずる）、はありかもだな。

#### Luaにおける問題点

02 §13.8改 で触れられているが、Lua では

```lua
local a = function() return 1, 2 end()
```

とした場合、自動で 2 は切り捨てられて a には 1 が入るという仕様。さらに

```lua
function f() return 1, 2 end
function g() return "a", "b" end
local a, b, c = f(), g()
```

とあったら a = 1, b = "a", c = "b" となる。末尾以外は暗黙に一個に切り詰められる、という仕様。
これが末尾再帰最適化を邪魔したり、非直感的なふるまいでバグの温床になったりしていた。

#### Goの場合

全部を受け取らないとエラー。
L^ でも静的に返り値の数がわかっている場合に返り値の数が合っていなかったらエラー。
ただし実行時は切り捨てもしくはnil埋めが行われる、はず（合ってる？）

また、引数の型と数が合ってれば、そのまま他の関数の引数として渡せる。合ってなかったらエラー。
L^では g(f()...) と引数の全展開を明示する必要があるが、同じ事ができる。

#### 導かれる結論。~~変数定義文で多値代入は不要では？~~ 必要でした

let^a, b, = 1, 2 などは対応しなくよいのでは？
let^a, b, = f() など、多値を返す関数の結果を受ける以外の使い方はできない、でよいのでは。
L^ では

let^a = 1
let^b = 2
do^{
    let^a = b
    let^b = a
}

ができるので…いや、ちがう、これのためにも必要だわ。これを
let^a = 1
let^b = 2
do^{
    let^a, b = b, a
}
と書かないと宣言の右辺で未宣言のL^でもスワップにならないんだから。
~~ただ、a, b, := 1, 2 これを禁止したところで~~
なんでもない。

### 人気のPythonオフサイドルールを採用しなかったのはなぜか？

#### Gemini によるオフサイドルールの問題点まとめ

Pythonのオフサイド・ルール（インデントによる文法ブロック制御）はコードの可読性を高める素晴らしい仕様ですが、ご指摘の通り「タブとスペースの混在」をはじめ、歴史的・実用的にいくつか顕著なデメリットや問題点が指摘され続けています。

1. タブとスペースの混在問題（Python 3での強制解決）

個人で経験された混乱は、Pythonコミュニティ全体でも最大の痛点でした。

過去（Python 2以前）: タブ幅を「スペース8個分」とみなして計算していたため、エディタの表示設定（タブ幅=4等）によっては「見た目では揃っているのに、Pythonからは異なるインデントレベルに見える」という理由で、意図しない挙動や構文エラーが多発しました。

現在（Python 3以降）: 同一ブロック内でタブとスペースを混在させると、実行前に即座に TabError: inconsistent use of tabs and spaces in indentation を吐いて停止する仕様に変更されました。現在はPEP 8に則り「スペース4個」が事実上の世界標準となっています。

1. ネストが深くなった際の視認性の低下

括弧を使う言語（C言語やJavaScript等）では、閉じ括弧 } を見ればブロックの終了が一目で分かりますが、Pythonは「インデントが戻ること」でブロックの終わりを示します。

スクロールによるコンテキスト喪失: 処理が長くなって画面を縦スクロールした際、「今いる行がどの if や for の配下なのか」を見失いやすくなります。

ネスト抜けのバグ: 複数はさまれたインデントから「1段だけ戻す」つもりが「2段戻してしまった」場合でも、構文として成立していればエラーにならず、静かにロジックバグとなります。

1. コードのコピペ・リファクタリング時の崩れ

貼り付け事故: Web上のコードや別のファイルからコードをコピー＆ペーストした際、インデントが数スペース分ずれるだけで動かなくなります。

Diff（差分）の肥大化: 条件分岐を1つ囲む（または外す）だけで、配下の数十行〜数百行のインデントを全て変更する必要があり、Gitなどのバージョン管理で差分（Diff）が無駄に大きくなります。

1. ワンライナーや無名関数（ラムダ式）の機能制限

Pythonの lambda が単一の式（Expression）しか書けず、複数行の文（Statement）を書けない最大の理由は、このインデント構文にあります。

無名関数の中にインデントブロックを持ち込もうとすると、式の中で文法解析（パース）を行うのが非常に困難になるため、言語設計として意図的に制限されています。

1. テンプレートエンジンや動的コード生成との相性の悪さ

HTML内にPythonコードを埋め込むようなWebテンプレートエンジン（Jinja2など）では、インデント制御だけではHTML側の構造と喧嘩するため、結局 {% endif %} や {% endfor %} のような明示的な閉じタグを独自導入せざるを得なくなっています。

現在では、フォーマッタ（RuffやBlackなど）を保存時に自動実行させる運用が定着したため、混在やズレによる事故は大幅に減りました。しかし「ブロックの終わりが視覚的にわかりにくい」「ラムダ式が制限される」といった言語構造に起因する課題は、今も議論の対象となっています。

---

- コードのコピペ・リファクタリング時の崩れ
- ワンライナーや無名関数（ラムダ式）の機能制限
- テンプレートエンジンや動的コード生成との相性の悪さ

これらの点が L^ が GDScript や Nim のように Pythonスタイルを採用しなかった、あるいはできなかった、理由。
おそらく、ビジュアルエディターの実装に深刻な悪影響を及ぼすだろうと考えたから。

### なぜL^はLispの方言にしなかったのか

Pythonオフサイドルールを採用しなかった理由と被る。

Pythonオフサイドルールの問題点として「ブロックの終了を見失う」というのがあるが、Lispの全部普通の丸括弧、というのも同じ問題をはらんでいる。あまりにも視覚的フックがなさすぎて対応を見誤る。

Lispは優れた言語だがあの()と空白だけで区切る見た目だけが問題。そして丸括弧だけからなるS式を手放したら、Lispの美点の半分くらいがなくなってしまう。

### サイレント実装漏れ

#### 多次元配列

``` lhat
let^t = {
    width^ = 3,
    1, 2, 3,
    4, 5, 6,
    7, 8, 9,
}
print(t[1, 2])
```

``` lhat
let^t = {autowidth^:
    1, 2, 3,
    4, 5, 6,
    7, 8, 9,
}
print(t[1, 2])
```

### 課題

### 検討項目

#### hash^ ユーザー定義のハッシュ

これがないと値比較でテーブルのキーにすることができないだろう、けど、
自動化できないか？

#### 静的無限ループを検知してエラーにする

脱出条件のないrepeat、等

#### アクセス指定子

現状、モジュールの公開メンバをマークする `public^` しか存在しない。

発想の元が Lua の全部テーブル、だし、根っこがテーブルである限り難しい、のではないか…？
正直自分は偏執的アクセス制御の厳格化（基本全部privateであるべき、など）には懐疑的だし。
明確な設計意図のない隠蔽は単にコピペプログラミングを増やすだけだと想ってる。

---

結論から言うと、「構造型付け」「private」「平坦な合成」を全部そのまま成立させる言語は少なく、たいていどれかを少し曲げている。解決法は大きく4系統ある。

##### 1. private だけ名目的にする — TypeScript型

TypeScriptは基本的に構造型だが、`private` を含むクラス同士の適合性だけは「同じ宣言から生まれたprivateか」を見る。

```ts
class A {
    private state = 0;
}

class B {
    private state = 0;
}
```

`A` と `B` は公開構造が同じでも互換にならない。privateがクラス固有のブランドとして働くためである。[TypeScript公式](https://www.typescriptlang.org/docs/handbook/type-compatibility)

概念的には次のような型になる。

```text
A = {
    [private A::state] : number
}

B = {
    [private B::state] : number
}
```

文字列 `"state"` ではなく、「宣言Aが所有するstate」という一意な名前として扱う。

ただしTypeScriptの `private` キーワードは実行時には通常のJavaScriptプロパティなので、実体上の衝突までは解決しない。実行時にも分離するのがJavaScriptの `#state` で、異なるクラスの同名privateは完全に別の要素として共存する。[MDN](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Classes/Private_elements)

L^へ持ち込むなら、内部的にこうする案になる。

```text
公開キー:      "value"
非公開キー:    PrivateKey(definition_id, "state")
```

これはかなり筋がよい。構造型付けは公開面に残し、privateだけを名目的なキーにできる。

##### 2. 合成しても平坦化しない — Go型

Goの埋め込みは、見かけ上メンバを昇格させるが、実体は内側の構造体を保持している。

```go
type A struct {
    state int
}

type B struct {
    state string
}

type C struct {
    A
    B
}
```

`C` は概念的には次の形であり、二つの `state` は本当に別の場所にある。

```text
C {
    A { state int }
    B { state string }
}
```

`c.state` は曖昧なので使えないが、`c.A.state` と `c.B.state` なら区別できる。埋め込みの同じ深さに同名があればセレクタを曖昧にするのがGoの基本方針である。[Go言語仕様](https://go.dev/ref/spec)、[Effective Go](https://go.dev/doc/effective_go)

また、Goでは異なるパッケージの非公開フィールド名は、綴りが同じでも型同一性上は別名として扱われる。ここにも「非公開名へ宣言元の識別子を加える」発想がある。

これはL^の現在の合成と対照的。

```lhat
let^ C = A .. B
```

現在のL^はAとBのインスタンス領域を一つの平坦な表へ展開する。Go方式にするなら、内部的には次のように区画を残す必要がある。

```text
C {
    hidden A-part { state }
    hidden B-part { state }
    promoted public members...
}
```

強力だが、`self^` がどの区画を指すか、`new` がどう初期化するか、委譲と合成をどう区別するかまで変わる。

##### 3. private をオブジェクト型から消す — OCaml型

OCamlのオブジェクト型は構造的で、private methodは外部から見えるオブジェクト型に現れない。

```ocaml
class counter = object (self)
  val mutable value = 0
  method get = value
  method private increment = value <- value + 1
  method bump = self#increment
end
```

外側から見る型は概ねこうなる。

```text
< get : int; bump : unit >
```

`increment` は実装には存在するが、構造型の一部にはならない。OCamlではprivate methodは基本的に `self` 経由でのみ呼ぶものとして定義されており、Java/C++型の「同じクラスなら別インスタンスのprivateも読める」とは異なる。[OCamlマニュアル](https://ocaml.org/manual/5.5/ocaml-5.5-refman.pdf)

これは先ほど提案したL^の公開ビュー／内部ビューに最も近い。

ただし、型から消すだけでは実体の同名衝突を解決しない。OCamlではオブジェクトの内部表現や継承機構がそれを処理するが、L^のような文字列キーの平坦なテーブルでは、別途内部キーが必要になる。

##### 4. private状態をオブジェクト外へ出す — クロージャ／能力型

LuaやJavaScriptで昔から使われる方法。

```lhat
let^ makeFoo = f^ {
    var^ state = 0

    return^ {
        get = f^ -> number^ { state },
        set = p^v:number^ { state := v },
    }
}
```

`state` は返すテーブルのメンバではなく、クロージャの環境にある。したがって同名衝突という問題自体が存在しない。

JavaScriptではネイティブprivate field以前に、クロージャや `WeakMap<object, state>` が使われていた。一般化すると、

```text
PrivateStorage[definition_id][instance] = private state
```

というサイドテーブル方式になる。

L^でも実装可能だが、

- インスタンスの割り当てが増える
- GCがサイドテーブルを認識する必要がある
- `self^.state` が普通のテーブルアクセスではなくなる
- 値のコピーやシリアライズとの関係が複雑になる

というコストがある。

##### L^に適用できる三つの現実案

###### A. 衝突を禁止する

```lhat
let^ A = def^{ self^{ private^ state := 0 } }
let^ B = def^{ self^{ private^ state := "" } }
let^ C = A .. B  # 衝突
```

最小実装。OCaml風の外部型消去だけ行い、格納方式は変えない。

欠点は、利用者には見えない実装詳細が合成可能性を壊すこと。同じミックスインを独立に改修しただけで、利用側の合成が壊れうる。

###### B. privateキーを宣言元で修飾する

```text
A::state
B::state
```

ソース上は両方とも `self^.state` だが、宣言元のメソッド本体を検査・コンパイルするときに、それぞれ異なる内部キーへ解決する。

```lhat
let^ A = def^{
    self^{ private^ state := 0 }
    a = f^self^ { self^.state }  # A::state
}

let^ B = def^{
    self^{ private^ state := "" }
    b = f^self^ { self^.state }  # B::state
}

let^ C = A .. B                  # 成立
```

実体は概念的にこうなる。

```text
{
    PrivateKey(A, state): 0,
    PrivateKey(B, state): "",
    "a": ...,
    "b": ...
}
```

JavaScript `#field` とGoの非公開名のidentityに近い方式。L^にはこれが最も向いていると思う。

必要になるのは、文字列以外のメンバーキー、またはコンパイル時に生成する決してユーザー名と衝突しないIDである。VM命令が現在文字列定数をキーにしているなら、例えば次の二系統にする。

```text
GET_MEMBER       "value"
GET_PRIVATE      definition_id, private_slot
SET_PRIVATE      definition_id, private_slot
```

さらに効率を求めるなら、private fieldをテーブルのハッシュキーではなく、定義レイアウト中のスロット番号にできる。

```text
A::state → private slot 0
B::state → private slot 1
```

これはアクセス制御だけでなく高速化にもつながる。

###### C. 合成パートを保持する

Goのように、AとBを内部サブオブジェクトとして残す。

```text
C instance
├─ A part
│  └─ state
└─ B part
   └─ state
```

A由来のメソッドへ渡す `self^` は、C全体ではなく「Cに埋め込まれたAビュー」を指す。

これは最も厳密だが、現在のL^の「合成後は一つの構造として振る舞う」という性質が弱くなる。`self^` の同一性にも影響するため、大改造になる。

##### 私ならこう進める

まずAで仕様を成立させ、その後Bへ移れるよう型情報だけは最初から宣言元IDを持たせる。

つまり初期実装でも、

```c
private_member {
    owner_definition;
    source_name;
}
```

としておき、実行時だけ当面 `"state"` に降ろす。将来、格納キーを `(owner_definition, private_slot)` に変えれば、言語仕様をほぼ変えずに同名共存へ移行できる。

ただ、後方互換性を気にしなくてよい現在の段階なら、最初からBも十分ありだと思う。L^では定義のメンバがすでに静的に解決され、`def^` の合成も定義時に確定しているため、「privateアクセスを宣言元固有スロットに解決する」条件がかなり揃っている。

要するに、構造型言語でよく使われる答えは：

> 公開名は構造的な文字列名、private名だけは宣言元に結びついた名目的なシンボル。

L^にとっても、これが一番きれいな着地点だと思う。

---

とのこと。自動 private slot 番号型、が確かに一番いいかな…

```lhat
public^let^A = def^{
    self^{
        a = 0,
        private^{
            a = 0 # 格納場所が違うので名前が被って良い
        },
    },
    proc = p^self^{
        self^.private^.a := 1 # self^.private^.a と書く必要がある
        self^.private^.privateProc() # self^.private^.privateProc と書く必要がある
        def^.private^.privateStaticProc() # 静的プライベートメンバーは def^.private^.*
        var^def^.private^.privateStaticData.a = 0 # 静的プライベートメンバーは def^.private^.*
    },
    private^{
        privateProc = p^self^{},
        privateStatic = p^{},
        privateStaticData = {}
    }
}

public^let^B = A .. def^{
    self^{
        private^{
            a = 0
        }
    },
    proc2 = p^self^{
        private^.a := 1 # 内部的に、self^.private^[1].a := 1 のような扱いになる
        # A の private^.a はこの場合先頭なので、self^.private^[0].a のような感じになる
    },
}
```

平坦化された姿は、

```lhat
B = {
    self^ = {
        a = 0,
        private^ = {
            { # A の分
                a = 0
            },
            {
                a = 0
            }
        }
    },
    private^ = {
        { # A の分
            privateProc = p^self^{},
            privateStatic = p^{},
            privateStaticData = {}
        }
    },
    proc = p^self^{},
    proc2 = p^self^{},
}
```

こんな感じか？

Q. `a["private^"][0]` などとすればいくらでもアクセスできてしまうが、いいのか？
A. 良い。private^ は完全なカプセル化ではなく情報の隠蔽（インテリセンスの候補に出さない）が主目的。
Q. privateメンバーは構造比較の対象か？
A. ~~対象外。privateメンバーにしか差がない型は、同一の型とみなされる。~~ これはうまくいかない。
privateメンバーも構造比較の対象にするしか無い。

#### ビット演算（済）

```lhat
bitand^
bitor^
bitxor^
bitnot^
bitshift^ # 算術シフトではなく論理シフト

# 引数がtrue^だったら一個でも立っているビットがあるか？false^だったら一個でも寝ているビットがあるか？
number^.bitany = f^self^, number^ -> bool^;
# 全ビットが、引数がtrue^だったら立っているか？false^だったら寝ているか？
number^.bitall = f^self^, number^ -> bool^;
# 指定したビット桁が立っているか？
number^.bitat  = f^self^, number^ -> bool^; # 範囲外は、false^
# bitand^ 等と同じく、実数だったら整数化して実行する（self^には変更無し）。
```

という形で決着。ついでに number^.isinteger も入った。

#### パラメトリック多相（済）

インスタンス化検査が入ったことで実質パラメトリック多相を実現している、という認識。

ホスト型・ホスト値用にジェネリクス風の記法も入った。

```lhat
std.task.async:p^c^{->*->*} -> std.task.Task<ARG0.ResultType>;
std.task.await:p^std.task.Task -> ARG0.T0;
```

`ARG0.T0` `ARG0.ResultType` `std.task.Task<>` と言った記法で、渡された引数の型から修飾された
返り値型を指定可能になった。

さらに、モジュール境界をまたいで適用できるよう、`templete^` 指定も入った。現状不満無し。

#### comptime^

Zigのあれ、なんていうんだろう…

L^でもコンプタイムというしか無いか？

```lhat
let^ListType = comptime^f^type: type^, length: number^ {
    def^ {
        self^ {
            type[length]
        }
    }
}

let^ListTypeA = SomeBase .. ListType(SomeDef, 3)

let^t = ListTypeA.new() # t => { SomeDef[3] }
```

#### std.task

```lhat
let^gen1 = p^{
    return await^someAsyncJob1()
}

let^gen2 = p^{
    return await^someAsyncJob2()
}

std.task.start(6) # ワーカースレッド６個で初期化。省略した場合物理コア分のワーカースレッドを製作

t1 = std.task.async(gen1()) # コルーチンではなくタスクハンドル、のような物を返す
t2 = std.task.async(gen2()) # なので、std.task.async/std.task.awaitの呼び出しは、yieldableにしない
std.task.await(t1) # get1 gen2 は別スレッドで完全並行動作。
std.task.await(t2)
```

Elixir で言うところの軽量プロセス は L^ では LhatMachine のこと。
数千のプロセスを立ち上げ並行処理、を実現するには、現在の L^ の設計では如何に LhatMachine を安価にできるか、にかかっている。
CPUリソースはさして問題ではなく、現状問題なのはメモリ容量。レジスタを最大数常に確保してしまうのをやめ、小さいサイズから初めて随時新調していく実装に改めれば解決する。

#### マクロ

定義

```lhat
macro^time(stats){
    let^start_time = time.now()
    stats
    print($"Time elapsed: {time.now()-start_time}s")
}
```

使用

```lhat
time! foo() # > Time elapsed: 0.0002s
```

マクロは、やっぱ今どきはやめたほうがいいかも。どんなに頑張ってもさほど「衛生的」にはならないんじゃないか、マクロである限り。

### 気になるところ（後で調べること）

relaxed では let^ と var^ に差がない、とすることで実行時の情報をケチれる、ということはないか？
var^ と書かないと := を許さない、というのは strict だけで十分、実行時は代入しろと言われたら
検査無しで代入でいいでしょ

let^vtos = {
    [constbox^positions.a] = "a",
    [constbox^positions.b] = "b",
    [constbox^positions.c] = "c",
}

の時、vtosのシグネチャが `t^{}` になってしまう。キーが文字列、識別子でないケースを想定していない。
おかげで、色々静的検査をすり抜ける。AIの言い方では、
> 辞書型（t^{ [K] : V } 相当）。用途は具体化済み（箱キー・hostdata キー・レジストリ）

~~これのついでで、現在不定長の型アノテーションが `t^{...:number^}` 等であるのを `t^{number^[]}` にしたい。~~（済）

~~関数の使用形も型推論の材料にすること（３パス目？）~~
「インスタンス化検査」として実装済み。

### もしかしたら今後やりたいこと

public^のシグネチャ明示。
考えが足らなかったので一旦取り下げたが、やはり将来的には必要かも。

ただ、それは旧来の
public^let^hoge = p^x:number, y:number^ -> number^ {}
と、本体の方の引数型、返り値型を明記するのではなく、

public^let^hoge:p^x:number, y:number^ -> number^; = p^x, y {}

と、**公開する束縛変数の型アノテーションで行う**。

def^も、def^本体の方ではなく、構造式を型アノテーションに書く。

### 実用言語として足りないもの

積極的な定数畳み込み
  ホスト定数の定数畳み込み

オブジェクトの寿命を静的に検知して可能ならGCAllocしない
  文字列、テーブル、ホストデータ、Box^の寿命を静的に検知し、
  関数・手続き・ファイル内で寿命が尽きることがわかっているものは
  GCに載せずファイナライザで解放するようにしたい。

### ゆくゆくはやりたい

コンパイルタイムメタプログラミング
コンパイル時にスクリプトを解釈実行できるLhatMachineを用意しておいて、comptime^と修飾された
文、式はコンパイル時に実行され、その実行結果で置き換えられる。

```lhat
comptime^let^なんかコンパイル時に呼び出せるスクリプト = f^->string^{
    ...
} # let^はここまでがコンパイルタイム扱い。var^はNG
let^a = comptime^なんかコンパイル時に呼び出せるスクリプト()
comptime^let^スクリプトを返すスクリプト = f^n -> program^{
    r = ""
    repeat^n {
    r ..= "dosomething() "
    }
    return^program^(r) # 文字列じゃなくて、置き換えるスクリプトだぞ、とマークする
}

comptime^スクリプトを返すスクリプト(5)
# 上に
# dosomething() dosomething() dosomething() dosomething() dosomething()
# と書いたのと同じ。

# ちなみに comptime^ をつけないと、そんな関数無いと言われる（本当に宣言してなければ）
スクリプトを返すスクリプト(5) # エラー
```

カスタマイズ可能な属性値

現状ホストプログラムがAPIで定義する形しかなく、L^で定義できない。

WASM出力

JIT

AOT

## L^ Visual Editor

グラフィカルにL^ スクリプトを編集する専用エディター

とりあえずは XState Editor（完全自前でReact Flowは使ってないらしい）を参考に
VSCode 上で動作する React Flow ベースのものを。

ただしオートレイアウトが基本で、自由なレイアウトは不要。なので、L^ソースにレイアウトデータを埋め込むようなことはしなくて良い。

メタデータを持つ予定はあるが、それは多言語化のための情報をソースから分離して持つためのもの。
レイアウトなど作業者ごとに必要なものはさらに別ファイルに格納してgit管理しなくて良い、とする。

| 拡張子 | 用途 | git |
| --- | --- | --- |
| *.lh | スクリプト本体 | ✔ |
| *.lhm | メタデータ（多言語化のための情報などを分離して持ちたい場合。コメントに埋め込むことも可能なので必須ではない） | ✔ |
| *.lho | コンパイル済みバイトコード | - |
| *.lhl | ビジュアルエディタのレイアウト情報 | - |

### 大人向けScratch

[Scratch Has a Marketing Problem](https://medium.com/free-code-camp/scratch-has-a-marketing-problem-f84626bd18ef)

↑ここで書かれていることが、まさしく L^ Visual Editor が目指していること。

### 文は縦に伸びる

if文 や パターンマッチ文に よる分岐は、分割されて横に並ぶ。

### 式は横に伸びる

if式 や パターンマッチ式に よる分岐は、分割されて縦に並ぶ。
