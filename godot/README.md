# L^ (lhat) GDExtension

Godot が実行時に読み込む共有ライブラリとして L^ を提供する側。
godot-cpp に対して書くので、ツリーの中でここだけが C++。言語本体は C11 のまま、
`include/lhat.h` 越しに——他のホストと同じ面で——呼ばれる（05 の 8.7）。

## L^ はエンジンのスクリプト言語である

`Engine.register_script_language` で登録するので、エディタの
「スクリプト作成」ダイアログの言語に **L^** が出る。`.lh` は
`ResourceFormatLoader` / `Saver` を通るので、開く・保存する・`load()` する
のいずれもエンジンの普通の道を通る。

- `LhatLanguage : ScriptLanguageExtension` — 拡張子 `lh`、型 `LhatScript`。
  `_validate` が**型検査器そのもの**で、**保存前のバッファ**に対して走る。
  ゆえに何も実行しなくても誤りが出る（03 の 1.1）
- `LhatScript : ScriptExtension` — 本文・検査結果・`reload()`

**コードの脇に並ぶメンバ一覧**（members overview）も `_validate` が答える。
エンジンは `"functions"` の欄に `"名前:行"` を並べたものを求めており
（`ScriptTextEditor::get_functions`）、そこに**`p^` か `f^` に束ねられた名前**を
入れている。GDScript が `func` だけを並べるのと同じ選び方で、それ以外のメンバは
場か 14.7改 の静的な定数なので、読む場所はインスペクタのほうである。

木ではなく**本文**から読む。行番号は画面のバッファと合っていなければならず、
`public^let^` や `override^` のような前置きと、同じ行に立つ注釈
（`@signal died = p^self^, …`）を剥がしてから名前を取る。
`_get_member_line` の飛び先も同じ読み方を通る。

### `.lh` は2種類ある — `module^` を書いたかどうか

エンジンはプロジェクト内の `.lh` を**すべて**スクリプト資源として読む。
L^ は 02 の 8.2 のとおり最上位に文が書けるので、「クラスを宣言するファイル」と
「文の並び」の両方が `.lh` である。**分かれ目は `module^` を書いたかどうか**で、
これはコンパイルの時点で決まる（05 の 3.2）。

| | `module^` 有り | `module^` 無し |
| --- | --- | --- |
| 何であるか | クラス／ライブラリ | エディタスクリプト |
| 読み込んだとき | 最上位が走る（宣言だけのはず） | **走らない** |
| ノードに着せる | できる（下の条件を満たせば） | できない |
| File > Run（Ctrl+Shift+X） | Godot が拒否する | **本文が走る** |

```lhat
# module^ 無し。これだけで File > Run が動く
let^ greeting = require^"lib/greeting.lh"
print(greeting.greet("Godot"))
```

**`_run` は書かない。** エンジンは `EditorScript::run()` から
`GDVIRTUAL_CALL(_run)` を通ってインスタンスの `_run` を呼ぶが、その名前で
呼ばれたら拡張が**本文を走らせる**。書き手から見て「実行」とはファイルの中身
そのものであり、置き場所を1つ余計に覚えることはない。

GDScript は最上位に文を書けないので、同じことを3つ書いて表す。

```gdscript
@tool
extends EditorScript

func _run() -> void:
    print("hello")
```

`.lh` はどれも要らない。`module^` を書かないことが「これは走らせるもの」と
言っている。

［補足］ **読み込みで走らせないことが要点**である。エンジンは保存のたび、
プロジェクトを走査するたびに `reload()` を呼ぶ。File > Run 自身も
`EditorNode::run_editor_script` の1行目で必ずハードリロードする。最上位に
`print` のあるファイルがそこで走ると、押していないのに走ったように見える。

［補足］ エディタの中だけである。`EditorScript` はエディタのクラスなので、
書き出したゲームには存在しない——`_can_instantiate` は
`Engine.is_editor_hint()` を答える。

`module^` を書いたファイルが File > Run で走らないのは Godot の規則そのままで、
拒否の文言もエンジンが出す——`EditorScript` を継承していない、`@tool` でない、
の2つ。クラスを走らせたければノードに着せる。

### 新規スクリプトのテンプレートは3つ

「スクリプトをアタッチ」「新規スクリプト」ダイアログの **Template** 欄に、
`.lh` の3種が並ぶ。上の2種類がそのまま3つになっている——着るもの、
引かれるもの、走らせるもの。

| 紐付け | 名前 | 中身 |
| --- | --- | --- |
| `Node` | Node script | `module^` ＋ `@game` ＋ 包みを継承した `def^` 1つ |
| `Object` | Module | `module^` ＋ `public^let^` の関数。`def^` 無し |
| `EditorScript` | Editor script | `module^` 無し。親クラスの指定は使わない |

**並ぶのは親クラスに合うものだけ**である。エディタは親クラスの継承鎖を辿って
集めるので、`Sprite2D` のノードに着せる流れでは `Node` と `Object` の2つが出て、
`EditorScript` は出ない。GDScript も同じ仕組みで、あちらの10個のうち
`CharacterBody2D` 用や `EditorPlugin` 用が普段見えないのはこのためである。

置換される綴りは GDScript と同じ4つ——`_BASE_`（親クラス名）、`_CLASS_`、
`_CLASS_SNAKE_CASE_`、`_TS_`（字下げ1段）。**新しい綴りを足していない**ので、
GDScript 用に書いた雛形の書き方がそのまま通じる。

```lhat
module^spinner

# 包みはプロジェクトのもの。Godot.Sprite2D が無ければ書き足す。
let^Godot = require^"lhat/Godot.lh"

@game
public^let^Spinner = Godot.Sprite2D..def^{
    self^{
        @export speed = 1,
    },
    …
}
```

**`module^` の名前はファイル名から来る。** ダイアログが渡すのは basename
だけでパスは渡らないので、`spinner.lh` なら `module^spinner` にしかならない。
入れ子の名前（`demo.spinner`）にしたければ書き直す。

`@tool` 版は無い。`@game` を1語書き換えるだけなので、GDScript も持っていない。

［補足］ 利用者の雛形も効く。`res://script_templates/<クラス名>/*.lh` に置くと
一覧の「Project」に出る。名前と説明は最初の行に `# meta-name:`
`# meta-description:` で書く——エディタが読むのは言語の拡張子と、空白を含まない
最初のコメント綴り（L^ では `#`）である。

