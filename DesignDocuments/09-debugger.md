# デバッガ

`L^` (lhat) のデバッグ機構の設計。機械が行に達したことを教えるフックと、
そのフックの中でフレームと束縛を読む公開 API、そしてそれを使う二つの利用者
——`lhat --dap` のデバッグアダプタと、Godot エディタのデバッガ——を扱う。

節番号のない参照（`14.9` など）は [02-syntax.md](02-syntax.md) を指す。

## 1. 位置づけ

処理系本体の仕様ではなく、処理系を利用するツールの土台。言語の意味論には
影響しない。デバッグ情報（行・名前）は proto に載るが、型にも同一性にも
参加しない（14.9）。

利用者は二つある。どちらも C 側にいる。

- **デバッグアダプタ**（`dap/`）——DAP を喋る相手（VSCode 拡張など）と
  ソケットで話す。
- **Godot のデバッガ**——DAP を喋らない。`ScriptLanguageExtension` の
  `_debug_get_stack_level_*` を実装し、行ごとに `EngineDebugger` へ
  ブレークポイントを問い合わせて止まる。

Lua は `debug` ライブラリとして同じことをスクリプトから操作できるようにして
いるが、`L^` は踏襲しない。あれはデバッガを Lua で書くための窓口であり、
`L^` のデバッガはホスト (C) にいて、その窓口はこの章の公開 API である。
`setlocal` に当たるものは静的型を壊し、`sethook` に当たるものは 10.6 の
純粋性に穴を開ける。`std.debug`（log と traceback）は現状のままとする。

## 2. 機械のフック

機械はデバッガのフックを一つ持つ（machine ごと。proto ではない——一つの
proto は `std.thread` で複数の機械に共有されうる）。フックが立っている間、
機械は命令の合間ごとに「新しい行に達した」、または run がフォルトしたことを
フックに知らせる。長く走るホスト呼び出しは、自ら選んだ安全な区切りも知らせ
られる。止まる・歩く・中断するの判断はフックの側にあり、機械は
ブレークポイントの表を持たない。

```c
typedef void (*LhatDebugHook)(LhatMachine *, void *context,
                              LhatDebugEvent, const LhatFrameInfo *where);
void lhat_machine_set_debug_hook(LhatMachine *, LhatDebugHook, void *context);
```

### 2.1 行イベント

フックは命令の実行直前、GC ステップと同じ境界で呼ばれる——すべての生きた値が
レジスタ・フレーム・open list のいずれかにある唯一の安全点である。呼ばれる
のは、その命令が**新しい行を始める**ときに限る。規則は Lua のもの:

- 直前と違う行に入ったとき
- 同じ行へ後方ジャンプで戻ったとき（ループの一周）
- ある本体に入って最初の命令のとき

フレームに入った/戻ったことは**フレーム数の変化**で判る。だから `CALL` /
`RETURN` / `RESUME` / `YIELD` のどの命令にも印を足さない。フレーム数が動いた
ときは、去った命令（呼び出した `CALL`、再開した `RESUME`、譲った `YIELD`）の
行と比べる。

帰結を表に示す。

| 遷移 | フックは鳴るか |
| --- | --- |
| 同じ行を前進 | 鳴らない |
| 違う行へ前進 | 鳴る |
| 同じ行へ後方ジャンプ（ループ） | 鳴る |
| 呼び出しで新しいフレーム | 被呼び出し側の先頭で鳴る |
| 呼び出しから戻る | 戻り先が呼び出しと同じ行なら鳴らない |
| 末尾呼び出し | 被呼び出し側の先頭で、同じ深さで鳴る |
| コルーチンの `yield^` と再開 | 行が違えば鳴る |

戻り先が呼び出しと同じ行なら鳴らないので、`step over` が同じ行に二度
止まらない。

#### フォルトイベント（D5）

