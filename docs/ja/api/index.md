# C API リファレンス

このリファレンスは、アプリケーションへの組み込みで使う主要 API の役割、呼ぶ順序、所有権を説明します。
最初から実装する場合は[組み込みガイド](../embedding/index.md)を先に読んでください。
全 API の宣言と細部の契約は[公開ヘッダ](https://github.com/SAM-tak/lhat/tree/main/include/lhat)で確認できます。

| 分類 | 主な内容 |
| --- | --- |
| [Program とユニット](./program.md) | ローダ、型検査、コンパイル、診断、リロード、バイナリ |
| [ホスト API の登録](./registration.md) | 関数、参照型、値型、定数、エラー、アノテーション |
| [Machine と値](./machine.md) | 呼び出し、コルーチン、GC、ルート、エラー、値の生成 |

## ヘッダとリンク

通常のホストは `#include "lhat.h"` を使います。
これは `program.h`、`vm.h`、値とオブジェクトの定義、診断、デバッグ、ポートの公開 API をまとめた入口です。
インストール済み SDK では、CMake の `lhat::lhat` と標準ポートの `lhat::lhatport` をリンクします。
標準ライブラリを使う場合は `lhat::lhatstdlib` と、必要な `<lhatstdlib/...h>` を追加します。

## 所有権の基本

| データ | 所有者 / 有効期間 |
| --- | --- |
| `LhatProgram *` | ホストが作成し `lhat_program_free` で破棄 |
| `const LhatUnit *`、コンパイル済み proto | Program が所有。reload では旧コードの寿命にも注意 |
| `LhatMachine *` | ホストが作成し `lhat_machine_dispose` で破棄 |
| Machine 上の文字列・テーブル・クロージャ | Machine の GC が管理。C の変数だけでは保持されない |
| ローダの戻りバッファ | `lhat_alloc` で確保して Program へ渡す |
| バイナリとシグネチャ表の出力バッファ | 呼び出し元が受け取り `lhat_free` で解放 |
| 登録する `context`、hostdata の指す先 | ホストが寿命を管理 |

## 戻り値と出力の読み方

- `bool` は各関数の契約に従って確認します。table の書き込みは `bool` に加え `refused` の確認が必要です。
- `LhatRunResult` はまず `status` を確認し、成功・予算による中断・失敗を区別します。
- テキスト出力は多くが必要バイト数を返します。終端 NUL を含まないため、測定値 + 1 の領域を渡します。
- `LhatValue` の getter は値の種類を確認してから使います。

パスと文字列は UTF-8、API の長さは特に断りがなければバイト数です。
数値は `number^` でも整数・実数の二つの表現を持ちます。
