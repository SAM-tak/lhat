# L^ メモ

## 目標

Modern & Better Lua with Visual Programming.

型システムが後付のLuauよりも良いものになってほしい。

## 影響を受けた言語

Lua / Zig / TypeScript / Python / Luau / Ruby

## 言語コア

「変数はすべて参照」は撤回。指す値の型によっていかようにも変容する。
効率のために真偽値と数値は値型で、ユーザー定義値型をも導入した今、維持できない方針なので。
ただAIの回答、値型は同一インスタンス、という返答には納得できていないが。

### nil^ の非対称性

nil^は終了時返す値がなかったら代わりに積まれるので、
p^; と p^->nil^; は**全く一緒**である。
しかし、**p^; と p^nil^; は違う**。p(nil^) は p() と違って実際に nil^ をつむ。
p()は実際に積まない。実行時に「引数の型が違う」と言われて落ちるので、呼び出し側は区別する必要がある。

### 多値返却を採用した意義

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

### サイレント実装漏れ

enum^
  いつの間にか廃案扱いになっている。
  が、確かに初期構想のテーブルの一種とするより、error^ 同様公称型の特別な型、
  としないと列挙型に期待する役割を果たせないかもしれない。
  それとは別に外部データからの読み取りで作ったテーブルのキーを列挙値のように扱いたい
  というケースは存在するだろうから、公称型としての「列挙型」と「列挙型テーブル」の
  二種類が存在するのだ、という建付けにする必要があるかも。

多次元配列
let^t = {width^3:
    1, 2, 3,
    4, 5, 6,
    7, 8, 9,
}
print(t[1, 2])

### 検討項目

