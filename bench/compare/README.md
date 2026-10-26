# L^ / Luau / Lua 比較ベンチマーク

同じ入力・アルゴリズムから同じチェックサムを得る 6 ケースを、独立した L^ VM、Luau VM、本家 Lua 5.5.1 で実行する。L^ 本体に比較対象の依存を追加せず、独立 CMake プロジェクトとしてビルドする。LuaJIT は対象に含めない。

これは [比較計画](../luau-plan.md) の初期版。Release VM 比較、正解確認、複数プロセスでの測定、生データ保存、Markdown レポート生成までを実装している。PGO/JIT、Luau の型解析、VM ヒープのバイト計測、ホスト API・値型の比較は今後の拡張。

最適化前の基準値は [baseline.md](baseline.md)、Lua との速度差を2倍以下にした変更と検証結果は [optimization.md](optimization.md) を参照。

## Windows x64 で実行する

必要なものは Git、CMake 3.25 以上、Ninja、Python 3.10 以上、Visual Studio の C/C++ ツールチェーンと「C++ Clang tools for Windows」。以下はリポジトリのルートから PowerShell で実行する。L^ の Windows 配布版と同じ Visual Studio 付属 `clang-cl` を、3 VM すべての C/C++ コンパイラに明示する。

```powershell
# ネットワークを使う準備はこのコマンドだけ。Luau の SHA と Lua の版・SHA-256 を固定。
python bench/compare/prepare.py

. ./scripts/devshell.ps1
cmake -S bench/compare -B build/compare-clang -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
cmake --build build/compare-clang --target lhat_compare luau_compare lua_compare -j 8

# CI 相当：正解確認とハーネスのテスト。速度による合否判定はしない。
ctest --test-dir build/compare-clang --output-on-failure

# 標準測定：5 プロセス × 3 ウォームアップ + 10 測定サンプル／ケース・実装。
python bench/compare/run.py
```

`results/<UTC 時刻>/` に `results.json`、`report.md`、CMake のキャッシュ、コンパイルコマンド、各ケース・各 VM のバイトコードを出力する。Lua の一覧には公式ソースの `luac` を同じ構成でビルドした `lua_bytecode` を使う。`results/` と依存ソース・ビルド出力は Git 管理外。出力先に既存の `results.json` がある場合は上書きしない。

用途別のコマンド：

```powershell
# 数秒で計算結果だけ確認する。
python bench/compare/run.py --verify

# 開発中の短い測定。標準測定と条件が違うので結果を混ぜない。
python bench/compare/run.py --quick

# 一部のケースだけ再測定する。
python bench/compare/run.py --case array --case entities

# 選択構成の比較：L^ / Luau は Clang、Lua は MSVC（既存の MSVC ビルドを指定）。
python bench/compare/run.py --lua-build-dir build/compare

# Luau の既定最適化レベル O1 は別のビルドディレクトリで比較する。
cmake -S bench/compare -B build/compare-o1 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCOMPARE_LUAU_OPT_LEVEL=1
cmake --build build/compare-o1 --target lhat_compare luau_compare lua_compare -j 8
python bench/compare/run.py --build-dir build/compare-o1

# 保存した測定データからレポートを再生成する。
python bench/compare/report.py bench/compare/results/<時刻>/results.json
```

`run.py` の既定ビルドディレクトリは `build/compare-clang`。別のビルドには `--build-dir` を指定する。コンパイラを切り替える際は既存キャッシュを流用せず、新しいディレクトリを使う。以前の `build/compare` は MSVC の測定用として残している。

L^ は `clang-cl` で computed goto による命令ディスパッチを使い、MSVC の `cl.exe` では switch を使う。したがって MSVC の結果は L^ の Windows 配布構成の速度を表さない。各処理系の既定のディスパッチ条件を使用し、実際のコンパイラとオプションを結果に保存する。

固定版の Luau も `clang-cl` では computed goto を使う。本家 Lua 5.5.1 の既定判定は `__GNUC__` のみなので、`clang-cl` では switch のままになる。この比較では Lua の `LUA_USE_JUMPTABLE` を追加で上書きしていない。コンパイラの統一とディスパッチ方式の統一は別の条件として扱う。

`--lua-build-dir` で Lua の別構成を選ぶ場合は、そのビルドの Lua 実行ファイルと luac を一緒に使い、VM ごとのコンパイラをレポートに記載する。各ビルドの構成・コンパイルコマンドは `build-lhat/`、`build-luau/`、`build-lua/` にも保存する。共通の入力・round 数・実行順で同じ測定中に比較するため、以前の別セッションの中央値を合成しない。

