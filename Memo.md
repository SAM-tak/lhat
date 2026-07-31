# L^ メモ

## 現在実装中の構文

### 最新の構想

#### 静的自動型付け言語

シンボルはすべて参照
なので、すべてのオブジェクトは clone() shallowclone() (cloneと同義) dup() (cloneと同義。推奨名称)
deepclone() (循環参照をケアしながら再帰的にclone)を持つ。これが値渡しの代わり
ただし、関数、手続き、継続はcloneしても自身の参照を返すのみで本当に複製はされない、で良い気がする
（再帰clone処理の都合上、cloneを持たないとするより良いと思う）
パラメータとして渡すときのデフォルトはimmutable 手続き内で値の変更を許したいときはmutable^を
指定する必要がある

`{}`はブロックのみ  `{^`は使わない
`[]`はテーブルリテラルのみ  メンバアクセス インデックスの意味で使わない
素の`{}` はテーブルオブジェクトリテラル

JSON ならぬ LTON (エルトン) L^ Table Object Notation テーブルオブジェクトリテラルだけのファイルフォーマットも整備

テーブル改め、オブジェクト。テーブルオブジェクト、でいいや。Luaと同じく唯一のデータ構造

a.1 と、テーブルアクセスは全部基本’.’
a.i    テーブルaのキーi
i = 1 a.(i) テーブルaのキー1
a.(1.234) 実数で引く場合は()必須  これ良くないので[]復活
a.1.1   Cで言う所のa\[0\]\[0\]
a[1, 1] これも等価

=は比較 ==はない
値比較でなくインスタンスが同じかを比較したい場合は is^ 演算子
≠ ≦ ≧ も演算子として許可する、というかこちらを推奨する != =/ <= >= も許可する が、フォーマッタが書き換える
定義は:=
id : type = value
: とセットで使われたときに比較ではない扱い
再代入は<-
->はいいけど<-は a<-2とか負数との比較のとき困るか… 再代入は後置式にするか i + 1 -> i
別に i -> i + 1 でもいいような
存在したら代入、そうでないなら新規定義、は :->
↑ほんとに需要あるか？
i.set^(i+1)   うーん
set^i i+1   うーん
++ += などの演算子はなし
代わりに数値オブジェクトはメンバメソッド(手続き)としてinc() dec() add(a) sub(a) を持つ

空のブロックは書けない
do^{} とする
テーブルリテラルなのかブロックなのかはキーワードの有り無しでわかる
あるいはwith^init with^init ... {}
with^の場合はスコープ終了でdisposeが呼ばれる。他言語で言うusing

f^  関数。関数しか呼べない。代入はローカル変数に対してしか許されない。デバッグ用にログ出力などの脱法関数は用意する。
　illigable^修飾された手続きを呼べる、とか？
p^ 手続き。手続きと関数を呼べる。継続は呼べない
c^ 継続。三種全て呼べる。yield^が使える。コルーチンのエントリーポイント new^メソッドを持っていて、コルーチンを返す
反復子も列挙子も中身はこれ。ただ、「継続」という名称は良くないので避けたいかも（実行文脈データそのものではないので）。
yieldable、のほうが実態には合ってるがやだなぁ。やっぱL^における継続はこれだ、と言い切ればいいか
関数が継続返せないんじゃ意味ない。構文上は関数もしくは手続きで、yieldを呼んでるかどうかでYieldableとマークされ、返り値の一個目が
必ず継続になる。コルーチンはこれをラップして継続を隠蔽して返り値のみ扱えるようにする仕組み。
op^演算子 関数扱い すべての演算子は関数としての性質を満たすことを強制する、またyield不可
なので、++ += とかはない

（本物の）非同期はどうしよう？async/awaitみたいなやつ

文字列リテラル
`""`  エスケープあり 改行も含む
`''`  エスケープなし 改行も含む `'`は`''`
`$"string interpolation {foo} {2.4:bar}"` 文字列補間
`$'string interpolation ''{foo}'''` 文字列補間 文字列補間したいなら""必須
`"""` は行末まで文字列リテラル

識別子リテラル

```lhat
id^foobar
id^"foobar"
id^'foobar'

foo:={a:=f^{} b:=f^{} c:=f^{}}
foo[for^bar { when^1: id^a when^2: id^b when^3: id^c }]()
foo.(for^bar { when^1: id^a when^2: id^b when^3: id^c })()
```