`LHAT_DEBUG_FAULT` は **run が失敗するときに一度だけ**鳴る。`OK` と、再開を
待つだけで失敗ではない `SUSPENDED` は鳴らさない。`vm_finish` がフォルトの
フレーム範囲・命令位置・状態を記録した直後、run が結果を返す前に呼ぶので、
フックの中では通常のフレーム・束縛 API で原因行の状態を読める。

```c
LhatRunStatus lhat_machine_fault_status(const LhatMachine *);
LhatValue     lhat_machine_fault_value(const LhatMachine *);
```

状態は `LhatRunStatus`、値は `panic^` が運んだ値（それ以外は `nil^`）であり、
次に machine を走らせるまで残る。デバッガがフックで待機してから続行を受ける
ことはできるが、**復旧ではない**——続行すると元のフォルト結果を返して run は
終わる。

`CALL` / `RETURN` のイベントは足さない。step in / over / out は既に行イベントと
フレーム深さの変化で区別でき、末尾呼び出し・コルーチン・cleanup に別のイベント
意味論を持ち込む利益がない。

### 2.2 費用

フックが立っている間、命令ごとに払うのは判定一つ。立っていない間は、通らない
分岐一つ（実質ゼロ）。空のフックを立てて空ループを回した実測で、一周あたり
の増分はおよそ 50 ns（`bench` のケース 14）——判定・フレーム情報の作成・
呼び出しの合計であって、判定だけの値ではない。

デバッグ情報の表（行、レジスタの名前）は、検査が走ったかどうかに関わらず
常に残す。03 §4.2 のとおり、実行するものは検査したかどうかに依らない。

出荷ビルドはその分岐一つも払わない。`LHAT_WITH_DEBUGGER`（CMake option、
既定 ON）を OFF にすると、フック・監視者（§3.4.1）・内観と評価（3 章）が
前処理で消え、実行ループの判定ごと無くなる——GDScript のリリースビルドが
検査を消すのと同じ形である。フォルトのトレースバック（04 §11.6改の3 API）は
デバッガではないので残る。ノブは LhatMachine の形を変えるため、生成ヘッダ
（version.h）に載る。`LHAT_BUILD_DAP` はこのノブを要求する。

### 2.3 フックの性質

フックは機械自身のスレッドの上で、命令の合間に呼ばれ、機械はフックが返るまで
待つ。プログラムを止めたいデバッガはここで止める（ブロッキング）。Lua の
`lua_sethook`、Godot の `script_debug` と同じ形である。DAP のように run から
抜けて後で再開する仕組み（Luau の `LUA_BREAK`）は要らない。

### 2.4 再入とフォルト

フックの中からフックは、`lhat_machine_call` などで L^ を呼び戻せる。その呼び
戻しの中の行はフックに知らされない（フックが走っている間、機械の内側のフラグ
がフックを外している）。呼び戻しがフォルトしたときは、ホスト関数がフォルトした
ときと同じに、フックが割り込んだ run をそのフォルトで終える。`lhat_machine_panic`
をフックから呼べば run を止められる——アダプタは切断でこれを使う。

### 2.5 コルーチン・末尾呼び出し・cleanup

これらはすべて 2.1 の深さ比較で自然に扱われ、専用の分岐はない。

- **末尾呼び出し**（03 §5.3）——被呼び出し側の本体が同じ深さで始まるので、
  その先頭行がその深さで鳴る。
- **コルーチン**——`yield^` の両側はそれぞれ自分の番に鳴る。再開は行が違えば
  鳴る。
- **`finally^` / `with^` の後始末**（10.7）——後始末の本体の行で鳴り、戻り先で
  必ず鳴る。

### 2.6 ［補足］境界の前でフレームの行を書く

フックが読む最内フレームの行は、そのフレームの保存された `pc` から引く（04 §11.6）。
ホスト関数の呼び出しは機械が制御を外へ渡す唯一の地点で、そこでは `pc` が
保存されていなかった——だから `std.debug.traceback` をホストから読むと最内行が
一つ前の呼び出しの行にずれていた。フックがこの `pc` を要にするので、ホスト境界を
またぐ前に必ず書くようにした。同じ理由で、入れ子の run が `run_base` を戻し、
run の開始で前の run のフォルト記録を消す。