### ノードに着せられる

**着せられる単位の条件は2つ** — `module^` であること、着るクラスが
`public^` な `def^` として在ること。

- `module^` は単位の答えを `L^.modules` に置く。これが GC の根なので、
  エンジンがノードを持っている間、定義が回収されない
- `public^` は 05 の 5.5 のとおり単位の答えに載る条件であり、
  ホストが値に手を届かせる唯一の道である

```lhat
module^ demo.counter

public^let^ Counter = def^{
    self^{ ticks = 0 },
    _ready = p^self^ { print("ready") },
    _process = p^self^, delta:number^ { self^.ticks := self^.ticks + 1 },
    count = f^self^ -> number^ { return^ self^.ticks },
}
```

#### どれを着るかは `@game` か `@tool` が言う

```lhat
@game
public^let^ Spinner = Godot.Sprite2D..def^{ … }

public^let^ Helper = def^{ … }      # 同じファイルで公開してよい
```

公開クラスが1つなら**どちらも書かなくてよい**——選ぶものが無いので。

**印は単位を通して多くて1つ。** どちらも 02 の 18.5 の `FILEUNIQUE` で登録して
あるので、**同じ印を2度書けば検査器がその場で弾く**（行番号付き）。`@game` と
`@tool` の併記も同じで、両者は 18.5.1 の排他として互いを挙げてある——一つの
問いへの二つの答えなので、**どちらか一方だけを書く**。公開クラスが2つ以上あって
印が無い場合は、**着せようとした時に**言う——`require^` されるだけのライブラリ
なら、どれを着るかを答えないのは何も間違っていないので（05 の 5.5 のとおり、
複数のクラスを答えるのがクラスのライブラリの姿）。

**どちらも公開された束縛にしか書けない**（02 の 18.4改 の的）。私有の `def^` は
単位の答えに載らないので、印を付けてもエンジンは値に手が届かない
——書けてしまうと黙って効かないため、検査器がその場で弾く。

```lhat
@game
let^ Hidden = …    # error: this annotation was not registered for what it
                   #        is written above: game
```

［補足］ 私有の `def^` は元から何個でも書けた。単位の答えに載るのは `public^` な
名前だけなので、制限は公開したいときにだけ掛かっていた。

以前は「`public^` な `def^` がちょうど1つ」という規則だった。**それは言語が
関与していない規約**であり、2つ目のクラスを公開した瞬間にそのファイルが
スクリプトでなくなる、という代償を払っていた。18.1 のとおり
「どれが何のためか」を言うのは注釈の仕事である。

#### `@tool` は編集中「も」動く

`@game` と `@tool` の違いは**いつ動くか**だけである。

- 無し / `@game` — ゲーム実行時に動く。シーン編集中は動かない
- `@tool` — ゲーム実行時に動き、**シーン編集中も動く**

`@tool` は加算であって「エディタ専用」ではない。Unity の `[ExecuteAlways]` と
同じもので、`@game` にエディタを足したものである。だから両方は書けない
——一つの問いに二つの答えがあるだけで、問いが二つあるのではない。

編集中は、`@game` のクラスを着たノードには**実体が作られない**。代わりに
エンジンの placeholder が入り、`@export` の欄はインスペクタに出るが
`_ready` も `_process` も走らない。書き手が意図しないコードがシーンを開いた
だけで走らないための線であり、GDScript が引いているのと同じ線である。

［補足］ placeholder が出す欄の初期値は **14.11 のプロトタイプの値**である。
定義は自分の雛形を `self^` にぶら下げており、初期化式は定義を組むところで
一度だけ走っている——だから実体を作らずに読める。14.15 の `abstract^` な欄
だけはプロトタイプにキーが無く、そこは型の零（`0` / `""` / `false`）に落ちる。

`@tool` を書いた側では、編集中かどうかを訊けたほうがよいことが多い。

```lhat
_process = p^self^, delta:number^ {
    if^ godot.isEditorHint() { return^ }
    self^.setRotation(self^.getRotation() + delta)
},
```

`_ready` も `_process` も、エンジンが GDScript を呼ぶのと同じ経路で呼ばれる。
`_process` が毎フレーム回るのは `has_method` が真を答えるからで、
書いていない単位は処理を有効化されない。

**メンバは共有、`self^` の欄はノードごと**（02 の 14.3）。同じファイルを着た
2つのノードは別々に数える。`.lh` 1枚につき `LhatProgram` と `LhatMachine`
が1組、ノード1つにつきインスタンス1つ。ノードごとに機械を持つ形ではない。

インスタンスは `L^.modules.godot.script.instances` の下に置かれる。
これも根のため。ノードが消えるとそこから外れる。

#### 定義のものは定義から読む

02 の 14.7 は、インスタンスが呼べるのは**受け手を取るメンバだけ**だと定める。
残りは定義のものである——`new`（14.11）と、受け手を取らない静的メンバと、
値のメンバ（14.7改 の言う「静的な定数」）。エンジンにも同じ線で答える。

- **`self^` の欄** → スクリプトの members。ゲームを走らせている間、
  リモートインスペクタの `Members/` に並ぶ
- **値のメンバ** → スクリプトの constants。同じく `Constants/` に並び、
  GDScript からは `get_script_constant_map()` で読める。継承の鎖は畳まれて
  いるので、`Godot.Sprite2D..def^` を継いだクラスの `gdBaseClass` も出る
- **受け手を取らないメンバ** → 静的メソッド。`has_method` が真を答える

```gdscript
var Probe := load("res://probe.lh")
Probe.get_script_constant_map()      # { "limit": 7, "gdBaseClass": "Sprite2D" }
Probe.has_method("twice")            # true
Probe.call_static("twice", [21])     # 42
```

`ScriptExtension` にはスクリプト資源そのものへの呼び出しの口が無い
（`Object::callp` は ClassDB までしか行かず、言語には訊かない）。だから
**呼ぶ道は `call_static` ただ一つ**である。

**`new` はそこに載せない。** エンジンには既に実体を作る道がある
——スクリプトをノードに着せれば `_instance_create` が走る——ので、
`has_method("new")` は偽を答え、`call_static("new", …)` は理由を言って断る。
二つ目の扉から作ったインスタンスはノードを持たず、`instances` にも載らない。