といったことができる

コメント構文は特に用意しない。使用しない文字列リテラルがその代わり
// だけはコメント専用にする？

`$symbol` はグローバルへの参照
`$$symbol` ファイルスコープ
`$^symbol` 現在のリテラルスコープの親

パラメータ値参照
ビジュアルプログラミングとの整合性のために中間値を無名で参照したい

```lhat
f(1 + 2 + @(a(3)), @1 + 3) // うーん
```

以下と同値

```lhat
temp := a(3)
f(1 + 2 + temp, temp + 3)
```

やっぱビジュアルプログラミングでも式からノードを引っ張って複数の箇所に刺そうとしたら
変数を自動で作って命名させる方式にすべきか？自動命名変数は採用したくないtemp_12345678とかになるから
ワイヤー引っ張ると自動でスコープを調整してくれて、スコープの最小化を指示すると自動でやってくれる、とかで良さそう

L^ 言語環境
L^.collectgarbage() とか

コマンド形式手続き呼び出し
>> foo 1 2 3

{width^2: 11, 12, 21, 22}[2, 1] // 21
{autowidth^: // Nx3 行列
11, 12, 13,
21, 21, 23,
31, 32, 33,
}[3, 2]  // 32

列挙体（テーブルオブジェクトの特殊化）
{enum^:
a,
b,
c,
enum^10:
d,
e}

{enum^3{a, b, c, d, e}
1, w = t, (e) = t}

nilプロパゲーション
boolを返す関数に?で終わる名前付けを許すために、!のほうをnilプロパゲーションに使う？
foo!.bar!(a).foobar?() // foobar?はそういう関数名
 !. !() という演算子が存在する
なので、!はシンボル名に使えない。?も末尾のみ、値がboolである場合のみ許可

三項演算子は無しでif式のみ  入れ子にしたとき醜いので

f = p^x, y, z {z, x, y；defer^}
f = p^x, y, z : z, x, y; // : で区切るとreturn^を省けるが、;が必須
　　foo(f^:x, y)
       - foo(f^{return^x, y}) or foo(f^{return^x}, y) ?
       なので;を要求する。
　　　　　foo(f^:x;, y) or foo(f^:x, y;) {}必須でいいでしょ
関数は返り値を必ず持つので、return^は必須にしたくない
手続きは返り値がない場合もあるのでreturn^は要るが

if文
if^expr {
elseif^expr:
    stats
else^if^expr:
    stats
elsif^expr:
    stats
elif^expr:
    stats
ei^expr://全部許していいでしょう。
    stats
else^:
    stats
el^:
    stats
or^expr:
    stats
or^:
    stats
}

推奨は if^ { elseif^: else^: }

if式
if^expr:expr elseif^expr:expr elsif^expr:expr elif^expr:expr ei^expr:expr else^/*or el^*/expr;

推奨は if^expr: el^expr: el^: ;

後置if式は設けない。入れ子にしにくいから

パターンマッチ

for when 文
for^expr {
when^0:
    stats
when^1 to^3:
    stats
other^: // : は省略可能だが必須にするか？
    stats
}

for when 式
for^expr: when^0:expr when^1to^3:expr else^:expr;

repeat^n {} // n回繰り返し
repeat^{ ...break^} // 無限ループ
repeat^while^expr {}
repeat^until^expr{
prolog^:
    stats
first^:
main^:
    stats
last^: // これがあると前回の状態を保存しておく必要があるな
epilog^:
    stats
}

繰り返しは全部 prolog^(一回だけ) main^ epilog^（終了時のみ）項を持てる
これがあれば後置until/whileいらないのでは？その場合は、prolog^main^:という書き方を許容する必要あり
後置until/while は ; 必須で見た目が悪いから採用したくない
しかもプログラミング初心者には分かりづらいだろうし

repeat^{}while^expr { epilog 一回でも実行されたら }
repeat^{}until^expr{  epilog  }
repeat^{}while^expr;
repeat^{}until^expr;

for^init repeat^{}while^expr { epilog 一回でも実行されたら }
for^init repeat^{}until^expr{  epilog  }
for^init repeat^{}while^expr;
for^init repeat^{}until^expr;

do^{} while^expr{}
do^{} until^expr{}
do^{} while^expr;
do^{} until^expr;