### 2.7 ホストの協調停止点（D3）

```c
bool lhat_machine_debug_pause_point(LhatMachine *);
```

長く走る `LhatHostFn` は、途中で安全に止まり、答えをまだ渡していない区切りで
これを呼ぶ。フックには `LHAT_DEBUG_HOST_PAUSE_POINT` が、呼び出した L^ の行を
`where` として届く。フックが返るまで待ち、`true` なら仕事を続け、`false` なら
フックが現在の run を終えたので、答えを書かず後始末して return する。

これは行イベントではない。DAP は `pause`、すでに始まった全台停止、切断だけを
ここで処理する。ソースの breakpoint と step は命令行だけで判定するので、ホスト
内部の都合で step の意味が動かない。フックの再入中とフック無しは何もせず `true`。
`LHAT_WITH_DEBUGGER=OFF` でも API は残り常に `true` なので、ホストは条件付き
コンパイルを要さない。

協調点は、デバッガや他 machine が必要としうる錠を持たずに呼ぶ。sample stdlib の
`std.async.wait`、`std.channel` の `demand` / `supply`、`std.thread.sleep` /
`join`、`std.task.await` は待機を最大約 20 ms に区切ってこの規則に従う。

同期 I/O（`std.io.readLine` など）、すでに開始した resource の破棄、task pool の
停止のように安全な中断点を作れないホスト呼び出しは対象外である。`pause` は受理
されても次の行または協調点まで **pending** のままで、切断・terminate もその
呼び出しを即時には止められない。セッションは安全性のため従来どおりその return
を待ち、timeout や detach はしない。

## 3. 内観

### 3.1 フレーム

フックの中では、既存のフレーム歩き API がそのまま使える。フォルトの記録が
無いときは**いま立っているフレーム**を歩く——機械は二つの命令の間にいて、
どのレジスタもプログラムが見るとおりの値を持つ。

```c
size_t lhat_machine_fault_depth(const LhatMachine *);
bool   lhat_machine_fault_frame(const LhatMachine *, size_t level, LhatFrameInfo *);
size_t lhat_machine_traceback(const LhatMachine *, char *out, size_t capacity);
LhatRunStatus lhat_machine_fault_status(const LhatMachine *);
LhatValue     lhat_machine_fault_value(const LhatMachine *);
```

名前に `fault` と付くのは、これがフォルトの巻き戻さないフレームを読むために
先に入ったからで（04 §11.6）、フックの中でも同じフレームを読む。

### 3.2 束縛

```c
typedef struct { const char *name; LhatValue value; } LhatBindingInfo;
size_t lhat_frame_local_count(const LhatMachine *, size_t level);
bool   lhat_frame_local(const LhatMachine *, size_t level, size_t index, LhatBindingInfo *);
size_t lhat_frame_upvalue_count(const LhatMachine *, size_t level);
bool   lhat_frame_upvalue(const LhatMachine *, size_t level, size_t index, LhatBindingInfo *);
```

ローカルは、そのフレームの命令で生きている名前を宣言順に返す。合成名
（`self^` `it^` `def^` `super^` `...` `this^`）も返す——選別は利用者の側で
する（Godot の members は `self^` を探す）。同じ名前の影は両方返し、後のものが
内側である。`let^` が宣言する名前はそのブロックの先頭から生きていて、実行前は
`nil^` を持つ（8.7）。捕捉（upvalue）は名前と値で読める。ホスト値は幅に関わらず
ポインタ形（05 §8.9）で返る。

### 3.3 値の展開

型は `lhat_value_type` と `lhat_runtime_type_write`、文字列化は
`lhat_value_text`、テーブルの展開は公開の `LhatTable`（配列部・エントリ部・
`definition`）で足りる。新しい値の API は要らない。