### プロジェクト全体で名乗るのは `@export_class` を書いたときだけ

**既定は無名である。** GDScript が `class_name` を書かなければ無名なのと同じ
側に立つ——書かなくてもスクリプトは普通に動き、ノードにも着せられ、参照は
パスで行う。Create Node にもヘルプのクラス一覧にも出ないだけである。

```lhat
@game
@export_class
public^let^ Spinner = Godot.Sprite2D..def^{ … }
```

これがあってはじめて `@icon` のアイコンが付き、GDScript から
`var s: Spinner` と書け、Create Node に並ぶ。

**名乗る名前は束縛の名前**で、注釈は引数を取らない。名前を2箇所に書けると
食い違えるためである。`module^` の綴りも混ぜない——**Godot の登録は入れ子の
無い平坦な1つの名前空間**なので、`demo.spinner.Spinner` を渡しても名前空間
としては読まれず、ドットを含む1つの名前になるだけで、GDScript から型として
書けなくなる。

**1ファイルに1つ**（`FILEUNIQUE`）。エンジンが覚えるのも「パス1つに名前1つ」
である。

一意性は保証されない。2つの `.lh` が `Spinner` を名乗れば両方主張する。
GDScript の `class_name Spinner` と同じ状況である。**エンジン自身の名前
（`Node` など）とだけは衝突させない**——足元から名前を奪うのは名前1つ以上の
被害になるので、その場合は登録しない。

［補足］ L^ 側に名前が戻ってくることはない。エンジンが持つのは「名前 →
パス」の対応で、パスはスクリプト資源、資源は定義そのものを握っている。
`L^.modules.…` を名前から引き直す場面が無いので、対応表も要らない。

［補足］ エディタは走査した結果を `.godot/global_script_class_cache.cfg` に
覚える。印を外しても、走査が回り直すまでは一覧に残って見える。

#### `gdBaseClass` が「どのエンジンクラスを包むか」を言う

Create Node が並べるのは **Node 派生**だけなので、着るクラスは自分が何を
包んでいるかを言う必要がある。定義のメンバ1つで言う。

```lhat
public^let^ Object = def^{ gdBaseClass = "Object", … }
public^let^ Node2D = Node..def^{ override^gdBaseClass = "Node2D", … }
public^let^ Sprite2D = Node2D..def^{ override^gdBaseClass = "Sprite2D", … }
```

`Godot.Sprite2D..def^` を継承した `Spinner` は書かなくても受け取る。14.12 が
派生に `override^` を書かせ、`compile_def` が継承の鎖を1つの表に畳むので、
一番近いものがそのまま読める。**言語には何も足していない。**

これは**下限**であって、そのクラスちょうどではない。`Node2D` 用に書いた
クラスは `Sprite2D` にも `Bone2D` にも貼れる。だから `className()` は別に要る
——あちらは「実際に何に貼られたか」で、ずれるのはまさにこの場合。

**下限を満たさないノードは着られない。** `Sprite2D` を包むクラスを `Bone2D`
に着せようとすると、実体を作る所で拒まれてエラーになる。ゲームでは、その
ノードはスクリプトの着いていないノードになる。

`module^` の無い `.lh` も同じ所で拒まれる——包んでいるのは `EditorScript`
で、ノードはそれではない。

エディタは着せる所で型を見ない——スクリプトをノードにドロップしても、
インスペクタの Script 欄に入れても、型は見られていない。GDScript も同じで、
拒むのは言語側（`GDScript::instance_create`）である。L^ も同じ場所で拒む。

`@game` は**編集中は黙っている**。編集中のそれは placeholder で、実体では
ないからである。エラーはゲームを実行した瞬間に出る。`@tool` なら編集中に
実体を作るので、着せたその場で出る。これも GDScript と同じ線である。

**編集中に拒んだときは placeholder を返す。ここだけ GDScript より親切。**
実体を作れなかったノードは `script` に nil を答えるので、そのまま保存すると
**`.tscn` から `script` 行も `@export` の値も消える**——シーンを開いて保存
しただけで消える。土台を書き換えてしまうのは編集中に起きることで、そこで
作業が消えるのは高すぎる。実測:

```text
@tool の GDScript を着せた Bone2D のシーンを開いて pack し直す
  → [node name="Bone" type="Bone2D"]              script 行ごと消える
同じことを @tool の L^ で
  → script = ExtResource(…) / mark = 7            残る
```

ゲームでは placeholder を返さない。placeholder は何の呼び出しにも答えず
`self^` も持たないので、黙って着ているノードは着ていないノードより悪い。

書かない定義（包みを継承していない `def^`）は `Object` に落ちる。あらゆる
オブジェクトなので何も拒まないが、Create Node にも出ない。

### エンジンのクラス木は L^ 側に書く

05 の 8.8 のホスト型は**名前的**で、適合は同一性のみ（`src/type.c` の
`return value == target;`）。Godot の 971 クラス・深さ8 の継承木を
971 個のホスト型にしても互いに無関係になり、`add_child(Node)` に
`Sprite2D` を渡せない。

だから **hostdata 型は `godot.Object` 1つだけ**にして、木は L^ 側に
`def^` の連鎖として書く。14.10 の幅部分型が正しい向きに効くので、
`CharacterBody3D` のラッパーは `Node3D` が要る所にそのまま通る。

```lhat
module^ lhat.Godot
import^ godot

public^let^ Object = def^{
    self^{ abstract^ gdobj : godot.Object },

    # ノードは new() の引数で届く
    override^new = f^obj:godot.Object { self^{ gdobj = obj } },

    className = f^self^ -> string^ { return^ self^.gdobj.className() },
}

public^let^ Node2D = Object..def^{
    getRotation = f^self^ -> number^ { return^ self^.gdobj.get("rotation") as^ number^ },
}

public^let^ Sprite2D = Node2D..def^{ … }
```

**ノードは `new()` の引数として渡る。** `_instance_create(Object *for_object)`
の時点でエンジンは既にオブジェクトを作り終えているので、`new` を呼ぶ前に
本物が手元にある。14.11 の生成はプロトタイプの複製で、`override^new` の本体が
その複製にノードを書き込む——**構成子がノードを触れるのはこの順序だけ**である。
返ってから欄に書き込む形では、本体が無効なハンドルしか見られない。

