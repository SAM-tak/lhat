# L^ (lhat) GDExtension

Godot が実行時に読み込む共有ライブラリとして L^ を提供する側。
godot-cpp に対して書くので、ツリーの中でここだけが C++。言語本体は C11 のまま、
`include/lhat.h` 越しに——他のホストと同じ面で——呼ばれる（05 の 8.7）。

**現状はビルド設定のみ。** 登録されるクラスは `LhatRuntime` 1つ、
持っているのは静的メソッド2つだけ:

- `LhatRuntime.version()` — ヘッダに届いていることの確認
- `LhatRuntime.run_status_message(status)` — ライブラリに届いていることの確認

実行時 API（プログラムの読み込み・検査・実行）はまだ無い。

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

## これから

- `res://` からユニットを読むローダ（既定の `port/loader.c` は stdio で
  ファイルを読む。パッケージの中は読めない）
- ホスト名の登録。`print` を Godot の出力へ、など（05 の 8.7）
- `stdlib/` を繋ぐか。`std.io` はゲームの中では意味が変わる
- ノードに L^ のスクリプトを割り当てる話（`ScriptLanguageExtension`）は
  その先