for^init do^{} while^expr{}
for^init do^{} until^expr{}
for^init do^{} while^expr;
for^init do^{} until^expr;

後置表現全部廃止

for from to 文(numeric for)
for^i from^1 to^10 { // Luaのfor i=1,10 等価
}
for^i from^10 to^1 { // Luaのfor i=10,1,-1 等価
}
for^i from^1 to^10 step^2 { // Luaのfor i=1,10,2 等価
}

for in 文(generic for)
for^k, v in^table {
}
for^k in^table.keys^ {
}
for^v in^table.values^ {
}

for while next 文
for^i := 1 while^i < 10 next^i << i + 1 { // Cのfor(;;)と同じように使える
}

for until next 文
for^i := 1 until^i >= 10 next^i.inc() {
}

for if 文
for^i := 1, j : int = 2 if^i + j < 10 {
}
do^{
    i := 1
    j : int = 2
    if^i + j < 10 {}
} と等価

オブジェクト指向

```lhat
// "クラス"定義
$Foo:={
    // インスタンス定義
    new^...{
        member:=f^a:=....1 b:=....?2or^nil { a + b }
        samenamemethod:=p^{
            return^ member()
        }
    }
    classmethod:=p^{}
    samenamemethod:=p^{ classmethod() }
}

Foo := $Foo
f = Foo.new(2 , 3)
f.member()
Foo.classmethod()
f.classmethod() // 合法としよう
f.class^.classmethod() // 上が合法なら冗長だが合法
f.samenamemethod() // これは警告、もしくはエラー？
f.class^.samenamemethod() // これは完全合法
```

#### 型明示

```lhat
k := p^x:number^, y:number^, z:number^ {^return^z, x, y}
k := p^x, y, z {^return^z, x, y} as^p^number^,number^,number^->number^,number^,number^;
k :p^number,number,number; // 返り値なし
k :p^->number,number,number; // 引数なし
k :p^; // 引数なし返り値なし（nil）
k :number^|nil^ // nilable

elseif^expr: などのせいで:を後置型指定に使いずらいのでas^に。

f^a:number^, b:number^=2->number^,string^ {}
返り値型は ->  :: で指定

f:=f^a:number^,b:number^=2->number^,string^{ a+b, "aa" }
f.type // f^a:number^, b:number^=2->number^
>> print f.parameters.length // 2
>> print f.parameters[0].type // number^
>> print f.parameters[0].name // a
>> print f.parameters[0].default // nil
>> print f.returnvalues.length // 2
>> print f.returnvalues[1].type // string^
print(f(1)) // 3 aa

rangeリテラル(いやいらないかな…for from やwhenの時しかかけないでも)
1 to^10
 is like:
range.next = f^t, i:if^i == nil^: t[1]
                    ei^t[3] > 0 : for^i+t[3]: when^t[1] to^t[2]:it^ other^nil^
                    el^           for^i+t[3]: when^t[2] to^t[1]:it^ other^nil^;
from^ = f^range:$range.next, range, nil;

{1, 10, 1}
10 to^1
 is like:
{10, 1, -1}
10 to^1 step^2
 is like:
{10, 1, 2}
// これは10を返した後一回で終わってしまう
```

#### クラス定義

```lhat
// 単一継承のみ。「中間状態にいちいち名前を付けなくて良い」という性質で多重継承ぽくもできる、というやり方
// 実体型・抽象型・プロトコル定義・オブジェクトテンプレート・アスペクト、全部をdefテーブル一個で済ます
$Foo := aspect..protocol..prototype..def^{
    self^{
        ...
        prop1 = true^
        prop2 = false^
        // インスタンスプロトタイプ
    },
    staticmethod = p^:;
}
// defnameof^foo -> "Name of Defination"になる。前後の空白はトリムされる
// defnameof^foo.super^ -> "Name of aspect"
// defnameof^foo.super^^ -> "Name of protocol"
// defnameof^foo.super^^^ -> "L^ Base Prototype"
// defnameof^foo.super^^^^ -> nil

// プロトタイプベース・ダックタイピングで行くのだから、継承はいらない。
// 静的な型検査もダックタイピングでいく

foo=Foo.new^()
foo.selfcall()
...
foo.dispose()
with^foo:=Foo.new^()
with^bar:=Boo.new^() // error : bar doesn't have dispose.
{
     ...
}
```