### 3.4 束縛の書き換え

```c
bool lhat_frame_set_local(LhatMachine *, size_t level, size_t index, LhatValue);
bool lhat_frame_set_upvalue(LhatMachine *, size_t level, size_t index, LhatValue);
```

読みと同じ番号で、その束縛へ値を書く。**検査器が約束したことの外にある、
デバッガの特権**である——機械はどの普通の値を書かれてもメモリ安全のまま
（レジスタはタグ付きの値なら何でも持てる）だが、本体の書いた型が予期しない
値は、後でそのとおりの実行時型エラーとして現れうる。これは 03 §4.2 の安全性の
線——SEGV は不可、型エラーでの停止は許容——の内側にある。

拒否される（false、何も書かれない）のは、level や index が何も指さないとき、
そして**束縛か値がホスト値のとき**。ホスト値は登録された幅の生スロット
（05 §8.9）で、そのレイアウトをまたいで書くことだけが安全でない。

GC との折り合い: レジスタへの書きにバリアは要らない（収集器は掃引の前に
ルートを読み直す——gc.c の atomic）。捕捉への書きは `SETUPVAL` 命令と同じ
バリアを敷く。テーブルのメンバへの書きは既存の `lhat_machine_table_set`
（バリア込み）で行う。

### 3.4.1 machine の誕生を観る

```c
typedef struct {
    void *context;
    void (*born)(void *context, LhatMachine *machine);
    void (*dying)(void *context, LhatMachine *machine);
} LhatMachineWatcher;
void lhat_debug_watch_machines(const LhatMachineWatcher *watcher);
```

`lhat_machine_new` の末尾と `lhat_machine_dispose` の冒頭（片付けが走る前）で
呼ばれる、プロセスに1口の観測者。machine を作る道は誰であれこの二点を通るので、
どのスレッド機構の上でも「追うべき machine」がここで全部見える。据えるのは
どの machine も作られていない間、外すのは自分の machine が全部去った後——
デバッグセッションの begin/end がそのまま自然な窓になる。

### 3.5 式の評価

```c
bool lhat_machine_evaluate(LhatMachine *, size_t level,
                           const char *text, size_t length, LhatValue *answer,
                           char *error, size_t error_capacity);
```

一つの入力——8.2 の対話形、裸の式は答え——を、フレームの名前が見える状態で
コンパイルし、機械の上の自分のフレームで走らせる。

- **名前は写し**である。捕捉、次に生きているローカル（内側が影）を評価
  フレームの先頭レジスタへ写し、その位置に種を蒔いたセッション
  （03 §4.3 の REPL と同じ機構）で**無検査**コンパイルする——03 §4.2 の
  とおり、検査せず走らせることは支えられた実行のかたちで、型の齟齬は
  実行時の誤りとして現れ、それがそのまま error に返る
- 入力の中の `:=` は**写しに書く**。フレームへ書き戻すのは §3.4 の仕事
- 失敗（構文・コンパイル・実行のどれでも）は error に文で返り、機械は
  評価の前の姿に戻る——評価のフレームは畳まれ、フォルトの記録も残らない
- **行フックは評価の間鳴らない**。フックの中から呼ばれるのが普通の形で、
  鳴れば再入で自分の停止ループに戻ってしまう
- 答えの値は次に機械が走るまで生きている。ホスト値のローカルと、L^ に
  結ばれていない裸のホスト名（`print` など）は見えない——`L^.modules` を
  辿る綴りは通る

## 4. コンパイラが残す表

`LhatProto` は行の表（命令ごとの行）に加えて、レジスタの名前の表を持つ。
一つのエントリは `{ 名前, from, to, reg, width }` で、`from` はその名前が
生きている最初の命令、`to` は最後の次（パラメータは `to` を閉じない）。捕捉は
`LhatUpvalueDesc` が名前を併せ持つ。どちらもデバッグ専用で、行の表と同じく
実行意味論には参加しない。