同一コンパイラ比較はコンパイラをそろえた際の差を見るもの、選択構成の比較は指定した組み込み構成どうしの速度を見るものとして区別する。Lua の MSVC 版を選ぶことは「Windows では MSVC が常に最速」や「Lua が MSVC 向けに最適化されている」という主張ではない。構成は全ケースで固定し、ケースごとに速い方を採用しない。

Luau の取得先は `prepare.py --dest <パス>`、ビルド時の依存パスは `-DLUAU_SOURCE_DIR=<パス>` で指定できる。Lua はそれぞれ `--lua-dest <パス>` と `-DLUA_SOURCE_DIR=<パス>`。既定では `build/compare-deps/lua-5.5.1` に取得する。

Lua は [公式アーカイブ](https://www.lua.org/ftp/)を取得し、`lua.lock.json` のアーカイブ SHA-256 を検証してから展開する。C ソースとヘッダ全体の SHA-256 も固定し、準備・CMake 構成・実行時に検証する。Luau は `luau.lock.json` のコミット SHA と clean な作業ツリーを要求する。依存ソースに変更がある場合は上書きせず拒否する。固定版を更新するときは lock を編集し、ソースの互換性と正解確認をやり直す。

Linux 用の時計・メモリ取得にも対応するが、最初の検証環境は Windows x64。Linux では devshell を省き、同じ系列の C/C++ コンパイラを使用する。

## ケースと計算の意味

各スクリプトは `kernel(rounds, size, seed)` 相当の関数を返す。入力はホストから実行時に渡す。`seed` は決定的な初期値であり、乱数生成器の seed ではない。実行順を決める乱数の seed は別に `--order-seed` で指定する。

`rounds` は同じワークロードの繰り返し数であり、3 VM に同じ値を渡す。以下の準備処理は **各 round の仕事の一部として計測に含む**。スクリプトの読み込み・チェック・コンパイル・ロードは定常実行に含めない。

| ケース | 標準入力 size / seed | 1 round の処理 |
| --- | --- | --- |
| arithmetic | 4096 / 17 | seed で初期化した合計に i と、分岐で選ぶ 1 または 3 を加算 |
| calls | 16 / 12 | 素朴な再帰による fib(seed) を size 回呼び出す |
| array | 512 / 17 | size 要素の表を初期化し、4 回の走査で全要素を加算・集計 |
| fields | 4096 / 17 | 2 フィールドの表を 1 個生成し、size 回更新・集計 |
| allocation | 1024 / 17 | 2 フィールドの短命な表を size 個生成し、フィールドを読み取って集計 |
| entities | 256 / 17 | size 個の位置・速度レコードを生成し、32 フレーム更新・集計 |

最初のスイートは整数値の演算を測る。L^ と Lua は整数、Luau は double で計算し、チェックサムは 2^53 以下に制限する。Lua の入力には `lua_pushinteger` を使う。整数オーバーフローや浮動小数点の丸め差を比較対象に含めない。浮動小数点性能を主張するためには、別途その入力と誤差許容を持つケースを追加する。

配列は L^ の 0 ベース、Luau / Lua の 1 ベースで、同じ順序・内容を格納する。L^ の添字アクセスが返す nil の可能性には、array で `?? 0`、entities で存在検査を使う。Luau / Lua にも対応する `or 0`・存在検査を記述するが、最適化後の検査回数や命令数の一致は求めない。Luau / Lua の添字には基準を合わせるための `+ 1` が含まれる。Lua の複合代入に相当する箇所は通常の再代入で記述する。

L^ のコードは strict モードで検査する。Luau は型注釈を持つが、このハーネスでは解析器を呼ばない。Lua のコードは型注釈なし。Luau のバイトコードは既定で O2、debugLevel=0。C/C++ は Release、JIT・PGO と L^ のデバッガを無効にする。Lua は公式ソースの VM と通常のパーサ／コード生成を使う。GC は各 VM の通常設定のまま有効にし、測定前の強制 GC は行わない。

3 ホストとも標準ライブラリを登録せず、ケースが使う VM コアだけを初期化する。Luau のグローバル環境は sandbox 化する。今後ライブラリを使うケースを追加する場合は、登録内容と起動・メモリの計測条件を更新する。

チェックサムの期待値は Python の閉じた式で求め、再帰ケースは独立した反復計算で求める。VM 同士の一致だけでは正解としない。3 VM とも、標準入力と `(size, seed) = (1, 0), (7, 3)` のそれぞれで 2 rounds を検証する。ウォームアップと全測定サンプルでも期待値を検証する。

## 測定・集計方法

1. 全ケースでバイトコードを保存し、入力を使ったループや呼び出しが残っているか確認できるようにする。ダンプは別プロセスで実行し、計測しない。
2. 予備測定で 3 VM のうち遅い実装の 1 サンプルが約 250 ms になる共通 round 数を選ぶ。速い実装はそれより短くなる。1 round が既に長い場合や正確な整数範囲の上限に達した場合は増やさない。全予備測定も保存する。
3. 各実装を新規プロセスで 5 回ずつ起動する。各プロセスで 3 ウォームアップ後、同じ VM 上で 10 サンプルを測る。各呼び出しの状態はスクリプト内で初期化するが、VM と GC の状態は保持する。
4. ケースごとに最初の実行順を固定 seed で選び、プロセスのグループごとにローテーションする。3 VM を同時には走らせない。
5. プロセスごとのサンプル中央値を取り、その 5 個の中央値の中央値を代表値にする。表示する最小・最大もプロセス中央値の範囲であり、信頼区間ではない。プロセス内のサンプルを独立した測定として水増ししない。
6. 比率は `L^ 時間 / Luau 時間` と `L^ 時間 / Lua 時間`。1 未満なら L^ が速い。各実装の実時間も表示し、単一の総合スコアは作らない。以前保存した 2 VM の JSON からも従来の列でレポートを再生成できる。

1 サンプルはホストからカーネルを呼び、戻るまで。ホストとの境界を 1 回含む。ログ出力・期待値検証は区間外。Windows の時計は QueryPerformanceCounter。`--timeout` は子プロセス全体の上限（既定 120 秒）であり、1 サンプルの上限ではない。

負荷の大きいアプリを避け、同じ電源設定で測る。CPU コア固定や優先度変更は自動では行わない。結果はそのマシンとビルド条件に属し、別 OS・コンパイラへ一般化しない。

## 起動・準備・メモリの読み方

定常実行とは別に、3 実装それぞれを 5 回、新規プロセス・ウォームアップなし・1 round で実行する。

- `first_result_from_main_ns`：main の開始から最初の計算結果まで。OS によるプロセス作成は含まない。
- `process_wall_ns`：Python ランナーが起動を開始してからプロセス終了まで。プロセス作成・出力・破棄も含むので、純粋な起動時間とは区別する。
- `peak_process_bytes`：Windows ではプロセスのピーク working set。コンパイラや初期化も含み、VM ヒープのバイト数ではない。定常実行の値も生データには保存するが、レポートのメモリ欄はこの新規プロセス測定を使う。
- L^ の `check_load_ns`：ソース読み込み・構文解析・strict 検査の合計。`compile_ns` は検査後のバイトコード生成。
- Luau の `read_ns`：ソース読み込み。`compile_ns` は構文解析とバイトコード生成。型解析は実行していないため `typecheck_ns` は null。
- Lua の `read_ns`：ソース読み込み。`compile_ns` は `luaL_loadbufferx` による構文解析とバイトコード生成。型検査はなく `typecheck_ns` は null。Lua は生成したコードをその VM に直接ロードする。

言語ごとに準備処理の内容が異なるので、準備時間を同じ仕事としてランキングしない。OS のファイルキャッシュは消さないため、新規プロセス測定をディスクのコールド読み込みと解釈しない。

## 成果物・失敗の扱い

`results.json` は全サンプル、チェックサム、予備測定、新規プロセス測定、実行条件、OS/CPU、C/C++ コンパイラ、L^ / Luau の SHA と変更状態、Lua のバージョンとアーカイブ・ソースの SHA-256、実行ファイルとケースの SHA-256 を持つ。実際のコンパイルオプションは同じ出力ディレクトリの `compile_commands.json` と `CMakeCache.txt` で確認できる。

入力不正、ビルド失敗、結果不一致、不正な測定時間、タイムアウトは成功結果に含めない。失敗したケースはレポートに明記して非ゼロで終了する。他のケースの測定は続行し、完了したプロセスの結果を逐次保存する。

## 拡張時の方針

- PGO は両実装に同じ訓練条件を用意し、通常 Release と別の条件として出力する。この初期版は PGO を明示的に OFF に固定する。
- JIT は対応ケースとフォールバックを調べ、生成時間と定常実行を別々に測る。
- ヒープ計測は計測専用ビルド／別パスで行い、割り当て計測のコストを実行時間に混ぜない。
- ホスト API・ベクトルは共通の処理と固有機能による処理を区別する。

Luau の最適化方針：[How we make Luau fast](https://luau.org/performance/)。固定版の API・オプションは取得したソースを正とする。