------------------------
以下古い構想
L^
C^^
C``
どれが良いやら。
（Cを付ける場合は、Cライブラリを呼び出す動的スクリプト言語、の意味。Luaの実装を流用しない場合）
C^
CCaret
C^^
CCaretCaret
コンパイラ ccc cccc

日本語キーボードだと^はシフトいらない。`は要る。USキーボードは逆
シフトいらないのはいいが遠い

i^"space\\ (included\) identifier)"
"space\ (included) identifier"という識別子指定の例。とにかく予約語がないし名前の制限が緩い。将来専用エディタでの編集に特化するため

//だめ。完全に自由な名前は、別ファイルのメタデータでやるべき。ローカライズも必要なんだし、生ファイルはテキストエディタでの視認性を優先すべき
そうか？ユーザーにローカライズを期待するのは無理な気がする
シンボルローカライズは本格的なプロバイダが用意する、が、一般ユーザーには無理。

関数定義どうするか？
p^x:int:int;
見づらい。がこれ採用で。()をつけるかはユーザの判断。
p^x:int->int;
普段の型指定と整合性がない
p^(x:int):int;
デルタ式構文を別途用意したくなる

やっぱり最後が一番いいんじゃ？デルタ式は後から追加してもいい。

p^():int; vs p^:int;

引数がない場合は省略できていいんじゃないかな。これシグネチャの指定でのみで
型不定ならいらないので

あとif^{...}[foo]==bar{^}というふうにその場でテーブル作って渡す記述許したいので、ブロックの開始は{^しかないと思う。

```lhat
//#reserve on
//#grave off
using`OtherNamespace

// 果たして分割定義と新規定義を演算子で区別できる、
// と言うことのためにクラスが第一級オブジェクトである必要はあるか…？
$MyNamespace.IMyClass := interface^{
     // シグネチャのみを指定
     {
          memberVirtual1 : p^(x:int,y:int):int; // シグネチャ指定の場合は;必須
          memberVirtual2 : p^(x:p^(x:int,y:int):int,string;,y:int):int; // メソッド変数の型指定
          // (,は省略できなくていいかな)
          memberVirtual3 : MyClassFoo.function; // 定義済み手続き・関数のシグネチャを採用する場合
          memberVirtual4 : MyClassFoo.self`function; // 定義済みインスタンス手続き・関数のシグネチャを採用する場合
          memberVirtualField : string;
     }
     staticVirtual : p^(x:int,y:int):int,int; // 多返却の場合
     // staticVirtual : p^(x:int,y:int):int,int;
     // インターフェース定義のみ特殊。パラメータの型指定の構文と統一させるため
     // := ではなく :
}
$MyNamespace.IMyClass := interface^{
     // インスタンスメンバのみなら、こう書ける。インターフェースだけでなくクラスも
}
// L`にstatic classは不要。C#などのは、メンバにstaticと修飾することで静的メンバを定義してるから必要なだけ
$MyNamespace.MyClass := Foo+IBar+IMyClass+class^{
     // 基本クラスOtherNamespace.FooとインターフェースOtherNamespace.IBarとMyNamespace.IMyClass
     // を継承している例。usingしているのでOtherNamespaceは省ける、 MyNamespace.MyClassの定義中なので
     // MyNamespaceも省ける
     {
          // インスタンス定義
          member := 0
          funcMethod := f^(x:string):int{(if^x!=0:int.Parse(x)else^member.ToString())}
          method := p^(x:string):int{
               member = int.Parse(x)
               return member
          }
          table := {name:="aaaa"}
     }

     // 型名の指定ではMyNamespace.MyClassがインスタンスの型。
     // クラスを要求する場合はMyNamespace.MyClass.class` ？
     // classof`MyNamespace.MyClass ？
     // class`MyNamespace.MyClassはカテゴリ名扱いになるから使えない
     Self^{
          // コンストラクタ
          // 何もしないなら省略できる。
          // インスタンスがあるクラスかどうかは入れ子の{}の有無で区別
     }
     static := "static field"
     public^staticMethod := p^(x:int):int{
          return x+1
     }
     // = はオーバーライド
     // := 新規定義
     // += はオーバーロード
}
// グローバル変数宣言の場合$は省略できない。=なら$は省略できる
$MyNamespace.Bar += class`CategoryName<DependCategoryName{
     // partial class definition
}
/*
MyNamespace ?+= {^
     Foo := class^BaseClassName(InheritedInterface,InheritedInterface2){^
          // こういうネームスペースで囲む記法を用意する必要はないかも
          // $MyNamespace.FooでMyNamespaceがなかったら自動でnamespace作られる
          // だけで十分。?+=演算子（存在しなかったら新規、そうでなければ追加）
          // なんてもの用意しなくて済むし
     }
}
*/
// やるならこうか。名前空間は特別扱い(第一級でない)
namespace^MyNamespace.MyChildNamespace {
    Foo := class^{
    }
}
for^ k,v in^table.All() : name {
    label^labelName
    while^true:name2 {
        do^{
             if^x<0{break^[name]} // 一番上を抜ける
         while^false:name3}
         if^x==222 { goto labelName }
    }
}
```

