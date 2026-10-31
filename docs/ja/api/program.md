# Program とユニット

宣言: [program.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/program.h)、
[port.h](https://github.com/SAM-tak/lhat/blob/main/include/lhat/port.h)。

## 作成とローダ

```c
typedef char *(*LhatProgramLoader)(void *context, const char *path,
                                  size_t *length);
LhatProgram *lhat_program_new(bool strict, LhatProgramLoader load, void *context);
void lhat_program_free(LhatProgram *program);
```

`strict` は型検査の厳密さです。`load` が NULL ならファイルを読みません。
ローダは見つからない場合に NULL、それ以外は Program が `lhat_free` するバッファを返します。
標準ファイルシステムのローダには `lhat_load_file` を指定できます。

`lhat_program_set_lock` は共有 Program の更新に使う lock/unlock と context を設定します。
実行開始前に設定します。ロックは再帰可能である必要はありません。
Program の作成と登録は直列に行ってください。

## 検査、コンパイル、インストール

```c
const LhatUnit *lhat_program_check(LhatProgram *program, const char *path);
bool lhat_program_compile(LhatProgram *program);
bool lhat_program_install(const LhatProgram *program, LhatMachine *machine);
```

| API | 契約 |
| --- | --- |
| `lhat_program_check` | 依存先から順に検査。ファイル自体が読めなければ NULL。非 NULL でも診断確認が必要 |
| `lhat_program_has_errors` | Program またはユニットの問題の有無 |
| `lhat_unit_ok` | ユニットの検査結果の確認 |
| `lhat_program_compile` | 検査済みで未コンパイルのユニットをコンパイル。false は失敗 |
| `lhat_program_compile_status` / `lhat_program_compile_failure` | コンパイル失敗の情報 |
| `lhat_compile_message_write` | コンパイル失敗のメッセージを整形 |
| `lhat_program_install` | ホストの登録を Machine の `L^.modules` などへ配置。false は失敗 |
| `lhat_unit_proto` | 実行するユニットのコンパイル済み本体。Program より長く保持しない |

## ユニットと診断

| API | 用途 |
| --- | --- |
| `lhat_program_units` / `lhat_unit_next` | 読み込んだユニットを走査 |
| `lhat_unit_path` / `lhat_unit_module_name` / `lhat_unit_state` | ユニットの識別と状態 |
| `lhat_program_diagnostic_count` / `lhat_program_diagnostic` | Program 自身の診断 |
| `lhat_program_error_message` | Program の診断コードをメッセージへ変換 |
| `lhat_unit_diagnostic_count` / `lhat_unit_diagnostic_write` | ファイル内の診断を列挙・表示 |
| `lhat_unit_source` | 診断位置に対応するソース |

`lhat_unit_diagnostic_write(unit, index, rich, out, capacity)` は必要バイト数を返します。
`rich` はソース位置や該当箇所を含む表示を選びます。

## 公開値の契約と型情報

```c
bool lhat_unit_export_conforms(const LhatUnit *unit, const char *name,
                               const char *signature);
```

公開値がシグネチャに適合するかを検査します。名前がない場合や型文字列が不正な場合も false です。
名前は `lhat_unit_export_count` / `lhat_unit_export_name` で列挙できます。
`lhat_unit_export_type_text` は型の文字列表現、`lhat_unit_export_type` は `LhatRuntimeType` 記述子を返します。
後者はコンパイル済み本体が必要で、再読込や破棄で無効になります。

## リロード

```c
size_t lhat_reload(LhatProgram *program, const char *path,
                   LhatMachine *const *machines, size_t machine_count);
```

`machines` は当該 Program をインストールした全 Machine です。安全な停止点で呼び出します。
戻り値は無効化したユニット数で、0 は変更なし、`SIZE_MAX` は未知のパスです。
再検査の成否は診断などから別途確認します。

| API | 用途 |
| --- | --- |
| `lhat_program_invalidate` | 変更ユニットと、それを必要とするユニットを無効化。旧コードは解放しない |
| `lhat_machine_forget_unit` | Machine に登録されたモジュールを忘れ、次回実行で作り直せるようにする |
| `lhat_program_retired_count` | 回収待ちの旧コードの数 |
| `lhat_program_discard_unheld` | 全対象 Machine を GC し、保持されていない旧コードを回収。完了なら true |
| `lhat_program_discard_retired` | 全旧コードを無条件に解放。実行中・保持中の旧コードがないことをホストが保証する場合だけ使用 |

## 配布とエディタ情報

| API | 契約 |
| --- | --- |
| `lhat_unit_write_binary` | コンパイル済みユニットを出力。`LhatWriteStatus` を確認し、受け取った bytes を `lhat_free` |
| `lhat_program_is_binary_unit` | バッファがバイナリユニットの形式かを識別 |
| `lhat_program_write_signatures` | フルビルドで API 登録後に VM 用シグネチャ表を出力 |
| `lhat_program_read_signatures` | VM のみのビルドで API 登録前にシグネチャ表を読み込み |
| `lhat_program_dump_host_api` | 登録済み API をエディタ用 JSON として出力。`(NULL, 0)` でサイズ測定 |

バイナリの互換条件と配布手順は[リロードと配布](../embedding/deployment.md)を参照してください。
