# AGENT.md

このファイルは、lhat リポジトリで AI アシスタントが守るべき実務ルールを定義します。

## プロジェクト概要

L^ is Modern & Better Lua with visual programming.

Lua風の、C言語でコンパイル可能なバイトコードインタプリタ型のグルー言語・プログラミング言語 `L^`(lhat)を制作しようとしてる

使用言語 C11
クロスプラットフォームビルドツール Cmake

まだ実用に供されていないので、**後方互換性を保つ必要はない**。
最少差分であることより、**最終的により少ないコードであることを重視**。既存コードを書き換えない100行の追加より、既存コードの形を変え、増分50行で収まる変更のほうを選ぶ。

## コーディング規約

### コメントは英語で書く

新しく書くソースコードのコメント（`.c` / `.h` / テスト / `CMake`）は英語。日本語で書かない。

既存の日本語コメントはそのまま残す。そのコメント自体を書き直すときだけ英語にする。

## その他のリソース

### lhat-svg-tools

L^ソースをシンタックスハイライト済みのSVGに変換するツール

@../lhat-svg-tools/

### lhat-gdextension

L^ のGodotエンジン向けバインディング

@../lhat-gdextension/

### lhatove

Love2D の L^ 使用版プロジェクト

@../lhatove/

### Lua 5.5.1

Lua 5.5.1のソース

@../lua-5.5.1/

### Luau

Luau のソース

@../luau/

### LuaJIT

LuaJIT のソース

@../megasource/libs/LuaJIT

### AngelScript

UnrealEngine-AngelScript のソース

@../UnrealEngine-AngelScript