名前空間いるか？静的メンバかクラスしか含まないクラス、で十分では
名前空間という概念のある言語は、クラス定義をあとから変更できない。
クラスとは別にあとから自由にシンボルを追加できる場所として名前空間を用意してる
L^が名前空間を持たないとしたらクラス定義の事後変更ができなければならない
あるいは、シンボルの追加のみは自由、とするか。
L^でクラス定義の事後変更を禁止する必要はあまりないかも。

```lhat
for^i from^1 to^10 step^2 {^
}
for^i := 1 while^i <= 30 next^i = i + 1 {^
     // i = 1 -> 30
}
while^i < 30 next^i + 1 {^} というのも用意しようか
do^{^sent while^expr}
do^{^sent while^expr next^expr}

do^{^
     sent
while^expr}

do^{^
     sent
     while^expr
} // こっちのかかれ方やだなぁ

for^i := 1, j := 3 while^i < 30 and^j < 3 next^i = i + 1, j = j + 1 {^
     // nextで代入文を書かせない、は無理だと思う
}

for^i := 1 to^10 {^
     for^j := 1 while^j<20 {^
          if^xxxx{^break^[1]} // 一番上のループを一気に抜ける
          j = getnext(i, j)
     }
}
```

for to文いらなくね？
for i in range(1, 6[, 1]) {^}
とか、pythonのイテレータ返す組み込み関数みたいなもの用意して…
ってLuaに無いんだよね。。。
for while 文やfor to stepはLuaの記述に機械的に変換可能だと思うけど

```lhat
/*
a := lazy`f`x:int{i+x}
b := lazy`FooFunction(1)
// 自動で遅延評価にならないなら参照透過性の意味が無いのでこれは不要
for^k,v in^table.Range(a, b) {
}
*/
// 擬似変数thisによる自己再帰
functorial := f^(x:float){^
     (if^x==0: 1 else^:x*this^(x-1))
}
// 関数ローカル変数への代入は許されるので（参照透過性を崩さない、ハズ）再帰で定義する必要はない
functorial = f^(x:float){^
     return := 1
     for^i from^1 to^x {^return = return * i }
     return
}
// ラムダ式
for^k,v in^table.Where(k,v =^ k=="hoge") {^
     // ラムダ式の型は文脈で決まる必要がある、でいいかな。
     // (k:???,v:??? =` k=="hoge") とは書けない、で。
     // 型指定したい場合はf`を使え、で。
     // else`if`:のせいでf`p`で始まってないとid:typeidの構文を受け付けない、としないと解析が面倒
}
// for inはループ変数必ず新規定義、でいいかな
// ヒアドキュメント
print(<<EOS
"ダブルクォーテーションはそのまま"EOS, i, param)

//if文
if^x<0{^
     print(y) // nilというか未定義
else^if^x == 0:
     y := 1
     print(y) // 当然1
     // ifが来た時だけ:要求で十分な気がする
else^if^x == 1{^
          // これ、文法エラーにしようか？
          // 文法エラーにする。:が無い、と。
     else^:
          if^x == 2 {^
               // これならありなんだけど…
          }
     }
     print(y) // nilというか未定義
     // if elseif else形式のぶら下がりifのない構文だと、実質if ~ else間でスコープ独立
     // になる、でいいんだよね
}
if^expr{sent else^if^expr:sent else^if^expr:sent... else^:sent}
これでFA?
else^:sentと書かせるならelse^if^はあり。else^sentならelseif^必須じゃない？