場は `abstract^` のままでよい、というより `abstract^` でなければならない。
ホストオブジェクトは可変値なので既定値としてプロトタイプに置けず（14.11）、
14.15 が生成を拒むのは既定の `new()` では場が埋まらないからで、
14.11改 の `override^` はその既定を消す。
作る道が場を埋める道しか無くなるので、置き場所用の仮値が要らない。

ラッパを使わない素の `def^` も着られる（`demo/counter.lh`）。そちらは既定の
`new()` のままなので、ホストは `new(obj)` を試して引数数で外れたら `new()`
に落ちる。

```lhat
module^ demo.spinner
let^ Godot = require^"lhat/Godot.lh"

public^let^ Spinner = Godot.Sprite2D..def^{
    self^{ turns = 0 },
    _process = p^self^, delta:number^ {
        self^.setRotation(self^.getRotation() + delta)
    },
}
```

`godot.Object` が持つのは**ポインタではなく `ObjectID`**。解放済みなら
`instance_from_id` が空を返すので、ダングリングしない。`RefCounted` の
ときだけ `Ref<>` を併せ持つ（それ以外に生かすものが無いため）。メンバは
`isValid()` / `className()` / `isClass()` / `get()` / `set()` /
`call()` / `emit()`。

**無から作る道は無い。** ここの `godot.Object` はどれもエンジンが作った
オブジェクトを指しており、その出どころは `override^ new` に渡ってくる1つだけ
である。「オブジェクトが無い」を表す値を置けば、`isValid()` が構造上必ず
偽になるラッパを作れてしまい、それに使い道が無い。

［補足］ `isValid()` は残る。ノードが解放されれば `ObjectID` の世代が合わなく
なるので、**在ったものが無くなる**問いは今も要る。

合成先は `import^ godot` を書かなくてよい。平坦化された基底の本体は
**自分の単位の名前空間で解決される**（その単位の `import^` の根と
`public^` な名前）。基底の単位が自分だけに留めた `let^` は届かず、
そう言う誤りが出る。

`_get_reserved_words` の語は `vscode-extension/syntaxes/lhat.tmLanguage.json`
の分類をそのまま使った。言語側は一覧を持たない——01 の 2 章 は
ハット付きの語を一律に読み、知らない語を弾くのは意味解析である。

### 数学の型は値である

`Vector2` は数が2つ並んだだけのもので、ポインタも参照も、回収器が覗く必要の
あるものも入っていない。**05 の 8.9 のホスト値**はまさにその形である
——バイト列がそのまま機械のスタックに乗り、`v.x` はホスト呼び出しなしに
読み書きし、算術はヒープを触らない。

16 種すべてが在る:

```lhat
import^godot

let^v = godot.vector2(3, 4)
print($"{v} x={v.x}")            # (3.0, 4.0) x=3.0
print($"{v + godot.vector2(1, 1)}  {v * 2}  {2 * v}  {-v}")

let^r = godot.rect2(v, godot.vector2(30, 40))
print($"{r.position()} {r.size()}")

let^b = godot.basis(godot.vector3(1, 0, 0), godot.vector3(0, 1, 0), godot.vector3(0, 0, 1))
print($"{b * godot.vector3(4, 5, 6)}")   # 基底をベクトルに適用
```

- **素の欄**（`Vector2` の `x`、`Color` の `r`、`Plane` の `d`）は
  **直に読み書きできる**。8.9 の欄はバイト列への直アクセスで、呼び出しが挟まらない
- **入れ子の部分**（`Rect2` の `position`、`Transform3D` の `basis`）は
  **読むだけ**のメンバ（`r.position()`）。ホスト関数が受け取るのはバイト列の
  控えなので、そこへ書いても呼び手には届かない。**変えるときは作り直す**
  ——値型とはそういうものである
- **等価はバイトで比べる**（`hostvalue_equal`）。`op^=` は要らない
- **メソッドはエンジンのものをそのまま**。名前だけ L^ の綴りに直してある
  （`length_squared` → `lengthSquared`）:

  | 型 | 答えるもの |
  | --- | --- |
  | `Vector2` `Vector3` `Vector4` | `length` `lengthSquared` `normalized` `dot` `distanceTo` `lerp` |
  | `Vector2` だけ | `angle` `angleTo` `cross`（数）`rotated` |
  | `Vector3` だけ | `cross`（ベクトル） |
  | `Vector2i` `Vector3i` `Vector4i` | `length` `lengthSquared`（単位ベクトルは整数に無い） |
  | `Color` | `inverted` `luminance` `lerp` |
  | `Quaternion` | `length` `normalized` `dot` `inverse` `slerp` |
  | `Plane` | `normalized` `distanceTo` `hasPoint` |
  | `Rect2` | `hasPoint` `intersects` `center` `merged` |
  | `Rect2i` `AABB` | `hasPoint`（`AABB` は `center` も） |
  | `Transform2D` | `affineInverse` `rotation` `scale` `basisXform` `rotated` |
  | `Basis` | `inverse` `transposed` `determinant` |
  | `Transform3D` `Projection` | `affineInverse` / `inverse` |

  **回転の向きに三角関数は要らない。** `Transform2D` の列が基底そのものなので、
  ワールドの X 軸は `t.x()`、Y 軸は `t.y()` である
- 演算子はエンジンが持っているものだけ。回転（`Quaternion`）や変換
  （`Transform2D` `Transform3D` `Basis` `Projection`）の `*` は成分ごとの積
  ではなく、それぞれの意味の積である

#### 持ち回るときは箱に入れる

8.9 はホスト値を**表の要素・捕捉・`any^`・合併・`...`・`def^` のメンバ**から
締め出す。フレームより長生きする場所には置けない。入るのは箱の方で、
言語が型ごとに自動で与える（`godot.Vector2.Box^`）。

```lhat
@game
public^let^Mover = Godot.Node2D..def^{
    self^{ velocity = box^godot.vector2(0, 0) },

    _process = p^self^, delta:number^{
        let^v = self^.velocity.get()
        self^.velocity.set(v * 0.99)
    },
}
```

#### ノードの欄は型ごとに読み書きする

`get` / `set` は `any^` を運ぶので、ホスト値は運べない。だから型ごとに1組ある。

