# プログラミング言語 L^

![L^ Logo](media/lhat-logo.svg)

**英語版はこちら: [README.md](README.md)**

`L^`（elhat）は、静的型検査が効いたバイトコードインタプリ型のグルー言語です。C11 で書かれ、CMake でビルドします。

Lua の実行時モデル——タグ付きの値、ひとつのデータ構造、コルーチン——に、パーサとコード生成器のあいだに型検査器を置いたものです。ファイルは strict（厳格）が既定で、間違いは実行時の異常ではなく診断になります。日本語で使うには `--language ja` を付けてください（[多言語化](#多言語化)を参照）:

```text
$ lhat --messages messages/ja --language ja --check todo.lh
todo.lh:2:1: エラー: この名前は let^ で束縛されていて、代入し直さない。名前を変える必要があるなら var^ と書く: done
todo.lh:2:9: エラー: この値は、書かれた場所に合わない
```

組込みを前提としています。言語は 1 つのヘッダで到達するライブラリであり、メモリ確保はホストが差し替えられるわずかな関数を介するだけで、ホストが手段を与えなければ、ファイルシステムには触れません。

- **バージョン** 0.3.10 —— 1.0 未満で、後方互換性は約束しません
- **ライセンス** Apache 2.0

## 何のための言語か

自分で書いたのではないホストにスクリプトを書かせるためのものです。ゲームエンジン、ビルドツール、データパイプライン、エディタなど。ホストがプログラムに見えるものを決め、プログラムは動く前に検査され、全体は 1 つのヘッダでライブラリとしてリンクされます。

## 設計が従う原則

仕様におけるどの決定も 1 文に帰着します: **間違いは書かれた場所で報告し、同じ言語規則を二重に実装しない。**

`=` は比較なので、*方程式に似た*ものはどれも方程式ではありません。呼び出しの `(` は呼び出し先と同じ行に置くので、誤読は「起こりにくい」ではなく「起こりません」。再代入は前置なので、文が `-` で始まることはありません。置換可能でない `override^` は、約束をした `..` の場所で報告されます。言語サーバはスコープを導き直さず検査器に尋ねます。二つの実装は、規則が難しい場所でこそ食い違うからです。

帰結として、見慣れた機構がそもそも存在しません。以下がそれで得られるものです。

### 予約語が存在しない

キーワードは常に `word^` の形をとります。`^` はその以外の用途に使いません。`if` は普通の識別子です:

```lhat
var^ if = 1
print(if)          # 1
```

そのため字句解析器にはキーワード表そのものがありません——ハット付きの識別子はすべて同じトークン種別で返し、それがキーワードなのか型名なのかは構文解析器に委ねます。キーワードを増やしても既存の識別子は壊れません。これが `^` 記法を採用する理由そのものです。

### `=` は比較、`:=` は再代入、`==` は無い

```lhat
var^ n = 0
n := n + 1
print(n = 1)       # true
```

`i = i + 1` は方程式に見えて別の意味を持つので、その綴りでは書きません。名前を変える方法は 2 つで、`:=`（このほう）と `let^`/`var^`（新しい名前を作る）です。どちらが必要かは検査器が指摘します:

```text
error: this name was bound by a let^ and is not reassigned; write var^ where the name has to change: n
```

上の診断は既定（英語）です。`--language ja` を付けると `--dump-messages` が見節の言語で出ます。

### 改行に意味が無い

自動的セミコロン挿入すらありません。字句解析器は改行トークンを出さず、すべてのトークンが「直前に改行があったか」を持ち、それを参照する規則は 10.9 の呼び出し括弧の規則**ただ 1 つ**です: 呼び出しと添字の `(` は、呼び出し先と同じ行になければなりません。

これが次を「呼び出し」として読まないようにします:

```lhat
let^ f = twice
(21)               # エラー: 式だけでは文にならない。return^ のつもりか
```

Lua ではこれは `f(21)` として読まれ、やがて生じる失敗はまったく別の場所を指します。ここでは誤読そのものが起こりえません。実用的な帰結も 1 つ: プロンプトへの貼的行为は、ファイルとまったく同じように振る舞います。

### 誤りが型である

例外機構はありません。失敗しうる操作は、値と失敗しうる方々の合併を返し、その合併が*そのまま*型です:

```lhat
errordef^ ParseError {
    Syntax { line : number^, column : number^ },
    Eof,
}

let^parse : f^string^ -> number^|ParseError; = f^text:string^ {
    if^ text = "" { return^error^ParseError.Eof{} }
    return^error^ParseError.Syntax{ line := 1, column := 1 }
}
```

扱う方法は 3 つ——置き換える、呼び出し元へ返す、ブロックごと捕まえる——と 4 つ目で、合併を受け取って絞り込みます:

```lhat
let^r = parse("hi")
if^ r fits^ ParseError.Syntax {
    print($"syntax error at {r.line}:{r.column}")   # ここでは r.line は number^
el^:
    if^ r fits^ ParseError.Eof {
        print("end of input")
    }
}
```

すべての種別を尽くしたときに残るのは成功時の型です。それが網羅性であり、専用の機構は要りません: `when^` は同じ `if^` の連鎖に落ち、残りは同じ絞り込みが担います。

誤りの*種別*は型として宣言されます。Zig 風の error set が自前の構文を持たなくてよい理由がこれです——`|` はもともとありました。L^ が公称的な同一性に手を伸ばすのはここだけです。「標準ライブラリの `NotFound`」と「利用者が宣言した `NotFound`」は同じ種別ではあってはならず、どれほど形が似ていても区別できないからです。

失敗は落とせません。診断は代わりに何を書けばいいかを告げます:

```text
$ lhat --check risky.lh
risky.lh:3:1: error: this can fail, and dropping the answer drops the failure with it; write try^ to hand it back, catch^ to answer instead, or a name to bind it and narrow
```

### `f^` は純粋、`p^` は手続き

2 つは別の種別で、その違いは検査されます:

| | 呼べる相手 | 代入できる先 | `yield^` |
| --- | --- | --- | --- |
| `f^` 関数 | 関数のみ | 自分の局所変数と、本体が作ったテーブル | 可 |
| `p^` 手続き | 両方 | 何でも | 可 |

```lhat
let^pure = f^t:t^{ x:number^ } -> number^ {
    let^u = { x := 0 }
    u.x := 1        # よい: この本体が u を作った
    return^u.x
}
```

同じ本体での `t.x := 3` は誤りで、`f^` から `p^` を呼ぶのも誤りです——利用者定義の演算子も含めてなので、演算子は副作用をもちません。

もう半分は `let^` です。導入子は必ず値を伴うので、「ここで宣言して後で 1 度だけ代入する」形が存在しません。Swift や Java が要する確実な初期化（definite initialization）の解析はここに要りません: `let^` の検査は、その名前への `:=` があるかを見るだけです。

### 型は構造的で、モジュールをまたいでも同じ

名前は診断のためのラベルです。同一性は形で決まります:

```lhat
let^needs_writer = t^{ write : p^self^, string^; }
let^use = p^s:needs_writer { s.write("ok") }
```

公称型付けではこう書けません。別の単位にある同じ形の 2 つの `Point` は*同じ型である*——モジュール境界は公称の境界ではありません。公称の島はちょうど 3 つです: 誤りの種別、`enum^`、そしてホスト登録型です。最後の 1 つは、不透明なホスト型には比べるべき構造がないからです。

### `def^` は式であり、`..` は 2 つの役割を持つ

`..` は一般的な連結演算子で、左に何があるかで適用されます:

```lhat
"abc" .. "def"      # 文字列
{1, 2} .. {3, 4}    # テーブル
Base .. def^{ ... } # 定義
```

そのため合成が文字列と同じように読め、`class^` は存在しません: `def^` が唯一の利用者定義型の仕組みで、実体型・抽象型・プロトコル・オブジェクトテンプレート・アスペクトをすべて 1 つで兼ねます。

```lhat
let^Shape = def^{ self^{ label = "shape" }, area = f^self^ -> number^ { return^ 0 } }
let^Square = Shape .. def^{
    self^{ side = 2 },
    override^area = f^self^ { return^self^.side * self^.side }
}
let^s = Square.new()
print($"{s.label} area = {s.area()}")   # shape area = 4
```

`abstract^` メンバがインタフェースの役割です。合成結果が置換可能でなくなるなら、その `..` で拒否されます——誤りは使われた場所ではなく、約束された場所に出ます。

`delegate^` はメンバを 1 つずつ転送するのではなく**借りる**ため、数百クラスのエンジン束縛が扱いられます: 委譲されたメンバは型に加わり、包装する手続きは生成されません。

### 演算子はメンバであり、すべて純粋

`..` や `+` をそのまま名とするメンバで、引数リストのどの位置に `self^` があるかが受け手を決めます。だから `__radd__` は無いのです:

```lhat
let^Vec = def^{
    self^{ x = 0, y = 0 },
    override^new = f^x:number^, y:number^ { self^{ x = x, y = y } },
    op^* = f^self^, k:number^ -> Self^ { def^.new(self^.x * k, self^.y * k) },
    overload^op^* = f^k:number^, self^ -> Self^ { def^.new(k * self^.x, k * self^.y) },
    op^+ = f^self^, o:Self^ -> Self^ { def^.new(self^.x + o.x, self^.y + o.y) },
    tostring = f^self^ -> string^ { $"({self^.x}, {self^.y})" },
}
let^sum = Vec.new(1, 2) + Vec.new(10, 20) * 3
print($"sum = {sum}")   # sum = (31, 62)
print(3 * Vec.new(1, 2))
```

型が書く比較は 1 つ、`op^<=>` で、`<` `>` `≦` `≧` `=` `≠` はすべてそこから読み出されます。集合、複素数、色、ハンドルは代わりに `op^=` を答えます。何が同じかしか言えず、何が先かを言えないものだからです。

オーバーロードは順位付けではなく*探索*で解決されます: 構造的型付けでは「より特殊」が定義された関係ではないため、1 つの呼び出しに適合する候補は高々 1 つで、重なる署名は書かれた場所で拒否されます。

### コルーチンは注釈ではなく推論で分かる

`yield^` を書けば、その手続きは中断できます。伝播させる `async` の印はなく、`Task`/`Future` 型のこともない——コルーチンが保存するのはスタックではなく 1 フレームで、呼んでも呼び出し側は中断しません。`await^` は委譲なので、必要な深さまで届きます。

`yield^` は式です: 値を外に出し、再開時に外から値を受け取ります。

```lhat
let^count_to = f^n:number^{
    var^i = 0
    repeat^until^i >= n {
        var^step:number^|nil^ = yield^i
        i += step ?? 1
    }
}

let^co = count_to(10)
var^got = co.start()
repeat^until^co.done() {
    print(got)
    got := co.resume(2) ?? 0
}
```

そして結果がコルーチンである呼び出しを**文**として書くと**コンパイルエラー**です。型がすでに、その文が何もしないことを示しているからです。

スケジューラは言語の外です。`std.task` は OS スレッドの上に N 台のワーカー機械を立ち続け、それぞれに仕事を手渡して分割して完走させ、`std.channel` はその上の MPMC 待ち行列です。協調的な予算があることで、`yield^` を一度も書かなかった機械も中断できます——[sample/async.lh](sample/async.lh) は、ホストからタイマーと待ちを借りるだけの、スケジューラ全体の L^ による書き下しです。

### `nil^` には一族があり、絞り込みは範囲も知る

`?.` は後置連鎖全体を守り、`?` は値があるか尋ね、`??` は置き換え、`?op=` は存在するときだけ演算子を適用します:

```lhat
var^ t : t^{ string^,string^,string^ }|nil^ = nil^
let^a = t?[0] ?? "100"        # "100"
```

```lhat
var^ count : t^{ number^[] } = { 0, 0, 0 }
var^ i = 0
count[i] += 1     # エラー: これは nil^ でありうるが、nil^ はどの演算子にも答えない
count[i] ?+= 1    # よい
```

絞り込みが効くのは `fits^`、`nil^` との比較、`?`、ループの限界、順序関係、そして抜けるガードです。位置数が決まったテーブル型が、限界の比較相手を与えます:

```lhat
let^bump = p^t:t^{ number^[9] }, d:number^ {
    if^ 1 <= d <= 9 { t[d - 1] += 1 }   # d は 1..9、d - 1 は 0..8
}
```

片側だけでは足りず、分岐が知っていたことはその外へ持ち越えません——次の 2 つはどちらも誤りです:

```lhat
if^ 0 <= d { var^ n : number^ = t[d] }          # 上の側がまだ開いている
if^ 0 <= d <= 8 { } var^ n : number^ = t[d]     # そして分岐は終わっている
```

### 絞り込みが推論の全部ではない

型は値の位置に書かれ、注釈はその本体が検査されるときの要求になります。`strict` と `relaxed` が変わのは*決まらなかった型をいつ報告するか*だけで、ソースの書き方は両方で同一です。したがって 2 つの方言に分かれることがなく、`strict` を通ったコードは `relaxed` でも同じように動きます。片道しか保証しないことが、使う意味のある向きです: `relaxed` は `strict` への踏み台であって、そこから逃げる手段ではありません。ファイルは既定で `strict`、プロンプトは `relaxed` なので、書きかけの行を完成前に送れます。

### 何も暗黙に存在しない

グローバルスコープは存在しません。名前が見えるのは、単位が `require^` で取り込んだか、ホストが `import^` で登録したかのどちらかです——`print` にも例外はありません: 修飾なしの `print("...")` が成り立つのは、`print` が言語に属しているからではなく、ホストが初期束縛として与えたからです。`L^` は機械自身のテーブルで、プログラムは読めて書けません。

`require^` は取り込む側が選んだ 1 つの名前だけを束縛します。モジュールは公開するものを、ファイル末尾の `return` ではなく宣言ごとの `public^` で示すので、**公開される名前は構文解析だけで決まります**。何も実行する必要がありません:

```lhat
module^ lib.greet

public^ let^hello = p^who:string^ { return^ $"hello, {who}" }
let^secret = 1                     # 取り込む側からは見えない
```

### ホストは自作型を言語自身の文法で書く

登録は L^ の構文で書かれた型で、何かが動くより前に検査器がそれを読みます:

```c
lhat_register_func(program, "std.io", "print", "p^string^;", print_fn, NULL);
lhat_register_global(program, "twice", "f^number^ -> number^;", host_twice, NULL);
lhat_bind_initial(program, "twice", "L^.twice");
```

宣言と実装は 1 つですから、乖離しません。引数は検査時に個数と型が確定した配列として届き、誤りは値として返るので、巻き戻しの仕掛けを用意する必要がありません。

## 意図的に採らないもの

以下はいずれも見落としではなく決定であり、仕様が理由を書いています。

| 採らないもの | 理由 |
| --- | --- |
| 例外 | 誤りが値なので「後始末の最中に投げられたらどうなる」という問題が発生しない |
| truthiness | 条件の位置に書けるのは `bool^` だけで、代わりに働くのは絞り込み |
| メタテーブル | 静的に検査する言語は、実行時に型システムを書き換えさせない |
| `==`、`++`、`!=` | `=` が比較、`:=` が再代入で、1 つの考えに 1 つの綴り |
| 自動セミコロン挿入 | 例外の集まりではなく、呼び出し括弧についての規則が 1 つだけ |
| 公称的な同一性 | 例外は 3 つ挙げられ、どれにも理由がある |
| 順位付きのオーバーロード | 構造的型付けに「より特殊」が定義されないため、順位付けの基準がない |
| 分解構文 | 照合は型の仕事、中身を取り出すのは名前の仕事 |
| 型システム上のジェネリクス | パラメトリックな場合は `template^` が受け持ち、可変長ジェネリクスは重い |
| プロセス全体のロケール | プログラムが読めるものは、ホストの言語設定によらず同じバイト列 |

## サンプル

[sample/](sample/) には 17 本のプログラムがあります。まず読む価値があるのは以下です。

### 階乗

`f^` が `this^` 経由で自分自身を呼ぶ——`this^` は囲んでいるサブルーチン自身の署名なので、再帰呼び出しも他と同じように検査されます。1 行です:

![sample/factorial.lh — print(f^n:number^{if^n < 2: 1 el^: n * this^(n - 1);}(10))](media/readme-factorial.svg)

### 合成

[sample/composition.lh](sample/composition.lh) は、ストレージの上にログ層、その上にキャッシュを積むもので、`delegate^` が各ラッパが扱わないメンバを素通しします。そしてそこ全体を——キャッシュヒットが*ロガーに届かない*ことまで含めて——`Store` を構造的に満たすテストダブルで検証します。具体的な型からも継承していません。両方のラッパはテーブル型に対して書かれているので、具体的なストレージの存在を知りません。

### タスク

[sample/hanoi3.lh](sample/hanoi3.lh) は葉っぱの問題ごとにジョブを持つハノイの塔です: `std.task` が 6 ワーカーを起動し、すべての葉がコルーチンで、`Task<number^>` の値はテーブル経由で戻ります。最初の `depth` 段の再帰は呼び出し元の機械に残ります——分割そのものはタスクを動かさないので、その手数をハーネス側で数えています。

### 言語で書いたスケジューラ

[sample/async.lh](sample/async.lh) は完成した協調スケジューラです——タスク表、待ち表、ホストのフレームループ用の `poll()`、ループを自前で持つ側の `run()`——を `def^`、`yield^`、`await^` で書いています。ホストから借りているのはタイマーと待ちだけで、それだけです。

### 24 ゲーム

[sample/24.lh](sample/24.lh): [Rosetta Code の 24 ゲーム](http://rosettacode.org/wiki/24_game)——4 つの数字を配り、プレイヤーがそれらを 1 度ずつ使って 24 になる式を書きます。読み手は `def^` として書いた再帰下降パーサです。

### データをテキストで — LTON

`.lton` ファイルはテキストで書いたテーブルで、ソースと同じ字句解析器が読み、純粋関数しか呼べない文脈で評価されるので、書式が効果をもつことはありません。式が動くのがこの形式の目的です:

```lton
# conf.lton
identity = "lhatove-suite",
window = { title = "test suite", width = 480 },
width = 480 * 2,               # 960 と書く必要はない
name = "lhat" .. "ove",
```

```lhat
import^std.lton
let^conf = std.lton.load("conf.lton") catch^ panic^it^
let^text = try^std.lton.stringify(conf)
try^std.lton.save("conf-copy.lton", conf)
```

LTON のために書かれた検査は 1 つもありません。言語がもともと持っていた規則がそのまま境界になっただけです。

## 言語バインディング

- [Godot](https://github.com/SAM-tak/lhat-gdextension)
- [LOVE 2D](https://github.com/SAM-tak/lhat-love)
- [Unreal Engine](https://github.com/SAM-tak/lhat-UE)（初期段階の実験）

## ツール

同じリビジョンから 2 つのバイナリが出荷されます。`lhat` はドライバ、`lhatls` は言語サーバです。

### `lhatls` — 言語サーバ

ホバー、補完、定義へ移動、参照検索、シグネチャヘルプ、ドキュメント記号、セマンティクストークン、機械的に適用できるものと要確認のものを区別するクイックフィックス、構文木表示。

この決定を貫いているのは、**サーバは型を導ぎ直さない**ことです。検査器が各名前が何に解決したかを記録し、サーバはその表を読みます。スコープの 2 つ目の実装は、検査器とまさに難しい場所で食い違い、そして永久に足並みを揃える必要があります。帰結は「ほぼ正しい」より強い約束です: サーバが出す候補は、検査器が受け入れる候補そのものです。

`LHAT_WITH_RESOLUTIONS` を切ると記録もサーバも一緒に外れます。言語だけを組み込みツールを使わないホストは、どちらも支払いません。

### デバッガ

`lhat --dap=PORT` は、ループバックソケット上の Debug Adapter Protocol セッションでプログラムを走らせます——ブレークポイント、ステップ実行、フレームと束縛の内観、フレームのスコープでの式評価、トレースバック、機械のウォッチポイント。VM にコンパイルされた行フック（`LHAT_WITH_DEBUGGER`）の上にあり、フックを有効にした状態でもループ 1 反復あたりおよそ 50 ns で、その「スレッド」は OS スレッドではなく L^ の機械です。

デバッガが意図的に C 側の製品なのは、スクリプトから呼べる `debug` ライブラリが `f^` の純精神と静的な型に穴を開けることになるためで、その代わりがこの公開 C API です。

### エディタ

- [VS Code 拡張](https://marketplace.visualstudio.com/items?itemName=SAMtak.lhat)
- [VS Code 拡張](https://marketplace.visualstudio.com/items?itemName=SAMtak.lhat)
  —— 言語クライアント、グラフ表示、デバッグクライアント。すべて `lhatls` と `lhat --dap` の上です。

**グラフによる編集。** ビジュアルエディタは同じ言語サーバの別フロントエンドであり、別個の言語ではありません: 検査器と同じ構文木を読み、グラフはテキストであるプログラムの 1 つのビューです。設計文書の 06 章は拡張のリポジトリへ移動しました。処理系を利用するツールは、処理系そのものの仕様ではないからです。[media/factorial-graph.svg](media/factorial-graph.svg) は [sample/factorial.lh](sample/factorial.lh) をグラフにしたものです。

コメントを構文木に付けて保持するのはこのためで、ノードがコメントを持てないグラフは、元のテキストよりずっと乏しくなります。言語だけを組み込みツールを使わないホストは `LHAT_WITH_COMMENTS=OFF` で切れます。

## 必要なもの

- CMake 3.25 以降
- C11 コンパイラ
  - Windows: Ninja プリセットなら Visual Studio 2022 以降、`vs` プリセットは Visual Studio 2026
  - Linux / macOS: GCC か Clang
- [Ninja](https://ninja-build.org/) —— 推奨。Windows では Visual Studio ジェネレータも使えます

## ビルド

### Windows — Ninja + MSVC（推奨）

Ninja は `cl.exe` を直接呼ぶだけでツールチェーンの場所を探さないので、先にシェルへ MSVC の環境をロードする必要があります。`scripts/devshell.ps1` が `vcvars64.bat` 経由で行います:

```powershell
. .\scripts\devshell.ps1      # 先頭のドットに注意: dot-source である必要がある
cmake --preset debug
cmake --build --preset debug
.\build\debug\lhat.exe
```

`debug` を `release` に置き換えると最適化ビルドになります。

VS Code から CMake Tools 拡張でビルドする場合、環境は選択したキットが設定するので `devshell.ps1` は要りません。

### Windows — Visual Studio ジェネレータ

Visual Studio ジェネレータはツールチェーンを自分で見つけるので `devshell.ps1` は要りません。Visual Studio IDE の中でデバッグしたいときに使ってください。

```powershell
cmake --preset vs
cmake --build --preset vs-debug
.\build\vs\Debug\lhat.exe
```

### Linux / macOS

```sh
cmake --preset debug
cmake --build --preset debug
./build/debug/lhat
```

## テスト

スイートは既定でビルドされ、CTest で走ります——85 本を 7 グループに分けています:

```powershell
ctest --test-dir build/debug --output-on-failure
ctest --test-dir build/debug -L check      # core, check, vm, stdlib, lsp, dap, e2e
```

`core` が言語本体、`check` と `vm` が検査器と機械、`stdlib` がサンプル標準ライブラリ、`lsp` と `dap` がツール、`e2e` がプログラム全体です。`e2e` には `install_smoke` も含まれ、木をインストールして `find_package(lhat CONFIG)` でインストール済みのヘッダに対してホストをビルドし、42 と答えないなら失敗します。この 1 本は Debug の木では無効です（インストールに時間をかける価値がないため）。Debug では 85 本のうち 84 本が走ります。

configure 時に `-DLHAT_BUILD_TESTS=OFF` を渡すとスキップします。

CI（`.github/workflows/`）は MSVC、GCC、Clang でビルドしてそれぞれスイートを回し、加えて Clang で ASan と UBSan を有効にしたビルドもあります。

## 実行

ファイルを渡さない場合はプロンプトになります。式だけを書けば答えが返り、構文が続いていれば読み続けます（`--language ja` を付けるとこの案内も日本語になります）:

```text
L^ (lhat) 0.3.8
式だけを書けば答えが返る。構文が続いていれば読み続ける
ctrl-d か空行で終わる
an expression on its own is answered; an unfinished construct reads on
ctrl-d or an empty line ends
> 2 + 3
5
> let^greet = f^n:string^ { $"hi {n}" }
> greet("there")
"hi there"
> let^add = f^a:number^, b:number^ {
.     return^a + b
. }
> add(2, 3)
5
```

ファイルを渡すと、既定ではプログラム全体——その単位と、それが要求するものすべて——を検査してから実行します:

```powershell
.\build\debug\lhat.exe path\to\file.lh
```

| オプション | なにをするか |
| --- | --- |
| *(ファイルなし)* | プロンプトから読む |
| *(既定)* | プログラムを検査して実行する |
| `--run` | プログラム実行を明示する。ファイルの後に続くものはスクリプトの `...` になる |
| `--check` | 型検査して報告する。実行はしない |
| `--ast` | 構文木を表示する |
| `--tokens` | 代わりにトークン列を表示する |
| `--dump-bytecode` | 単位がコンパイルされる先を表示する |
| `--command` | 入力をコマンド形式（`foo 1 2` は呼び出し）として読む |
| `--strict` | 型の誤りをコンパイル時に報告する（ファイルでは既定） |
| `--relaxed` | 決まらなかった型を実行時検査に任せる（プロンプトでは既定） |
| `--compile -o DIR` | プログラム全体を検査してコンパイルし、すべての単位を `DIR` にバイト列として書く。`.lton` は単独でコンパイルできる |
| `--strip-debug` | `--compile` の出力から局所名前と捕捉名を除く |
| `--dump-signatures FILE` | このドライバの登録がつくる署名表を書く |
| `--signatures FILE` | 登録の前に署名表を読む。フロントエンドなしのビルドが登録に使うもの |
| `--dump-host-api [file]` | このドライバが登録するものを JSON で書く（`lhatls` 用）。[sample/lhat-host.json](sample/lhat-host.json) を参照 |
| `--dump-messages DIR` | 英語のメッセージを `DIR` 配下にソースごとに 1 ファイルで書く。翻訳の土台になるカタログ |
| `--messages DIR` | カタログを実行ファイルの横ではなく `DIR` から読む |
| `--language TAG` | システムが読んでいる言語ではなく、その言語で話す |
| `--dap=PORT` | そのループバックポートの DAP でデバッガの下で走らせる（`--run` を含む） |
| `-h`、`--help` | 使い方を表示して何もせず終わる |
| `-v`、`--version` | バージョンを表示して何もせず終わる |

`--ast`、`--tokens`、`--dump-bytecode`、`--command` はいずれもファイルを読みます。ファイルがなければ使い方を表示します。

### 多言語化

`messages/ja/` に日本語カタログがあります。メッセージは固定の文字列 ID と穴を持ち、英語が正で、他の言語はその翻訳です——`--dump-messages` が翻訳者のために英語を書き出します。選択は単位ごとに可能で、プロセス全体を設定する方式ではありません。

述べる価値のある不変条件は、**プログラムが読めるものは、ホストがどの言語に設定されていても同じバイト列である**、ということです。対象は `tostring` の答え、数の書き出し、`typeof^` の綴り、LTON の読み書きです。それらを比較・保存・送信する 2 つのプログラムが、実行した人の設定で違う挙動を示すことはありません。

## 組み込み

言語は `lhat.lib` で、`lhatport.lib` はメモリがどこから来るかと単位のテキストをどう読むかだけです。ホストは `include/` をパスに置いてヘッダを 1 つ名指します:

```c
#include "lhat.h"
```

`src/` には `parser.h` や `type.h` のような名前があります——誰かの include パスに置くにはありふすぎるので、`src/` の**何も**インストールされません。エディタで L^ に色を付けるホストは `lhat/lexer.h` も名指しますが、これが唯一の別の公開ヘッダで、バイトコードだけを走らせるホストは決して見ません。

ホストは 1 つの単位を検査し、コンパイルし、インストールし、走らせます。これは [tests/install_smoke/host.c](tests/install_smoke/host.c) の全体からエラー処理を省いたものです:

```c
LhatProgram *program = lhat_program_new(/*strict=*/true, load_main, NULL);
lhat_register_global(program, "twice", "f^number^ -> number^;", host_twice, NULL);
lhat_bind_initial(program, "twice", "L^.twice");

const LhatUnit *root = lhat_program_check(program, "main.lh");

LhatMachine *machine = lhat_program_compile(program) ? lhat_machine_new() : NULL;
lhat_program_install(program, machine);
LhatRunResult ran = lhat_run(machine, lhat_unit_proto(root));

lhat_machine_dispose(machine);
lhat_program_free(program);
```

登録は検査より前に置きます。検査器が署名の意味を知る必要があるからです。インストールは実行より前で、それが「登録したものが `L^` に届く」瞬間だからです。

この形のほかに、API はホストが実際に必要とするものを覆っています: 24 個の登録呼び出し（型、メンバ、ホストデータ、ホスト値、`enum^`、誤りの種別、アノテーション、定数、インスタンス化検査）、コルーチンとスケジュール（`lhat_machine_resume`、`lhat_machine_set_budget`、`lhat_machine_call`）、デバッガ、単位と公開分の内観、バイナリ単位と署名表、そしてアロケータ。

### ホットリロード

エディタの保存は 1 つの呼び出しです:

```c
lhat_reload(program, "lib.lh", machines, machine_count);
```

無効化し、各機械でその単位を忘れ、再検査し、再コンパイルし、退避させた本体はどの機械もクロージャを保持しなくなったと確認してからしか解放しません。タイミングを自分で握りたいホストのために、それぞれの段は公開されています。

### ポートの差し替え

`lhatport` はメモリがどこから来るかと、単位のテキストをどう読むかだけです。

静的なホストが自分の `port/alloc.c` と `port/loader.c` をコピーして 4 つの関数を書き換え、ライブラリをリンクから外します——コアは `lhat_alloc` とその仲間を、あるところにあるものに対して解決するので、間接層もなく、登録するものもありません。

共有ビルドはその継ぎ目を使えません。DLL はホストに見えるより先にリンクされるからです。そのため既定は `lhat_set_allocator` を通してアロケータを受け取ります。何かが確保される前に呼ばなければならず、そうでなければ false を返すことでそれを知らせます。

ローダは既定では決して用意されません: `lhat_program_new` がそれを取り、`NULL` はどの単位も読めないことを意味します——したがって、組み込まれたものが指示されない限りファイルシステムには届きません。[include/lhat/port.h](include/lhat/port.h) と 05 §8.9 を参照。

## プリセット

| configure プリセット | ジェネレータ | build プリセット | バイナリ置き場 |
| --- | --- | --- | --- |
| `debug` | Ninja、`Debug` | `debug` | `build/debug` |
| `release` | Ninja、`Release` | `release` | `build/release` |
| `asan` | Ninja、`Debug` + サニタイザ | `asan` | `build/asan` |
| `pgo` | Ninja、`Release` + PGO 計測 | `pgo` | `build/pgo` |
| `vs` | Visual Studio 2026（マルチコンフィグ） | `vs-debug`、`vs-release` | `build/vs` |
| `vmonly` | Ninja、`Release`、VM のみ | `vmonly` | `build/vmonly` |

`ctest` プリセットは `debug`、`release`、`asan` にあり、`outputOnFailure` は設定済みです。

Ninja のプリセットはシングルコンフィグで、ビルド種別は configure 時に決まります。Visual Studio のプリセットはマルチコンフィグなので、build プリセットが選びます。

すべてのプリセットが `CMAKE_EXPORT_COMPILE_COMMANDS` を設定しますが、`compile_commands.json` を実際に出すのは Ninja のプリセットだけです——Visual Studio ジェネレータは対応していません。clangd には `build/debug/compile_commands.json` を指定してください。

`vmonly` はフロントエンドをまったく持たないコアをビルドします: 字句解析器も構文解析器も検査器もコンパイラも入れません。`--compile` が書いたバイナリ単位を読み、署名の表でホストを登録するので、配るランタイムはソース言語の機構もテキストも運びません。この木を MSVC Release でビルドすると `lhat.exe` は 685 KB から 422 KB に、`lhat.lib` は 2.1 MB から 969 KB になります。

`scripts/pgo.ps1` が PGO の 2 つの相を動かします——`GENERATE` が計測を入れ、`bench/train/` の訓練がプロファイルを書きます。`USE` がそれを使って再リンクします。

## ビルドオプション

| オプション | 既定 | 何を制御するか |
| --- | --- | --- |
| `LHAT_WITH_FRONTEND` | `ON` | 字句解析器・構文解析器・検査器・コンパイラ。`OFF` が `vmonly` で、LSP とスイートには `ON` が必要 |
| `LHAT_WITH_DEBUGGER` | `ON` | VM の行フック、フレームの内観、機械の監視。`LHAT_BUILD_DAP=ON` には `ON` が必要 |
| `LHAT_WITH_COMMENTS` | `ON` | コメントを保持して構文木に付けること |
| `LHAT_WITH_RESOLUTIONS` | `ON` | 各名前が何に解決したかの記録。`LHAT_BUILD_LSP=ON` には `ON` が必要 |
| `LHAT_BUILD_STDLIB` | `ON` | `stdlib/` のサンプル標準ライブラリ |
| `LHAT_BUILD_LSP` | `ON` | 言語サーバ |
| `LHAT_BUILD_DAP` | `ON` | ドライバに畳み込まれたデバッグアダプタ |
| `LHAT_BUILD_CLI` | `ON` | コマンドラインドライバ |
| `LHAT_BUILD_TESTS` | `ON` | テストスイート |
| `LHAT_BUILD_BENCH` | `OFF` | メンバ読みと検査コストのベンチマーク |
| `LHAT_SANITIZE` | `OFF` | AddressSanitizer。コンパイラが持つところには UBSan も |
| `LHAT_PGO` | `OFF` | `OFF`、`GENERATE`、`USE` のいずれか |

## ディレクトリ構成

```text
CMakeLists.txt        ビルドの定義
CMakePresets.json     configure / build / test のプリセット

include/lhat.h        ホストが名指す唯一のヘッダ
include/lhat/         残りの公開面と、生成される version.h

src/                  言語本体                                -> lhat.lib
  source.c              単位の読み込み。改行と BOM の正規化
  error.c               各段階が何を報告するかの一つの形
  message.[ch]          メッセージの ID・穴・描画された文
  number.[ch]           整数と実数。型は 1 つ、表現は 2 つ
  token.c               トークンの定義
  lexer.c               字句解析
  ast.[ch]              構文木の節点とそのアリーナ
  parser.[ch]           構文解析
  type.[ch]             型：構築と適合
  check*.[ch]           型検査：式・文・初期化
  semantic.c            単位の名前・型・メンバ
  completion.c          カーソルの後ろに何が続きうるか
  fix.c                 クイックフィックスと、それが何に効くか
  program.c             単位のグラフと、ホストが登録するもの
  registry.[ch]         ホストの登録
  rttype.[ch]           実行時の型の記述
  code.[ch]             バイトコード、チャンク、コンパイル済み単位
  compile.[ch]          構文木からバイトコードへ
  serialize.[ch]        バイナリ単位と署名表
  vm*.c                 コード生成と機械
  machine.h             機械の中身：スタック、フレーム、ヒープ
  gc.[ch]               コレクタ：mark and sweep、一歩ずつ
  value.c               実行時の値
  object.c              ヒープの値
  debug.c               行フックとフレームの内観
  port.h                言語が周囲に求めるもの

port/                 メモリ・ファイル・スレッド・ソケットの既定
  alloc.c               malloc と、DLL が必要とする登録
  loader.c              単位をファイルから読む
  thread.[ch]           OS スレッド。コアの隣で使うもの
  socket.[ch]           ループバックソケット。デバッグアダプタ用
  -> lhatport.lib, lhatthread.lib, lhatsocket.lib

stdlib/               サンプル標準ライブラリ（C 実装）                   -> lhatstdlib.lib
  io, json, thread, random, regex, math (+ complex, quaternion, vector2/3/4),
  debug, async, channel, task, lton, load, error, carry

cli/main.c            コマンドラインドライバとプロンプト                   -> lhat.exe
lsp/                  言語サーバ                               -> lhatls.exe
dap/                  デバッグアダプタ。ソケット上の DAP                 -> lhatdap.lib
transport/            ストリーム上の Content-Length フレーミング
vendor/cjson/         JSON。上の 2 つのため
messages/ja/          メッセージカタログ
bench/                メンバ読みと検査コストのベンチマーク、PGO 訓練用
tests/                テストスイート（CTest）。install_smoke を含む
sample/               サンプル 17 本
DesignDocuments/      言語の設計仕様（日本語）
media/                ロゴとサンプルの描画
scripts/              devshell.ps1、pgo.ps1、install_smoke.cmake
cmake/                CMake パッケージ設定のテンプレート
Memo.md               言語の設計ノート（発散。仕様ではない）
```

`source.[ch]`、`error.[ch]`、`token.[ch]`、`lexer.[ch]`、`value.[ch]`、`object.[ch]` と `port.h` は `.c` で代表させています。これらのヘッダは公開で `include/lhat/` にあります。L^ に色を付けるホストには字句解析器が、単位を読むホストにはソースの規則が必要だからです。

パイプラインは左から右へ `source` → `lexer` → `parser` → `check` → `compile` → `vm` と流れ、`program` が単位のグラフを辿るので、単位はそれが要求するものすべての検査のあとで検査されます。構文木は必須です: 型推論はソースの後方にある情報を必要とするので、読みながらバイトコードを出す 1 パスでは成立しません。

`vm_internal.h` は `LhatMachine` を不透明に保つので、機械であるファイル——走らせる `vm*.c` と、根を見る必要がある `gc.c`——は `machine.h` を共有します。

## 設計文書

仕様が言語の権威ある記述で、ソースは至る所で章番号を引いてこれを引用します。文書は日本語で、[その索引](DesignDocuments/README.md)にはまだ決まっていないことが並びます。

| 文書 | 内容 |
| --- | --- |
| [01-lexical-structure.md](DesignDocuments/01-lexical-structure.md) | 文字、トークン、リテラル、コメント、スコープ指定子 |
| [02-syntax.md](DesignDocuments/02-syntax.md) | 文、演算子、型、オブジェクトモデル、サブルーチン、コルーチン、パターンマッチ |
| [03-compilation-pipeline.md](DesignDocuments/03-compilation-pipeline.md) | 4 段階、厳格度、推論、値の表現、バイトコード、コレクタ |
| [04-errors.md](DesignDocuments/04-errors.md) | `errordef^`、`try^`、`catch^`、網羅性、取りこぼし |
| [05-modules.md](DesignDocuments/05-modules.md) | 単位、`require^`、`import^`、`L^`、ホストが提供するもの |
| [07-language-server.md](DesignDocuments/07-language-server.md) | `lhatls`：何に答え、何から答えるか |
| [08-lton.md](DesignDocuments/08-lton.md) | LTON、テーブルをテキストで書く形式と、なぜ安全に読めるのか |
| [09-debugger.md](DesignDocuments/09-debugger.md) | 行フック、フレームと束縛の内観、DAP アダプタ |
| [10-localization.md](DesignDocuments/10-localization.md) | 観測可能な文字列の不変条件、メッセージの ID、カタログ、言語の選択 |

06 は欠番です。ビジュアルエディタの設計は、それを実装するツールの隣、拡張のリポジトリへ移動しました。

## ライセンス

Apache License 2.0. [LICENSE](LICENSE) を参照。
