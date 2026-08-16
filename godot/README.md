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

**ノードには付けられない。** `_can_instantiate()` は偽を返す。ノードに付いた
L^ が動くには L^ の値と Variant の変換層が要り、それはまだ無い。
メンバ・メソッド・シグナルを訊かれると空で答えるのは、
インスタンスを持たないスクリプトの正直な答えとしてそうしている。

`_get_reserved_words` の語は `vscode-extension/syntaxes/lhat.tmLanguage.json`
の分類をそのまま使った。言語側は一覧を持たない——01 の 2 章 は
ハット付きの語を一律に読み、知らない語を弾くのは意味解析である。

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

**単位は毎回走り直す。** 前回の状態は残らない。機械を跨いで保つには
**ホストが値を走行を跨いで保持する道**が要るが、ライブラリにまだ無い
（src/gc.c の根は L^ とフレームと保留中の破棄だけ）。Script のインスタンスは
それを必要とする。

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

- **ノードに付いて動く**（`GDExtensionScriptInstanceInfo3` の関数表）。
  ここで **L^ の値 ⇄ Variant** の変換層が要る。ここが本番
- 補完（`_complete_code`）。`lsp/` が既にあるので、そこを使い回す話になる
- `stdlib/` を繋ぐか。`std.io` はゲームの中では意味が変わる