```lhat
let^at = self^.gdobj.getVector2("position")
self^.gdobj.setVector2("position", at + godot.vector2(10, 0))
self^.gdobj.setColor("modulate", godot.color(0, 1, 0, 1))
```

型がそこで決まるので、`Color` の欄を `Vector2` として読む間違いは**検査が
その場で捕まえる**。別の型の欄を読めばエンジン自身の変換が答える
（`Vector2` を `Vector3` として読めば z が 0 になる）。

**`call()` に渡すときは箱に入れる。** 8.9 が `...` からホスト値を締め出すので、
そのままは書けない——`box^` で包めばよく、箱は解かれて渡る。

```lhat
self^.gdobj.call("set_position", box^godot.vector2(7, 8))
```

同じ理由で、`self^` の欄に箱を持たせておけば**インスペクタから読み書きできる**。
エンジンが書いた値は箱の中身に上書きされる——箱そのものは差し替わらない。値型を
`@export` するときはこの形になる。

```lhat
self^{ @export where = box^godot.vector2(0, 0) },
```

［補足］ 逆向き——Variant から箱を**作る**道は無い。箱はヒープの物であり、
ヒープは機械のものだからで、`box^` が唯一の作り手である（8.9 が「持ち回るなら
綴りに出せ」と言っているのと同じこと）。

#### 値に見えて値でないもの — `Callable` と `Signal`

どちらも中に参照を持つ。8.9 のホスト値は**回収器が覗かない生バイト**でなければ
ならないので、この2つは値になれない。**8.8 のデータ**として、ポインタの向こうに
置き、`dispose` で返す（02 の 12.5）。

代わりに得るものがある——**表にも `any^` にも入る**。`get` / `set` / `call` が
そのまま運ぶ。

```lhat
let^me = self^.gdobj
let^to = godot.callable(me, "onFired")     # 自分のメンバを指す
let^sig = godot.signal(me, "fired")

sig.connect(to)
sig.emit("hello")                          # onFired が走る
sig.disconnect(to)
```

**無から作る道は無い。** `Callable` はオブジェクトとメソッド名から、`Signal` は
オブジェクトとシグナル名から作る。それ以外の作り方は無い——`godot.Object` が
同じ理由で `override^new` の引数からしか来ないのと同じである（14.15）。

`Callable` は `isValid()` `getMethod()` `getObject()` `call(…)`、
`Signal` は `getName()` `getObject()` `isNull()` `connect()` `disconnect()`
`isConnected()` `emit(…)`。

［補足］ `RID` は値の側にいる。8バイトの不透明な番号で、参照を持たない
——指し示す先はサーバのもので、L^ が生かすものではない。読めるのは
`getId()` だけである。

#### `Packed*Array` の10種も 8.8 のデータ

エンジンの側では書き込み時複製・参照計数なので 8.9 の値ではない。かといって
**L^ の表にも書き出せない**——10種のうち4種（`Vector2` `Vector3` `Vector4`
`Color` の配列）は要素がホスト値で、8.9 がそれを表から締め出しているためである。

だから10種とも同じ形にした。**長さと添字を持つ不透明な構造**である。

```lhat
let^points = godot.packedVector2Array()
points.append(godot.vector2(1, 2))
points.append(godot.vector2(3, 4))
print($"{points.size()} 点、2番目は {points.at(2)}")
points.set(1, godot.vector2(9, 9))
self^.gdobj.set("polygon", points)
```

`size()` `at(i)` `set(i, v)` `append(v)` `clear()` `dispose` `iterate`。添字は
1 から（02 の 14 の並びと同じ）で、無い添字は要素の零を答える——表と同じ
読み方である。

**`for^` で歩ける**（05 の 8.8 の `iterate` 登録）。出すのは要素だけ——
Packed\* は列であり、1名の走査が列の値を受けるのは表の密部と同じ読み方。
添字も要るなら従来どおり `for^ i from^ 1 to^ a.size()` と数える。
`Vector2` などホスト値の要素も焦点へ丸ごと渡る（8.9改）。

```lhat
for^ p in^ points {
    total := total + p.x
}
```

`get()` で受け取った配列は `any^` なので、strict では焦点に型を書く
（03 の 3.1③）——`for^ p:godot.Vector2 in^ points { … }`。

走査中の `append` は見える（歩みは毎歩長さを引き直す）。走査中の `dispose`
は `at()` 後の `dispose` と同じく書き手の誤りである（02 の 10.7）。

**写し取らない。** 千要素の配列を読むのは呼び出し千回であって、千要素の表を
作ることではない。`get()` / `set()` / `call()` はそのまま運ぶ。

### 外で編集したものは読み直される

`.lh` を Godot の外（VSCode など）で書き換えても、エディタは気づいて読み直す。

- **タブで開いているもの** — Godot 自身がフォーカスを取り戻したときに読み直す。
  設定は `text_editor/behavior/files/auto_reload_scripts_on_external_change`
  （既定で入っている）。未保存の編集があるタブは黙って上書きせず確認を出す
- **開いていないもの** — `Script::reload_from_file` が言語に回してくるので、
  こちらがファイルを読み直して検査し直す。**エンジンはもう自分では読まない**
  ので、読むところから拡張の仕事である

**読み直したら着せ直す。** 14.3 はインスタンスの持ち物を作るときに決めるので、
新しい機械の上では前のインスタンスは通用しない。だから読み直したあと、その
スクリプトを着ていたオブジェクト全部に**もう一度着せる**——エンジンがそこで
新しいインスタンス（エディタなら placeholder）を作り直す。

**外からの書き換えに限らない。** エディタは保存のたびに `reload()` を呼ぶので、
着せ直しはそちらでも要る。しかも `@game` と `@tool` を書き換えたときは
**インスタンスと placeholder のどちらであるべきかが変わる**——着せ直さないと、
`@tool` にしても編集中に動き出さず、`@game` に戻しても前のインスタンスが
死んだまま残って `@export` の欄がインスペクタから消える。

**着せ直す前に欄の値を読み、着せ直したあとに書き戻す。** それが
`reload(keep_state)` の `keep_state` で、エンジンは常に真を渡してくる。
14.3 は新しいインスタンスを雛形から作るので、これが無いと保存のたびに
インスペクタで入れた値が既定値に戻る。値はオブジェクトから読む——
placeholder の値はエディタのもので、L^ 側のどこにも無いため。