> 文面は「添字」を含んでいるのに、実装は ( しか見ていない（parser.c:1827 にだけ !at.preceded_by_newline があり、その直前の [ の枝には無い）。

```lhat
let^ t = { 10, 20 }
let^ a = t
[1]
print(a)          # → 10
```

> t[1] として解釈される。10.9 が防ごうとした Lua の罠が、[ について現存している。

が、これが実害をもたらすのは本当にC#風の[属性]を入れたときや、[]をリストリテラルにした時、
なので放置でいいか…？

hash^ ユーザー定義のハッシュ。これがないと値比較でテーブルのキーにすることができないだろう、けど、
自動化できないか？

静的無限ループを検知してエラーにする
  脱出条件のないrepeat、等

#### sync^構文

同期的にその場でコルーチンを回すイディオムを毎回書くのだるいから、`sync^`を追加しても良いかも。
が、具体的にどういう構文になる？ 受け取る結果は T 型のみでいいが、 R Y があるコルーチンの場合は？
`sync^`一語では済まない。L^ にマクロかジェネリクスがあればユーザー定義で同じ事ができるかもしれないが、
現状無いのでできない。

> sync^(coroutine, awaiter:f|p^Y -> R;)

```lhat
let^r = sync^(co, f^a, b { # start/resume の返り値を引数に受け取り（そしてこれは流石に推論してほしい）
    print(a)
    return^b + 1 # 次のresume に渡す値を返す
})
```

この構文で良さそうだが、もうちと練る。（が、Fable先生によると必要ないらしい…？）
たしかに。Unityで async/await使っているとasync関数を呼び出すトップの姿を見たことがないわけだが
Unity内部やUniTaskの中で見えないだけで、そういうもの（スケジューラー）は存在する。
スケジューラーをユーザーに書かせるかホストが提供するかはホストアプリケーションの判断。
どっちにしろ、ユーザーサイドでコルーチンの自前回しは基本しない、と考えて良い。

#### 異型 yield は、実は今でも書ける

let^ g = p^ -> c^{p^ -> number^|string^ -> nil^} {
  yield^ 1 as^ number^|string^      # 通る
  yield^ "a" as^ number^|string^
}
as^ を外すと落ちる。理由は chk_unify_yield が lhat_type_equal（厳密一致） で照合しているから — 合併を書いてあっても各サイトが合併型そのものでないと通らない。

ここを「Y が書かれているときは適合で照合する」に変えれば as^ が消える。 検査器1箇所。「全部 await する」を現実的にする最小の一手はこれ。推論で勝手に合併を作らない（15.2 の「異種の yield^ を型で見分けさせない」）方針は保ったまま、書いた合併だけは効くという線になる。

「どうしても同型にできない」場合も、アダプタは普通の p^ で書ける — 内側を回して外側の形で yield し直すだけ。マクロは要らない。

let^ adapt = p^ inner:c^{…} -> c^{外の形} {
  var^ y = inner.start()
  ... 外の形に変換して yield^、返ってきた R を inner.resume() に渡す
}
呼ぶ側は await^ adapt(inner)。

##### まとめると

専用構文もマクロも要らない。要るのは (a) Y を「要求型」1つに決める設計判断、(b) 書かれた Y の照合を適合に緩める検査器の1箇所
f^ 内での駆動は諦める。作る側が f^、回す側が p^ かホスト、で分業する
Godot は _ready start /_process resume。エンジン側の await と同じ構造なので違和感も出ない
(b) は小さい変更で効果が大きい。やるなら着手する。

これは良いが、なるとなると c^{} のシグネチャの手書きが増えるのが嫌。

gen.Return などとしてその返り値型にアクセスできて、

f^->gen.Return {}

と書けるなら良いかもしれない。が、そうなると合併されてた時の書き分け、引数にも同様なのが欲しい、となる…

```lhat
func = f^a:number^, b:string^ -> (number^|string^, number^|nil^)|Error|nil^;

func.Argument        # (number^, string^)
func.Argument[1]     # number^
func.Argument[2]     # string^
func.Return          # (number^|string^, number^|nil^)|Error|nil^
func.Return[1]       # (number^|string^, number^|nil^)
func.Return[2]       # Error
func.Return[3]       # nil^
func.Return[1][1]    # number^|string^
func.Return[1,1]     # number^|string^ これはこう書けなくても良いかな…
func.Return[1][1][1] # number^
func.Return[1][1][2] # string^

gen = f^a:number^, b:string^->c^{f^number^,number^->string^,string^->File|Error|nil^};

gen.Return          # c^{f^number^,number^->string^,string^->File|Error|nil^}
gen.Return[1]       # c^{f^number^,number^->string^,string^->File|Error|nil^}
gen.Return[1][1]    # (number^,number^) こう、か？つまり R項
gen.Return[1][2]    # (string^,string^) つまり Y項
gen.Return[1][3]    # File|Error|nil^ つまり T項
gen.Return[1][3][1] # File
gen.Return[1][3][1] # Error

c^{}はそれ以上分解できない、でいい気はする
```

typeof^() は？あれは何？

あと、現状の as^ はまずい。キャスト失敗＝panicは乱暴すぎる。

考えられる解決は、 as^ T は T|Error.CastFailure で T への解決はしない、というものでは。

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

### 気になるところ（後で調べること）

relaxed では let^ と var^ に差がない、とすることで実行時の情報をケチれる、ということはないか？
var^ と書かないと := を許さない、というのは strict だけで十分、実行時は代入しろと言われたら
検査無しで代入でいいでしょ

Vector3|OutOfMemory などエラーと合併している型を絞り込む手段として、正当なのはtry^をつけることなのに
as^ Vector3 とキャストしてしまうことでも絞り込みなってしまう（エラー＝panic）
OutOfMemoryならたしかにその挙動でいいのだが…その他のエラーもキャストで潰せてしまうのは問題では。
つか、isa^さえあれば、as^なんてキャスト手段用意すべきじゃなかった…？
他言語にあるからきっと必要なんだろう、程度の理由で存在しているが。

let^vtos = {
    [constbox^positions.a] = "a",
    [constbox^positions.b] = "b",
    [constbox^positions.c] = "c",
}

の時、vtosのシグネチャが `t^{}` になってしまう。キーが文字列、識別子でないケースを想定していない。
おかげで、色々静的検査をすり抜ける。AIの言い方では、
> 辞書型（t^{ [K] : V } 相当）。用途は具体化済み（箱キー・hostdata キー・レジストリ）
これのついでで、現在不定長の型アノテーションが `t^{...:number^}` 等であるのを `t^{number^[]}` にしたい。

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

コンパイル済みバイトコードのファイルへのシリアライズ・デシリアライズ

VMのみビルド

オブジェクトの寿命を静的に検知して可能ならGCAllocしない
  文字列、テーブル、ホストデータ、Box^の寿命を静的に検知し、
  関数・手続き・ファイル内で寿命が尽きることがわかっているものは
  GCに載せずファイナライザで解放するようにしたい。

定数畳み込み

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

## Godot組み込み

### L^ 側のラッパーライブラリ

いま、なにかスクリプトの確保に関して非効率なことをやっているらしい。
godot\demo\lhat\Godot.lh にズラズラブリッジクラスの定義を増やしていくと、Node2D..def^ で一個触っただけで
全体の再読み込みになる…らしい？

node.call は返り値が any^ なのでホスト値を詰めない。
    型ごとの呼び口 — callVector2(name, ...) -> godot.Vector2。getVector2 と同じ形で、静的に型が決まる。17本増える
これをやればいい。が、今のところ不要なのでやってない。

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

### 文は縦に伸びる

if文 や パターンマッチ文に よる分岐は、分割されて横に並ぶ。

### 式は横に伸びる

if式 や パターンマッチ式に よる分岐は、分割されて縦に並ぶ。
