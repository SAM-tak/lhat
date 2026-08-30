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
機械は命令の合間ごとに「新しい行に達した」ことをフックに知らせる。止まる・
歩く・中断するの判断はフックの側にあり、機械はブレークポイントの表を持たない。

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

### 2.2 費用

フックが立っている間、命令ごとに払うのは判定一つ。立っていない間は、通らない
分岐一つ（実質ゼロ）。空のフックを立てて空ループを回した実測で、一周あたり
の増分はおよそ 50 ns（`bench` のケース 14）——判定・フレーム情報の作成・
呼び出しの合計であって、判定だけの値ではない。

デバッグ情報の表（行、レジスタの名前）は、検査が走ったかどうかに関わらず
常に残す。03 §4.2 のとおり、実行するものは検査したかどうかに依らない。

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

## 3. 内観

### 3.1 フレーム

フックの中では、既存のフレーム歩き API がそのまま使える。フォルトの記録が
無いときは**いま立っているフレーム**を歩く——機械は二つの命令の間にいて、
どのレジスタもプログラムが見るとおりの値を持つ。

```c
size_t lhat_machine_fault_depth(const LhatMachine *);
bool   lhat_machine_fault_frame(const LhatMachine *, size_t level, LhatFrameInfo *);
size_t lhat_machine_traceback(const LhatMachine *, char *out, size_t capacity);
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

## 5. デバッグアダプタ（`lhat --dap=PORT`）

`lhat --dap=PORT` は 127.0.0.1 のそのポートで待ち、一つのデバッガを受けて
プログラムを走らせる。VSCode 拡張が空きポートを選んで `lhat` を起動し、
`DebugAdapterServer` で繋ぐ。スクリプトの標準出力は端末のまま（拡張が
プロセスを捕まえる）。

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
  initialize / setBreakpoints / launch / configurationDone を同期に受けて
  観測とフックを据え、受信スレッドを起こしてから run を走らせる。run の後に
  終える（`terminated` と `exited`）——終わりはワーカーの machine が全部
  去るのを待つ。
- **停止と歩き**（各 machine のフックで毎行）——全台停止中である、`pause` が
  要求された、その machine の `stepIn`、`stepOver` で深さが戻った、`stepOut`
  で深さが減った、あるいは行がブレークポイント——のいずれかで駐機する。
  歩きは machine ごと（`next` の `threadId` の1台に効き、resume は全台）。
- **中断（pause）**——受信スレッドが旗を立て、各 machine は次の行イベントで
  止まる。ホスト呼び出しの中にいる machine はその境界まで止まらない（D3）。
- **変数の参照**——frameId は `threadId * 1000 + level`。scopes はフレーム
  ごとに Locals と Captures、テーブルは停止ごとの handle 配列（値と machine
  の組）で展開する。停止の間は駐機した machine のレジスタが根なので値は
  生きている。
- **切断**——旗を立て、各フックが自分の machine を
  `lhat_machine_panic_text` で止める。cli はデバッガが止めた run のフォルトを
  自分のエラーとして出さない。

対応する要求（v1）: initialize, launch, attach, setBreakpoints（行はすべて
`verified` 固定）, configurationDone, threads, stackTrace, scopes, variables,
setVariable, evaluate, continue, next, stepIn, stepOut, pause, disconnect,
terminate。イベント: initialized, stopped, terminated, exited。

- **setVariable**——パネルが打った文字列を L^ の綴りで読む（`nil^` /
  `true^` / `false^` / 数 / 引用符の文字列。式は evaluate の側）。行き先は
  名前で引き、影があれば内側——読みがパネルに並べたのと同じ規則。テーブルの
  メンバは数だけの名前を列の鍵、それ以外を文字列の鍵として書き戻す。
- **evaluate**——§3.5 をそのまま。ホバーにも答える
  （`supportsEvaluateForHovers`）。答えは描画した文字列だけで、展開の
  参照は配らない——評価の答えはフレームが畳まれた後は何にも根を張られて
  おらず、後から読む参照は腐りうる。

`dap/` は `src/` の何も名指ししない——デバッガは `lhat.h` の公開面だけで動く。

## 6. Godot

Godot 側は DAP を喋らない。行フックを据え、`where` の行と源を
`EngineDebugger::is_breakpoint(line, source)` に問い、当たれば
`script_debug(this, true, false)` で止まる。歩きは拡張がフレーム数の差で数える。
止まっている間、`_debug_get_stack_level_locals` / `_members` はこの章の束縛
API で埋まる。詳細は別リポジトリの godot バインディングにある。

## 7. VSCode 拡張

`contributes.debuggers` に `{ type: "lhat", languages: ["lhat"] }` を足し、
`DebugAdapterDescriptorFactory` が空きポートを選んで
`lhat --dap=PORT --run <program>` を起動し `DebugAdapterServer(port)` を返す。
標準出力・標準エラーは起動側がデバッグコンソールへ流す。

## 8. 未決事項一覧

| 番号 | 内容 |
| --- | --- |
| D2 | `output` イベント（スクリプトの出力をコンソールへ）。v1 は起動側が捕まえる |
| D3 | ホスト呼び出しの中にいる machine は `pause` も全台停止も次の行まで効かず、そこで止まったままだとセッションの終わりも待たされる |
| D4 | 列（column）の情報。行の表に列は無い |
| D5 | ［提案］`CALL` / `RETURN` / フォルトのイベント。フォルトで止まる |
| D6 | 源のパス照合。シンボリックリンク等の同一視 |
| D8 | ブレークポイントの行の検証（実在する命令の行か）と条件付き |

## 改定履歴（要約）

- 新設。行フック・束縛の内観・コンパイラの名前の表・DAP アダプタ・Godot 連携。
- 束縛の書き換え（§3.4）と `setVariable` を追加。D1 は式の評価だけ残る。
- 式の評価（§3.5）と `evaluate` を追加。D1 閉鎖——評価は写しの上で走り、
  書き戻しは setVariable。
- 全 machine 対応（§5.1）。machine の誕生の観測・全台停止・受信スレッド。
  D7 閉鎖、pause の覗き見は受信スレッドに置き換え。