［補足］ `Object::set_script` は今と同じスクリプトを渡すと何もしないので、
一度外してから着せている。GDScript も同じ手順を踏む。

### エンジンに渡すことは注釈で書く

02 の 18 の注釈は**構文木に括り付けられて残る**ので、実行しなくても読める。
`@export` も `@signal` も、この拡張が `lhat_register_annotation` で名前と
書ける場所を登録し（引数の形は `lhat_register_annotation_signature`、
`@game`/`@tool` の排他は `lhat_register_annotation_exclusive`）、
木から読み返しているだけである。言語はどれが何のためのものかを知らない。

```lhat
@game
public^let^ Spinner = Godot.Sprite2D..def^{
    self^{
        @export_range(0, 10) speed = 1,
    },

    @signal died = p^self^, message:string^ {
        self^.gdobj.emit(id^died, message)
    },
    ...
}
```

- **`@game`** / **`@tool`** — その `public^` な `def^` がノードの着るクラスで
  あると言う。違いは編集中も動くかどうか。公開クラスが1つしか無ければどちらも
  書かなくてよい。上の「ノードに着せられる」を見る

- **`@export_class`** — そのクラスがプロジェクト全体で名乗る。名前は束縛の
  名前で、引数は取らない。**書かなければ無名**であり、それが既定である。
  上の「プロジェクト全体で名乗るのは `@export_class` を書いたときだけ」を見る

- **`@icon`** — そのクラスのアイコン。シーンツリー、Create Node、ファイル
  一覧に出る。**アイコンはグローバルクラス名に掛かる**ので、`@export_class`
  と併記しなければならない——02 の 18.5.2 の併記必須として登録してあるので、
  片方だけ書けば検査器がその場で弾く（行番号付き）。的も `@export_class` と
  同じ（`PUBLIC` と `FILEUNIQUE`）で、理由も同じ——私有のクラスは名前を持て
  ないし、1ファイルが登録する名前は1つなので、着けるアイコンも1つである

  ```lhat
  @game
  @export_class
  @icon("res://lhat/lhat-logo-small.svg")
  public^let^ Spinner = Godot.Sprite2D..def^{ … }
  ```

  ```lhat
  @game
  @icon("res://lhat/lhat-logo-small.svg")
  public^let^ Spinner = …
  # error: this annotation means nothing on its own, and the one it has to
  #        stand beside is missing: export_class
  ```

- **`@export`** — `self^` の欄をインスペクタに出す。欄の現在値がそのまま
  出て、インスペクタで変えるとその**インスタンスの** `self^` の欄が変わる。
  ヒントを付ける形が4つ:

  | 書き方 | 出るもの |
  | --- | --- |
  | `@export_range(下, 上)` | スライダ。3つ目以降は Godot の追加引数そのまま |
  | `@export_enum("赤", "緑")` | 選択肢。1つ以上 |
  | `@export_file("*.png")` | ファイル選択。絞りは0個以上 |
  | `@export_multiline` | 複数行の入力欄 |

- **`@signal`** — メンバをシグナルの宣言にする。**メンバの引数欄がそのまま
  シグナルの引数欄**で、13.4 が `self^` を外すので呼び手が書く欄と一致する。
  名前も型も書かれているから、接続ダイアログにも `arg0` ではなく
  `message : String` と出る
- **`@rpc`** — メンバをネットワーク越しに呼べるものにする。語彙は GDScript の
  もので、何も書かなければ authority・reliable・call_remote になる

`self^.died("…")` は**普通のメンバ呼び出し**として型検査を通り、走ると
`emit_signal` に届く。Godot は**同名のメソッドとシグナルを同居させる**ので、
`connect` した先も `call("died", …)` も両方が期待どおりに動く。

**本体を空にすると、拡張が埋める**（02 の 18.7改）。

```lhat
@signal died = p^self^, message:string^ { },
```

発火の一行はどのシグナルでも同じなので、書き写さなくてよい。空の本体には
`_reload` のたびに「自分の名前を引数ごと発火する」値が入る。**型で宣言する形**
（`@signal died : p^self^, string^;`）**は無い**——13.4 が型から引数名を落とし、
接続ダイアログが `message` ではなく `arg0` と言い出すためである。

**本体を書けば、書いたものが走る。** 差し替わるのは空のときだけで、独自の
発火を書く道はそのまま残る。`@signal` の付いていない空の本体は、何もしない
普通の手続きのままである。

**書いた本体が自分の名前を発火しているかは拡張が見る。** 02 の 18.7 のとおり、
`@signal` が何を意味するかを知っているのは言語ではなくホストなので、
検査器はここに口を出せない。`_reload` のたびに本体の書かれた名前を見て、
自分の名前が無ければ警告する:

```text
res://spinner.lh: @signal died is never emitted by name in its own body
```

計算して得た名前（`emit(names[i], …)`）は「書かれていない」側に落ちる。
空の本体は拡張のものなので、見張らない。

**接続ダイアログは受け手の雛形を書く。** シグナルをメソッドに繋ぐとき、
`_on_… = p^self^, … { }` が作られて**クラス本体の中に入り**、エディタが開いて
そこにキャレットが来る。引数の型は Godot のもので L^ のものではないので、
共有している3つ（`int`/`float` → `number^`、`String` → `string^`、
`bool` → `bool^`）だけ移し、残りは `any^` にする——知らない綴りを書くと検査が
通らなくなり、消してから書き直す羽目になるため。

**書いているのは拡張である。** エンジンの挿入位置は**ファイルの末尾で固定**で
（`script_text_editor.cpp` の `add_callback`）、言語が口を出す道は無い。波括弧で
本体を閉じる言語ではそこは閉じ括弧の外であり、C# は同じ壁に当たってこの機能を
諦めている（`csharp_script.cpp`「末尾に足すとコンパイルが壊れる」）。

こちらは `_make_function` に**何も答えさせず**、代わりに `LhatEditorPlugin` が
エディタの `script_add_function_request` を受けて自分で書く。エンジンが末尾に
足すのは空行2つだけになるので、**その直後にエンジンが保存するファイルは常に
構文として正しい**。位置は字句解析器で求める——文字列やコメントの中の `}` を
閉じ括弧と読み違えないため。

［補足］ `_can_make_function` は真のままにしてある。偽にすると接続ダイアログが
「受け手は自動生成されません」と出すが、生成しているので嘘になる。