// C#と同じく、elseif^やelif^などは提供せず、elseに新たなif文をぶら下げたいなら新規ブロックを作ることを要求する。
Luaがelseif持ってるからelseif^はあったほうがいいかも
//if式
上で
(if^cexp1:rexp1 else^if^cexp2:rexp2 else^:rexp3)

or... (?^cexp1:rexp1 |?^cexp2:rexp2 |^:rexp3) あくまで記号ベースの場合
これ、いいかな？ei^ el^まで用意するならいらないのでは

if式かif文か、の分別は、
if^expr:
か
if^expr{^
か、で見分ける。

カッコで囲むの必須にしたいが、難しいかも…？
いや、多分大丈夫でしょう

という構文が提案されているが…
まぁこれがベストか
else^if^の短縮形としてei^を用意してもいいかも
だったらついでにelse^の短縮形el^も…
この短縮形はif式でしか使えない（if文で使えない）として。
(if^cexp1:rexp1 ei^cexp2:rexp2 el^rexp3)
　　（こっちだと、else節の:は省略できる。else^if^では:がelseの直後に来るかどうかで判定すべき（ぶら下がりif対策））
いい気がする

//cexp1?rexp1:?cexp2:rexp2:rexp3
//うーん。三項演算子の欠点そのままな気が。改良点は並列条件が書きやすいのみ
//よって却下

// レキシカルスコープ
$a := 0 // グローバル変数
print(a) // 0
a := 1 // ファイルローカル
print(a) // 1
print($a) // 0
print($.a) // 0
print($$a) // 1
print($$.a) // 1
print($1a) // 1
print($1.a) // 1
print($2a) // エラー
print($0a) // エラー　か1
print($-1a) // エラーか0
{^
     a := 2 // ブロックローカル
     print(a) // 2
     print($a) // 0
     print($$a) // 1
     print($$$a) // 2
     print($1a) // 1
     print($2a) // 1
     print($3a) // 2
     print($0a) // エラー か、2
     print($-1a) // 1
     print($-2a) // エラーか0
     {^
          a := 3
          print(a) // 3
          print($a) // 0
          print($$a) // 1
          print($$$a) // 2
          print($$$$a) // 3
          print($1a) // 1
          print($2a) // 2
          print($3a) // 3
          print($4a) // エラー
          print($0a) // エラーか3
          print($-1a) // 2
          print($-2a) // 1
          print($-3a) // エラーか0
          print($-4a) // エラー
     }
}

/*
$
-2a
グローバル環境と、-２と変数a。文法エラー。
$-2a
空白文字無しでくっつけて書かないとスコープ指定子とみなされない
*/
やっぱ^採用で、
$$$$$a
$^^^^a
$3$a
$3^a
とかのほうが良いような
もともとこの言語を作ろうとしたのは、Luaの基本グローバル、ローカルにキーワードの修飾子が必要、ローカル変数をマスクするとエイリアスを作っておかない限りマスクしたローカル変数にアクセス出来ない、のが不満だったんだろうに
C#なんかではローカルのマスクを許さない、
for(int i = ...) { for (int i = ...) {}}
はエラーになるわけだけど、入れ子のループ変数に別々の名前を付けなければいけない、なんてのは構文強度の本質的な強化だろうか？
ループ変数なんてのは適当な名前でOKであるべき。

いや、やっぱ$-3のほうが見た目がいいような
```

テーブルリテラルどうしよう

今のところ
1)すべてのブロックを{^ ... }として テーブルリテラルは{ }
2)if/for/classなどの{}を要求する構文ではブロックであり、その文脈以外では{}はテーブルリテラル。制御文以外でブロックを定義したいときに{^ }
3)ブロックはすべて{}で、テーブルリテラルがt^{}。テーブル内の{}はブロックではなくテーブルリテラル
の３案ある。
1)3)は文脈に依存しないが、冗長
3)は冗長な上に統一性がない
2)は冗長ではないが、文脈に依存して全く違う解釈に。
Luaベースだと、ブロックのつもりで書いてテーブルリテラルに解釈されても即構文エラーとはならないところが辛い
なるほど、pythonのようにインデントでブロックを表すとしたほうが、このへんの悩みはなくなる
上に書いたが、あらゆるところでテーブルを即値で渡せて、かつテーブルリテラルが{}のままで、インデントブロックを採用する気がないのなら1)しかない