名前を宣言する場所とスコープを閉じる場所はコンパイラの二つのヘルパ
（`declare_local` / `release_locals`）に集約され、そこで表が書かれ閉じられる。
名前を空の `nil^` に落とす前置きは、行 0 ではなく本体の最初の行に属す——
そうしないとデバッガが宣言の行を二度歩くことになる。

### 4.1 実行できる次の行

```c
uint32_t lhat_proto_next_instruction_line(const LhatProto *,
                                          uint32_t at_or_after);
```

本体とその内側に書かれた本体の命令表を読み、指定行以後で最小の行を返す。
命令が無ければ 0。バイトコードを公開せずに、DAP がコメントや空行に置かれた
ブレークポイントを次の実行可能行へ寄せるための問いである。

## 5. デバッグアダプタ（`lhat --dap=PORT`）

`lhat --dap=PORT` は 127.0.0.1 のそのポートで待ち、一つのデバッガを受けて
プログラムを走らせる。VSCode 拡張が空きポートを選んで `lhat` を起動し、
インライン中継で繋ぐ。中継はスクリプトの標準出力・標準エラーもプロセスから
捕まえ、セッション固有の `output` イベントにする（§7.2）。

構成:

```text
  VSCode / editor
   └ DAP client ──(TCP, 127.0.0.1)──► lhat --dap=PORT
                                        ├ dap/         セッション・受信スレッド・フック
                                        ├ transport/   Content-Length の枠（lhatls と共有）
                                        └ port/socket  ループバック
```

枠（Content-Length）は言語サーバと同じもので、`transport/` に切り出して
バイトストリーム（`LhatStream`）を受けるようにした——サーバは stdio、
アダプタはソケット、試験はメモリバッファ。中身の綴りは違う: DAP は request /
response / event の三種で、`lsp/rpc.c` の固定した `jsonrpc` ではない。

### 5.1 すべての machine を追う

**DAP の「スレッド」は machine である。** OS スレッドではない——ホストが
どのスレッド機構（std.thread・自前・無し）で machine を走らせるかに依らず、
machine が一つならスレッドは一つ。

セッションは machine の誕生を観る（§5.1 の `lhat_debug_watch_machines`）。
生まれた machine にはその場でフックが立ち、デバッガへ `thread` イベント
（started）が出る。死ぬときに外れて exited。**ホストの配線はゼロ**——
`std.thread` のワーカーも、ホスト自作スレッドの machine も、作られただけで
追われる。

止まり方は**全台停止**（`allThreadsStopped`）。どれかの machine が止まると
デバッガはその1台の `stopped` を聞き、残りは次の行イベントで黙って駐機する。
ソケットを読むのは**受信スレッド1本**で、駐機した machine は凍った資料——
その frames を歩くのも、その上で evaluate を走らせるのも、受信スレッドが
錠越しに行う。動いている machine の stackTrace は空で答える。

- **ライフサイクル**——`lhat_program_install` の後にセッションを始め、
  initialize / setBreakpoints / setExceptionBreakpoints / launch /
  configurationDone を同期に受けて
  観測とフックを据え、受信スレッドを起こしてから run を走らせる。run の後に
  終える（`terminated` と `exited`）——終わりはワーカーの machine が全部
  去るのを待つ。
- **停止と歩き**（各 machine のフックで毎行）——全台停止中である、`pause` が
  要求された、その machine の `stepIn`、`stepOver` で深さが戻った、`stepOut`
  で深さが減った、あるいは行がブレークポイント——のいずれかで駐機する。
  歩きは machine ごと（`next` の `threadId` の1台に効き、resume は全台）。
