# L^ (lhat) GDExtension

Godot が実行時に読み込む共有ライブラリとして L^ を提供する側。
godot-cpp に対して書くので、ツリーの中でここだけが C++。言語本体は C11 のまま、
`include/lhat.h` 越しに——他のホストと同じ面で——呼ばれる（05 の 8.7）。

登録されるクラスは `LhatRuntime` 1つ。全部 static で、状態を持たない
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

**単位の答えは Variant で返らない。** 値が境界を越える変換層は Script が
要求するもので、まだ無い。非 nil^ の答えは出力パネルに書かれるだけ。

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

`godot/demo/` を Godot で開けば拡張が読まれる。スクリプトから:

```gdscript
print(LhatRuntime.version())
```

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

## これから

- `.lh` をリソースに（`ResourceFormatLoader` + `ScriptExtension`）
- 言語として登録（`ScriptLanguageExtension` + `Engine.register_script_language`）。
  ここで「スクリプト作成」ダイアログに L^ が出る。`_validate` は
  `check()` の使い回し
- ノードに付いて動く（`GDExtensionScriptInstanceInfo3`）。ここで
  **L^ の値 ⇄ Variant** の変換層が要る
- `stdlib/` を繋ぐか。`std.io` はゲームの中では意味が変わる
