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
`@tool` の併記だけは、個数が登録ごとに数えられる以上どちらの登録からも見えない
——こちらは `_reload` の位置で言う。公開クラスが2つ以上あって印が無い場合は、
**着せようとした時に**言う——`require^` されるだけのライブラリなら、どれを
着るかを答えないのは何も間違っていないので（05 の 5.5 のとおり、複数のクラスを
答えるのがクラスのライブラリの姿）。

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

［補足］ placeholder が出す欄の初期値は**型の零**（`0` / `""` / `false`）で、
書かれた初期化式ではない。14.11 が初期化式を `new` の中で走らせる以上、
実体を作らずに読む道が無いため。ゲームを走らせれば書いたとおりの値になる。

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
本物が手元にある。14.11 が `self^` の初期化式を `new` の中で走らせる以上、
**構成子がノードを触れるのはこの順序だけ**である——返ってから欄に書き込む形
では、初期化式も `override^new` の本体も無効なハンドルしか見られない。

だから場は `abstract^` のままでよい。14.15 が生成を拒むのは既定の `new()` が
入れる値を持たないからで、14.11改 の `override^` はその既定を消す。
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

### エンジンに渡すことは注釈で書く

02 の 18 の注釈は**構文木に括り付けられて残る**ので、実行しなくても読める。
`@export` も `@signal` も、この拡張が `lhat_register_annotation` で
名前と引数の形を登録し、木から読み返しているだけである。言語は
どれが何のためのものかを知らない。

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

発火するのは本体である。ホストが値を差し替えるわけではないので、
`self^.died("…")` は**普通のメンバ呼び出し**として型検査を通り、走ると
`emit_signal` に届く。Godot は**同名のメソッドとシグナルを同居させる**ので、
`connect` した先も `call("died", …)` も両方が期待どおりに動く。

**本体が自分の名前を発火しているかは拡張が見る。** 02 の 18.7 のとおり、
`@signal` が何を意味するかを知っているのは言語ではなくホストなので、
検査器はここに口を出せない。`_reload` のたびに本体の書かれた名前を見て、
自分の名前が無ければ警告する:

```text
res://spinner.lh: @signal died is never emitted by name in its own body
```

計算して得た名前（`emit(names[i], …)`）は「書かれていない」側に落ちる。

登録してあるが**まだエンジン側の意味が無い**もの: `@icon` `@rpc`。
書いても検査は通り、何も起きない。

**`@onready` は登録していない。** GDScript のそれは初期化式を `_ready` まで
遅らせるものだが、14.11 は `self^` の欄の初期化式を `new` の中で走らせ、
欄ごとの入口を残さない。あとから1欄だけ走らせ直す道が実行時に無い以上、
名前だけ受けても何も起こせない——**書けて何も起きない注釈は嘘**なので、
`no host registered an annotation of this name` と言わせる。
木が要るものは `_ready` に書けばよい。

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
`Array` は 1..n の表になる。**それ以外は拒む**——`Callable` も `Node` も
`Vector2` も、L^ 側に居場所がまだ無い（05 の 8.8 / 8.9 がその居場所）。

```gdscript
LhatRuntime.call_member("res://lib/api.lh", "total", [[1, 2, 3, 4]])   # 10
LhatRuntime.call_member("res://lib/api.lh", "numbers", [5])            # [1,4,9,16,25]
```

**この道では単位は毎回走り直す。** 前回の状態は残らない。状態を持たせたい
なら上のスクリプトの道を使う——`module^` が答えを `L^.modules` に置くので、
機械を跨いで生き続ける。

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
.\scripts\dump-host-api.ps1 -Godot D:\path	o\Godot_console.exe
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

## これから

- 着ていないスクリプトのプロパティ一覧（`_get_script_property_list`）。
  いまインスペクタに出るのはインスタンスの欄だけで、スクリプト自体に
  訊く道は空を返す
- `@icon` `@rpc` にエンジン側の意味を与える。`@icon` は
  スクリプトがグローバルクラス名を持たないと出ないので、
  `_get_global_name` に何を答えるかを先に決める必要がある
- 補完（`_complete_code`）。`lsp/` が既にあるので、そこを使い回す話になる
- `stdlib/` を繋ぐか。`std.io` はゲームの中では意味が変わる