- **ソースブレークポイント**（D8）——`setBreakpoints` はコンパイル済みの
  program の行表で、要求行以後の最初の命令行を探す。見つけたものは
  `verified: true` と実際の `line` を返し、以後に命令が無ければ
  `verified: false` と理由を返す。要求はその source の古い表だけを置換し、
  他のファイルのブレークポイントは残す。`condition` はヒットした machine の
  level 0 で evaluate と同じ写しの環境に評価され、`true^` のときだけ止まる。
  `false^`、bool 以外、構文・実行の失敗は不一致であり、走行中のプログラムを
  フォルトさせない。
- **フォルト停止**——`LHAT_DEBUG_FAULT` は行を待たず、その machine を
  `stopped(reason: "exception")` で駐機する。全 runtime fault は停止対象で、
  `exceptionInfo` は状態（`panic^` ならその値）を返す。フックの中にいる間は
  フレーム・Locals・evaluate が通常の停止と同じように使え、continue はその
  フォルトを返して run を終える。
- **中断（pause）**——受信スレッドが旗を立て、各 machine は次の行イベントまたは
  §2.7 の協調停止点で止まる。後者も `stopped(reason: "pause")` を出し、既に
  全台停止なら黙って駐機する。協調点の無いホスト呼び出しでは停止は pending のまま。
- **変数の参照**——frameId は `threadId * 1000 + level`。scopes はフレーム
  ごとに Locals と Captures、テーブルは停止ごとの handle 配列（値と machine
  の組）で展開する。停止の間は駐機した machine のレジスタが根なので値は
  生きている。
- **切断**——旗を立て、各フックが自分の machine を
  `lhat_machine_panic_text` で止める。cli はデバッガが止めた run のフォルトを
  自分のエラーとして出さない。

### 5.2 パスの照合はホストの言葉で

デバッガが送るのはエディタのファイルパス、machine が報告するのは単位の綴り
（`LhatFrameInfo.source`）で、両者は同じものとは限らない——アーカイブや仮想
ファイルシステム（PhysFS の `.love` など）から単位を読むホストの単位名は、
ディスクのどこにも無い。対応を知っているのはホストだけなので、
`dap_session_begin` は、行を照合するコンパイル済み `LhatProgram` と写像
（`DapPathMap`）を受け取る:

- `to_unit`——エディタのパス → 単位の綴り。setBreakpoints はこれ越しに
  照合され、以後の行イベントは**単位の綴りどうしの完全一致**（正規化なし）
- `to_editor`——単位の綴り → エディタのパス。stackTrace の source は
  これ越しに報告され、デバッガがファイルを開ける

写像が無ければ（cli）、両側ともファイルシステムのパスとして正規化
（`_fullpath` / `realpath`、Windows は大小無視）して照合する。写像が知らない
ファイルにはブレークポイントが結ばれない。呼び出しはセッションのスレッドから
錠の下で来る——速く、スレッド安全に。

対応する要求（v1）: initialize, launch, attach, setBreakpoints（次の命令行へ
移動して検証、`condition` 対応）, setExceptionBreakpoints, configurationDone, threads, stackTrace,
scopes, variables, setVariable, evaluate, exceptionInfo, continue, next, stepIn,
stepOut, pause, disconnect, terminate。イベント: initialized, stopped, terminated,
exited。

- **setVariable**——パネルが打った文字列を L^ の綴りで読む（`nil^` /
  `true^` / `false^` / 数 / 引用符の文字列。式は evaluate の側）。行き先は
  名前で引き、影があれば内側——読みがパネルに並べたのと同じ規則。テーブルの
  メンバは数だけの名前を列の鍵、それ以外を文字列の鍵として書き戻す。
- **evaluate**——§3.5 をそのまま。ホバーにも答える
  （`supportsEvaluateForHovers`）。答えは描画した文字列だけで、展開の
  参照は配らない——評価の答えはフレームが畳まれた後は何にも根を張られて
  おらず、後から読む参照は腐りうる。