#### `@rpc` はネットワーク越しに呼べると言う

書いたメンバがエンジンの RPC 表に載る。**語彙は GDScript のものをそのまま
採る**ので、あちらで書いてきたものがそのまま読める。

```lhat
@rpc
fire = p^self^ { … },                                    # 既定のまま

@rpc("any_peer", "call_local", "unreliable", 3)
move = p^self^, x:number^, y:number^ { … },              # 全部書いた形
```

| 語 | 言うこと | 既定 |
| --- | --- | --- |
| `"call_local"` / `"call_remote"` | 呼んだ側でも本体を走らせるか | `call_remote` |
| `"any_peer"` / `"authority"` | 誰が呼べるか | `authority` |
| `"reliable"` / `"unreliable"` / `"unreliable_ordered"` | どう届けるか | `reliable` |
| 数 | チャンネル | `0` |

**何も書かなくてよい。** 4つとも既定があるので、`@rpc` だけで
「authority が呼べて、確実に届いて、呼んだ側では走らない」になる。

GDScript より緩いところが2つある。

- **数はどこに書いてもチャンネル**。GDScript は4番目の位置にあるものだけを
  チャンネルとして読むが（`gdscript_parser.cpp` の `if (i == 3)`）、位置に
  意味を持たせる理由が無い。`@rpc(3)` も `@rpc("any_peer", 3)` も通る
- **裸の名前も書ける**。`@rpc(any_peer)` は `@rpc("any_peer")` と同じ。
  18.3 が名前を綴りのまま渡し、意味の判断をホストに任せているため

**語を判じるのはホストである**（18.1）。検査器は引数がリテラルだと知って
いるだけなので、知らない語・同じ区分を2度・`@signal` との併記は、
`_reload` のときに拡張が言う:

```text
res://net.lh: error: @rpc does not know this word on fire: whenever -- one of …
res://net.lh: error: @rpc says who may call it twice on move
res://net.lh: error: @rpc and @signal say different things about the same member: fired
```

［補足］ エンジンは名前を昇順に並べて ID を振る
（`scene_rpc_interface.cpp` の `_parse_rpc_config`）ので、両端が同じ綴りを
持っていれば ID は自然に一致する。


**`@onready` は登録していない。** GDScript のそれは初期化式を `_ready` まで
遅らせるものだが、14.11 は `self^` の欄の初期化式を `new` の中で走らせ、
欄ごとの入口を残さない。あとから1欄だけ走らせ直す道が実行時に無い以上、
名前だけ受けても何も起こせない——**書けて何も起きない注釈は嘘**なので、
`no host registered an annotation of this name` と言わせる。
木が要るものは `_ready` に書けばよい。

### 説明文は綴りを持たない

01 の 6.4 が既に決めている——**説明文の記法は設けない**。コメントはすべて
等しく保たれ、**そのものの直上に置かれたコメント塊がそのものの説明**である。
Go と同じ形で、`##` のような専用の綴りは無い。

```lhat
# 回るスプライト。エディタのヘルプページに出るのはこれ。
@game
public^let^Spinner = Godot.Sprite2D..def^{
    self^{
        # 一秒あたりの回転。インスペクタの欄のツールチップに出る。
        @export_range(0, 10) speed = 1,
    },

    # 死んだときに鳴る。
    @signal died = p^self^, message:string^{ … },
}
```

**塊は空行で切れる。** 間に1行でも空けば、その上のものはもう説明文ではない
——空行の上にあったものについて書かれている。**行末に残したコメントも
説明文ではない**。どちらも 6.4 の規則で、言語側が答える。

出る先は2つ:

- **F1 のヘルプページ** — クラス名で引くと、クラスの説明、メソッド、
  シグナル、`@export` の欄、静的な定数が並ぶ
- **インスペクタのツールチップ** — `@export` の欄にカーソルを置くと、
  その欄の上に書いた塊が出る

クラス自身の説明が無ければ**ファイル先頭の塊**に落ちる。1クラスの単位では
頭に書くほうが自然なので。

［補足］ 出るのは**着るクラス1つ分**だけである。05 の 5.5 は単位が複数の
クラスを公開することを許すが、エンジンがページを引ける名前を持つのは着る
ものだけで、2つ目は誰も訊かない場所に置かれることになる。

## 呼び出して使う

登録されるもう1つのクラスが `LhatRuntime`。全部 static で、状態を持たない
——1回の呼び出しがプログラムを作り、検査し、走らせ、捨てる（05 の 8.7 の順）。

- `LhatRuntime.version()`
- `LhatRuntime.run_status_message(status)`
- `LhatRuntime.check(path) -> PackedStringArray` — `path` とそれが
  `require^` する全部を検査。空なら診断ゼロ
- `LhatRuntime.run(path) -> PackedStringArray` — 同じ。通れば走らせる。
  空なら走った

```gdscript
for line in LhatRuntime.run("res://hello.lh"):
    push_error(line)
```

`print` は Godot の出力パネルへ行く（05 の 8.2 の初期束縛）。
`res://` のパスは **`FileAccess` 経由**なので、書き出した .pck の中も読める。

### 値は両方向に渡る

`LhatRuntime.call_member(path, member, args)` が単位を走らせ、
**単位が答えたテーブルの成員**を呼ぶ。単位が答えるのは `public^` な名前の
表（05 の 5.5）なので、「単位の成員」がそのまま呼べるものになる。

| L^ | Variant |
| --- | --- |
| `nil^` | `null` |
| `bool^` | `bool` |
| 整数 / 実数 | `int` / `float` |
| `string^` | `String` |
| 鍵が 1..n の表 | `Array` |
| それ以外の表 | `Dictionary` |

逆向きも同じ対応。`String`/`StringName`/`NodePath` はすべて `string^` に、
`Array` は 1..n の表になる。**`Node`・`Callable`・`Signal`・`Packed*Array` は
8.8 のデータとして渡る**。数学の型は 8.9 の値なので `any^` に入れないが、
`box^` に入れれば渡る。残りは拒む。

```gdscript
LhatRuntime.call_member("res://lib/api.lh", "total", [[1, 2, 3, 4]])   # 10
LhatRuntime.call_member("res://lib/api.lh", "numbers", [5])            # [1,4,9,16,25]
```

**この道では単位は毎回走り直す。** 前回の状態は残らない。状態を持たせたい
なら上のスクリプトの道を使う——`module^` が答えを `L^.modules` に置くので、
機械を跨いで生き続ける。

着せてあるスクリプトの静的メンバを呼びたいなら、上の `call_static` の方である
——あちらは既に読み込まれている機械と定義をそのまま使い、走らせ直さない。

（`src/gc.c` の根は `L^` とフレームと保留中の破棄だけで、ホストが C 変数で
持っている値は根ではない。`module^` を要求しているのはこのため。）

## ビルド

```powershell
. .\scripts\devshell.ps1
cmake --preset godot-debug        # 初回は godot-cpp を取得する
cmake --build --preset godot-debug
```

`godot-release` も同じ。前者は Godot の `template_debug`、後者は
`template_release` を作る。出力は `godot/demo/bin/` に直接落ちる:

```text
liblhat.windows.template_debug.x86_64.dll
```

この名前は godot-cpp が自分のライブラリに付ける接尾辞から取っている。
`demo/lhat.gdextension` の `[libraries]` はその綴りをそのまま書いたもの。

## 言語サーバーに `godot` を教える

`lhat-host.json` は「ホストの C が何を登録したか」を、C を走らせられない
道具に伝えるための唯一の口（05 の 8.7）。言語サーバーは
**ワークスペース直下の1枚**しか読まない（`lsp/workspace.c`）。

ところが全部を知っているホストが居ない — cli は stdlib を登録するが
エンジンをリンクせず、この拡張は `godot` を登録するが stdlib 抜きで建つ。
そこで両方に訊いて併合する:

```powershell
. .\scripts\devshell.ps1
.\scripts\dump-host-api.ps1 -Godot D:\path\of\Godot_console.exe
```

`-Godot` を省くと cli の分だけ書く。拡張側の答えは
`LhatRuntime.dump_host_api(path)` が出していて、
`demo/dump_host_api.gd` がそれを呼ぶ。

8.7 は同じ名前の2度目の登録を**腕の追加**とするので、違う項は両方残る。
まったく同じ項だけが落ちる（`print` は両方が登録している）。

## デモ

`godot/demo/` を Godot で開いて **F5**。または

```powershell
godot --headless --path godot\demo
```

どちらも `main.tscn` → `main.gd` を走らせる。拡張が読めない・単位が通らない
場合は非ゼロで終わるので、そのまま検査に使える。

スクリプトが1本なのは、**エディタとコマンドラインの両方で走る形が
メインシーンしか無い**ため。`EditorScript` は「ファイル > 実行」専用で
エディタ内でしか生成できず、`--script` は `MainLoop`/`SceneTree` しか
受け取らない。単一継承なので1つのクラスが両方になることはできない。

## godot-cpp

既定では `build/godot-cpp` に `godot-4.5-stable` を取ってくる。プリセットごとに
ツリーが分かれても取得は1回で済むよう、`build/` の下を共有している。

- `LHAT_GODOT_CPP_DIR` — 別の場所にある checkout を使う。そこに
  `CMakeLists.txt` があれば取得はせず、**そのバージョンで**ビルドする
- `LHAT_GODOT_CPP_TAG` — 取得するタグ

タグを変えたら `demo/lhat.gdextension` の `compatibility_minimum` も合わせる。

## 注意

- **MSVC ランタイム。** godot-cpp は既定で C ランタイムを静的リンクし、
  リンカは混在を拒む。`godot/CMakeLists.txt` が `lhat` と `lhatport` を
  godot-cpp の選択に合わせる。`godot-*` プリセットが cli・言語サーバー・
  テストを切っているのはこのため——同じツリーで食い違わせない
- **スレッド。** 言語本体は1スレッドで走る前提（`port/thread.h` 冒頭）。
  Godot 側から複数スレッドで触る話はまだ何も決めていない

- **パスの scheme は言語に見せない。** 05 の 5.1 は区切りを畳んで他を
  解釈しないので、`res://a.lh` は `res:/a.lh` になって開けなくなる。
  境界で scheme を外し、ローダが付け直す。結果として**1つのプログラムが
  読めるのは1つの scheme**（`res://` と `user://` は混ぜられない）

- **GDExtension の仮想メソッドは GDScript から呼べない。** `_validate` も
  `_is_valid` も `has_method` に出ず `call` も通らない——エンジンが呼ぶための
  ものだから。ヘッドレスから確かめられるのは公開メソッド越しの道
  （`load` / `reload` / `ResourceSaver.save`）だけで、ダイアログと
  エディタの赤線はエディタを開いて見るしかない

- **`String(const char *)` は Latin-1 として読む。** UTF-8 のリテラルを
  そのまま渡すと壊れる（`05 の 5.5` が `05 ã® 5.5` になる）。
  非 ASCII を含む文字列は必ず `String::utf8(...)` を通す

- **言語が答えることの多くは、エディタが先に「扱うか」を訊く。**
  `_get_global_class_name` は `_handles_global_class_type` が真を返すまで
  一度も呼ばれない。実測でそこに一日分の見当違いをした——`get_global_name()`
  は最初から正しく答えていて、訊かれていなかっただけだった。
  答えが出ないときは、まず**入口の可否を返す仮想**を疑う

- **エディタの走査は変更のあったファイルしか見ない。** 拡張を建て直しても
  既存の走査結果は無効にならないので、`.godot/global_script_class_cache.cfg`
  は空のまま。ファイルを一度保存すれば走る

## これから

- 補完（`_complete_code`）。`lsp/` が既にあるので、そこを使い回す話になる。
  意味層は `lhat/semantic.h` として公開済みなので、補完も同じ筋で降ろせる
  かもしれない
- `stdlib/` を繋ぐか。`std.io` はゲームの中では意味が変わる
- `require^` が返す表を名前空間と見るか。いま
  `let^Godot = require^"lhat/Godot.lh"` の `Godot` に色が付かないのは、
  検査器が単位の公開表を `is_module` と見ないため。色付けの都合ではなく
  意味論の判断なので、そこから決める必要がある
- ハイライタが打鍵ごとに検査していて、`_validate` の分と合わせて2回になる。
  目に見えて重ければ、本文をキーにした共有を `LhatLanguage` に置く
  ——先に仕組みを作らず、測ってから