- **条件付きブレークポイント**——`supportsConditionalBreakpoints` を返す。
  条件の `:=` は evaluate と同様に写しへ書くだけだが、呼び出したホスト関数の
  副作用までは抑えない。これはデバッグコンソールの式評価と同じ範囲である。
- **例外ブレークポイント**——`setExceptionBreakpoints` は受理するが、L^ の
  runtime fault は catch^ で捕捉する error^ ではなく run を終える失敗なので、
  caught / uncaught のフィルタはまだ分けない。常に停止する。

`dap/` は `src/` の何も名指ししない——デバッガは `lhat.h` の公開面だけで動く。

### 5.3 停止位置の列

`stackTrace` が返す列は、**アダプタが単位の本文から読む** ——
停止した行の最初の非空白がその列である（`dap_column_of_line`）。
命令ごとの列の表は持たない。04 の 11.6 の「列は持たない」は動かない。

**列は停止の粒度になり得ない。** 行イベントの同一行抑止（§2.1）を
`(行, 列)` に広げると、`a(); b()` で `next` が一行に何度も止まる。
行でしか止まらない以上、列は**どこで止まったかの札**にしかならず、
だから行内ブレークポイントも来ない——`SourceBreakpoint.column` は受け取っても
黙って捨て、`setBreakpoints` の応答にも列を返さない。持っていない精度を
持っているように読めるからである。

**命令ごとの列を持っても、札としてはむしろ悪い。** その行の最初の命令は
最初に評価される部分式のものなので、`total += twice(i)` なら列は行の途中を
指す。読み手が見たいのは文の頭である。加えて記憶域は命令あたり +25%〜+50%、
バイナリ形式は破壊、`LhatFrameInfo` に欄を足せば別リポジトリの Godot
バインディングの ABI を破る——あちらの `is_breakpoint(line, source)` に列は
無いので、払った分は丸ごと死荷重になる。

数え方は字句解析器のものと自然に一致する。跨ぐのは `' '` と `'\t'` だけで
どちらも1バイトなので、答えの手前は必ず ASCII であり、バイト・コードポイント・
UTF-16 単位が同じ数になる。本文は `LhatSource` が LF に正規化済み（01 の 3 章）。

列 1 に落ちるのは、源の名前が無い・その名前の単位が無い（対話入力）・
単位に本文が無い（バイナリから読んだ）・行が末尾を越えている・行が空か
全部空白のとき。DAP はこれを「見るに値しない列」として読む。

走査は本文一回分をフレームごとに払う。**人が「今どこか」を訊いたときにしか
走らない**（命令ごとには決して走らない）ので、覚え書きは置かない。

## 6. Godot

Godot 側は DAP を喋らない。行フックを据え、`where` の行と源を
`EngineDebugger::is_breakpoint(line, source)` に問い、当たれば
`script_debug(this, true, false)` で止まる。歩きは拡張がフレーム数の差で数える。
止まっている間、`_debug_get_stack_level_locals` / `_members` はこの章の束縛
API で埋まる。詳細は別リポジトリの godot バインディングにある。

## 7. VSCode 拡張

`contributes.debuggers` に `{ type: "lhat", languages: ["lhat"] }` を持ち、
`DebugAdapterDescriptorFactory`（`vscode-extension/src/debug.ts`）が
空きポートを選んで `lhat --dap=PORT <program>` を起動し
インラインの中継を返す。中継が実装するのは DAP の枠とメッセージをそのまま
通すこと、それに標準出力・標準エラーを `output` イベントとして差し込むことだけで、
要求・応答・デバッグの意味を実装しない。**アダプタは実行時そのもの**である。

`contributes.breakpoints` に `lhat` を挙げるので、`.lh` の行に赤丸が置ける。
`launch.json` を書かなくても F5 で開いている `.lh` が走る
（`DebugConfigurationProvider` が `program` を埋める）。

### 7.1 いつ繋いでよいかを実行時が言う

**接続の合図が要る。** ソケットが上がるのはプログラムを読み終えて検査した後で、
その時間はプログラムの大きさ次第。しかも型に誤りがあればそこまで辿り着かない。
起動した側が頃合いを測れば、大きなプログラムでは早すぎ、小さなプログラムでは
競合する。

だから `dap_session_begin` は listen が通った直後、accept で塞がる前に、
**標準エラーへ1行書く**:

```text
lhat: dap listening on 51234
```

プロトコルが使わない側の流れなので、これで邪魔になるものは無い。拡張はこの行を
待ち、行が来る前にプロセスが死んだら**そこまでに溜めた出力をそのまま誤りとして
見せる** —— よくある失敗は型の誤りで、溜まっているのはその診断そのものである。

行の照合は改行まで含める。数字の途中でチャンクが割れると、その時点の桁だけで
一致してしまう。

### 7.2 出力はデバッグコンソールへ

プログラム自身の標準出力・標準エラーは実行時の `output` イベントではなく、
**起動したプロセスの管**である。拡張のインライン中継が両方を汲み、`stdout` /
`stderr` の `output` イベントにして VSCode へ渡す。イベントはデバッグセッションに
結び付くので、同時に二つ走らせても出力は混ざらない。中継が実行時から来た DAP の
`seq` を自分の連番に置き換えるので、差し込んだイベントとも一つの送信列になる。

パイプ相手の C の `stdout` は全バッファになるため、`print`、`std.io.print`、
`std.debug.log` は各行を `fflush(stdout)` する。止まった行までの出力を、終了時まで
待たずに読めるようにするためである。

## 8. 未決事項一覧

| 番号 | 内容 |
| --- | --- |
| D6 | 写像なしの照合でのシンボリックリンク等の同一視（§5.2 の正規化は fullpath どまり） |

## 改定履歴（要約）

- 新設。行フック・束縛の内観・コンパイラの名前の表・DAP アダプタ・Godot 連携。
- 束縛の書き換え（§3.4）と `setVariable` を追加。D1 は式の評価だけ残る。
- 式の評価（§3.5）と `evaluate` を追加。D1 閉鎖——評価は写しの上で走り、
  書き戻しは setVariable。
- 全 machine 対応（§5.1）。machine の誕生の観測・全台停止・受信スレッド。
  D7 閉鎖、pause の覗き見は受信スレッドに置き換え。
- パスの写像（§5.2、`DapPathMap`）。仮想ファイルシステムのホスト（lhatove）
  のための editor↔unit の両方向解決。
- `LHAT_WITH_DEBUGGER`（2026-08-31）。出荷ビルドがデバッガを前処理で消す
  ノブ。トレースバックは残る——フォルト報告はデバッガではない。
- D2 閉鎖。VSCode 拡張のインライン中継が子プロセスの標準出力・標準エラーを
  セッションごとの DAP `output` イベントにする。`print` 系はパイプでもその場で
  見えるよう行ごとに stdout をフラッシュする。
- D5 閉鎖。`LHAT_DEBUG_FAULT` はフォルト記録後・run の返却前に一度鳴り、
  DAP は `exception` 停止と `exceptionInfo` でフレームと理由を見せる。CALL /
  RETURN のイベントは、既存の行と深さで歩けるため採らない。
- D8 閉鎖。ブレークポイントは次の命令行へ寄せ、無ければ未検証として理由を
  返す。条件は現在フレームで評価し、`true^` のヒットだけを停止にする。
- D3 閉鎖。長時間のホスト関数は協調停止点を呼べ、DAP の pause・全台停止・
  terminate をそこで処理する。同期 I/O と中断不能な後始末は pending のまま
  即時停止を保証しない。
- D4 閉鎖。列はアダプタが本文から読む（§5.3）——停止した行の最初の非空白。
  命令ごとの表は採らない: 行でしか止まらない以上、列は札にしかならず、
  命令の列は行の途中を指すので札としても悪い。04 の 11.6 は動かない。
