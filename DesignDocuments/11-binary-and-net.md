# std.binary・std.stream・std.net — バイナリ保存、ストリームと UDP

**状態: v1 実装済み**（`stdlib/binary.c`・`stdlib/net.c`・`port/socket.c`）。
スキーマ不要のテーブル保存とメモリ Reader／Writer も実装済み（7 節）。
圧縮・暗号化ラッパーは将来の拡張とし、ここでは接続する契約を定める。

`std.binary` はテーブルをビット単位で詰めてバイト列にし、戻す。`std.net` は UDP の
ソケットを開き、そのバイト列を送受信する。どちらも標準ライブラリで、特定のホスト
（LÔVE など）に依らない。

## 1. 動機 [binary.why]

対戦ゲームのロールバック型ネットコードは、毎フレームの入力を相手へ送る。
1 フレームの入力はボタンが数個とスティックの向きくらいで、詰めれば数バイトに
収まる。テキストやテーブルの汎用の書き出し（LTON、[lton.host]）で送ると何倍にも
膨らむ。**1 ビットも無駄にせず詰める道具**が要る。

送る手段も要る。オンラインの対戦は、マッチングと NAT 越えとリレーを持つ
サービス（EOS の P2P など）に乗せるのが普通で、L^ からは拡張（eos-lhat）で
使える。だが次の 2 つはそれでは賄えない。

- **ネットが無い場所での対戦。** 大会会場の LAN など。EOS の P2P は EOS への
  ログインが前提である
- **開発と検証。** 1 台で 2 つのプロセスを loopback でつなぎ、遅延や欠落を
  人工的に入れて試す。毎回オンラインのログインとロビーを経由するのは重い

どちらも生の UDP があれば足りる。

LÖVE は Lua 版で LuaSocket・ENet・lua-https を同梱していたが、どれも Lua の
C API で書かれていて L^ へは移せない。LÖVE のために書き直すより、**言語の
標準ライブラリとして持つ**方が、LÖVE 以外のホストでも使える。

## 2. 入れ物は `string^` と `std.binary.Bytes` [binary.containers]

送受信するバイト列は `string^` で運ぶ。

L^ の文字列はバイトの列であって、文字の列ではない（[lex.string]）。作るときに
UTF-8 として検査しないので、任意のバイトも NUL もそのまま持てる。バイト数は
`.size`、文字数は `.length`（[syntax.obj.count]）。**直列化したデータの大きさは
`.size` で測る。** `.length` は先頭バイトの数を数えるだけなので、バイト列に
対しては意味を持たない。

Lua 5.3 の `string.pack` が Lua の文字列にバイト列を詰めるのと同じ立場である。
文字列のまま運べるので、`std.channel` にも LTON にもそのまま流せる。

**使い回す入れ物として `std.binary.Bytes` も持つ。** 文字列は作るたびに新しい
オブジェクトになるので、毎フレーム符号化して送るとそのたびにごみが出る。
`Bytes` は中身を書き換えられる hostdata で、一度作って同じものに書き込み続ける。

符号化と復号は `std.binary` が C の中で行い、L^ 側はテーブルと文字列
（と `Bytes`）だけを扱う。詰めた値のフラグを読むなら言語のビット演算
（[syntax.obj.bits]）を使う。

### 2.1 Bytes［補足］ [binary.containers.bytes]

```lhat
let^ out = std.binary.bytes()
input.encodeInto(frame, out)     # writes over what out held
try^ sock.send(out)
```

| 呼び出し | 答え |
| --- | --- |
| `std.binary.bytes()` | `std.binary.Bytes` |
| `bytes.size()` | `number^` — 今のバイト数 |
| `bytes.toString()` | `string^` — 中身の写し |
| `bytes.dispose()` | — |

バイト列を受け取る側は `string^` と `Bytes` の両方を受ける。`decode`・
`decodeInto` は合併 `string^|std.binary.Bytes` の1本、`send`・`sendTo` は
`string^` 版と `Bytes` 版の2本のオーバーロード（[syntax.obj.override]）である。
書き込む側は `fmt.encodeInto` と `sock.receiveInto` で、どちらも中身を
置き換える。中身を 1 バイトずつ読み書きする口は持たない — 読み書きは
形式（[binary.codec]）を通す。

### 2.2 std.binary の外から Bytes に触る［補足］ [binary.containers.outside]

std.net やネイティブ拡張（05 の拡張 ABI）のように、std.binary の外にある C が
`Bytes` を読み書きするときは、std.binary が登録時に置いた口を引く。

```c
const LhatBinaryInterface *binary =
    lhat_lookup_host_context(program, "std.binary", NULL, "bytes");
```

拡張は関数表の `lhat_lookup_host_context` を使う。答えは `stdlib/binary.h` の
`LhatBinaryInterface` で、`bytes(value)`（dispose 済みなら NULL）と
`resize(bytes, length)` を持つ。読み書きしてよいのは `data` と `length` で、
確保と大きさの変更は `resize` に任せる。

**std.binary が無いプログラムでは NULL が返る。** そのときは `Bytes` 版の
オーバーロードを登録せず、`string^` 版だけを持つ。合併の引数にすると
`std.binary.Bytes` が引けない時点で署名が通らないので、`Bytes` を受ける口は
オーバーロードの1本として足す。std.net もこの形で、`Bytes` を受ける
`send`・`sendTo`・`receiveInto` は std.binary が先に登録されているときだけ現れる。

## 3. std.binary [binary.codec]

### 3.1 形式（Format） [binary.codec.format]

まず**形式**を作る。形式はフィールドの並びで、各行は `{ 名前, 種類 }` である。

```lhat
import^ std.binary

let^ b = std.binary
let^ Input = t^{
    aaa : number^, bbb : number^,
    buttonA : bool^, buttonB : bool^, buttonC : bool^,
}
let^ input = b.format(Input, {
    { "aaa", b.uint(16) },
    { "bbb", b.uint(8) },
    { "buttonA", b.bool() },
    { "buttonB", b.bool() },
    { "buttonC", b.bool() },
})
```

**並びは列で書く。** テーブルの名前付きのキーは書いた順を保たない（
[syntax.for.forms] 。ハッシュ部の順序は約束されない）ので、`{ aaa = 16, bbb = 8 }` の形では
ビットの並び順を決められない。

`std.binary.format` は最初の型値と列を一度だけ読んで、**型付き形式オブジェクト**
（`std.binary.Format<T>`、[modules.hostdata] の hostdata）にする。上の `input` は
`std.binary.Format<Input>` である。次をここで検査する。

- 名前が文字列で、重複していない
- 各種類の幅が 1〜64 ビットに収まる
- 総ビット数（上限 2³² ビット［補足］）
- 種類の木が復号する構造が、指定した型 `T` に適合する

誤りは panic である。形式はプログラムの書き手が書くものなので、間違いは
プログラマの誤りになる（04 の線引き）。

形式オブジェクト自身は可変個数のフィールド定義を持つので、ホスト値
（[modules.hostvalue] 、248 バイト上限・スタック限定）にはできず、hostdata になる。これは
符号化される値の幅が可変という意味ではない。

#### 種類も hostdata である［補足］

`b.uint(16)` などが返す種類は `std.binary.Kind`（hostdata）で、`Format` は
その派生型（[modules.hostdata.base]）である。だから形式をそのまま種類として書ける（[binary.codec.kinds]）。
`format` と `array` は渡された種類を**写して**持つので、元の種類や形式を
`dispose` しても、それを含む形式は壊れない。

### 3.2 種類 [binary.codec.kinds]

| 種類 | 幅 | L^ の値 | 備考 |
| --- | --- | --- | --- |
| `uint(n)` | n | 0 〜 2ⁿ−1 の整数 | |
| `int(n)` | n | −2ⁿ⁻¹ 〜 2ⁿ⁻¹−1 の整数 | 2 の補数 |
| `bool()` | 1 | `true^` / `false^` | |
| `range(min, max)` | 範囲から計算 | min 〜 max の整数 | 幅を手で数えない |
| `fixed(min, max, step)` | 範囲と刻みから計算 | min 〜 max の実数 | 刻みに量子化する |
| `enum(E)` | メンバの数から計算 | E のメンバ | 宣言順の番号を詰める |
| `array(n, 種類)` | n × 種類 | 0 から n−1 の列 | 固定長 |

**幅を範囲で書く**のは Glenn Fiedler の直列化の手法に倣う。`range(-8, 7)` は 16 通り
なので 4 ビット、`range(0, 100)` は 101 通りなので 7 ビットになる。書き手は値の
範囲を知っていて、ビット数は知らなくてよい。`fixed(0, 1, 0.01)` は 0.00〜1.00 の
101 通りで 7 ビット。`enum(love.keyboard.Key)` は 181 メンバなので 8 ビット。

`array` は入力の履歴を送るのに使う。ロールバックでは、直近の数フレーム分の
入力をまとめて冗長に送り、1 つ欠けても次の 1 つで埋め合わせるのが定石である。

```lhat
let^ Frame = t^{ stick : number^, buttons : number^ }
let^ Packet = t^{ first : number^, history : t^{Frame[8]} }
let^ frame = b.format(Frame, {
    { "stick", b.range(0, 8) },     # 9 directions, 4 bits
    { "buttons", b.uint(6) },
})
let^ packet = b.format(Packet, {
    { "first", b.uint(32) },             # the frame number of history[0]
    { "history", b.array(8, frame) },    # a format is itself a kind
})
```

形式はそのまま種類として入れ子にできる。

`enum(E)` は E を作った機械のホスト根（[modules.host.memory]）に E を置く［補足］。種類は
GC の根ではないので、そうしないと復号で返すメンバが回収されうる。E 1 つにつき
1 つで、同じ E を何度渡しても増えない。

**Format は固定ビット長の形式だけを扱う。** 長さ付きのバイト列、文字列、可変長の
列など、値によって幅が変わる種類は持たない。これにより `bits()` と `size()` は
常に形式だけから決まり、符号化前に出力領域を正確に確保できる。可変長のデータが
必要なプロトコルは、固定長のヘッダと別のバイト列に分け、Format の外で境界を
管理する。境界は `decode` の読み始めの位置と、答える次の位置で渡していく（[binary.codec.encode]）。

```lhat
let^ header, start = try^ Header.decode(src, 0)
var^ at = start
for^ i from^ 1 to^ header.count {
    let^ entry, next = try^ Entry.decode(src, at)
    at := next
}
```

存在しない値を表す種類を後で設ける場合も、存在ビットに常に値の領域を続ける
固定幅とする。可変個数の列が必要なら、個数と最大個数分の領域を持つ固定幅の形式を
使う。いずれも Format の固定長という契約を変えない。

### 3.3 符号化と復号 [binary.codec.encode]

```lhat
let^ data = input.encode({
    aaa = 11111, bbb = 111,
    buttonA = true^, buttonB = true^, buttonC = false^,
})
data.size                  # 4

let^ got, next = try^ input.decode(data, 0)
got.aaa                    # 11111
next                       # 4
```

- `fmt.encode(t) -> string^` — `T` のテーブルから形式どおりに詰める
- `fmt.encodeInto(t, bytes)` — 同じものを `Bytes` に書き込む（[binary.containers.bytes]）
- `fmt.decode(s, at) -> (T, number^)|std.binary.Error` — `s` の `at` バイト目から
  読み、新しい `T` のテーブルと、次の形式が始まる位置 `at + fmt.size()` を返す。
  `s` は `string^|std.binary.Bytes`。返す位置をそのまま次の `decode` に渡せば、
  続くレコードを順に読める。サイズでなく位置を返すのは、固定長の形式では
  サイズが常に `fmt.size()` で新しい情報にならないからである
- `fmt.decodeInto(s, at, t) -> number^|std.binary.Error` — **既存のテーブルへ書き込む**。
  答えは `decode` と同じ次の位置。
  毎フレーム `decode` で新しいテーブルを作ると、そのたびにごみが出る。受け側が
  同じテーブルを使い回せるようにする。入れ子の形式と `array` も、そこにある
  テーブルへ書き込む。テーブルが既に持つキーの文字列をそのまま使うので、
  2 回目からは文字列も作らない［補足］
- `fmt.bits()`、`fmt.size()` — 総ビット数とバイト数。`bits()` は種類にもある。
  hostdata のメンバは関数なので、呼び出しで書く［補足］

`decode` が返すテーブルは `format` に渡した `T` を保つ。したがって
`let^ got, next = try^ input.decode(data, 0)` の `got` は上の `Input` であり、`got.aaa` は
検査を通り、`got.aaaa` のような綴り間違いは起動前に見つかる。

型値は単なる注釈ではない。`format` は種類の木から復号結果の構造型を一度作り、
それが `T` に適合しなければ panic にする。たとえば `uint` のフィールドを
`string^` と宣言することはできない。検査済みの型記述子は `Format` が所有し、
[modules.host.boundary-yields] の型引数として公開する。`encode` と `decode` のメンバ署名はそれぞれ
`self^.T0` を引数と答えに使う。

### 3.4 ビット順とバイト順［重要］ [binary.codec.bit-order]

並びは OS にも CPU にも依らず、常に同じにする。

- 1 バイトの中は**下位ビットから**詰める
- バイトをまたぐ値は**リトルエンディアン**（下位バイトが先）
- 最後のバイトの余りは 0 で埋める

[binary.codec.encode] の例は 16 + 8 + 1 + 1 + 1 = 27 ビットで、4 バイトになる。

```text
aaa = 11111 = 0x2B67   → bytes 0..1: 67 2B
bbb = 111   = 0x6F     → byte  2:    6F
buttonA = 1            → byte  3, bit 0
buttonB = 1            → byte  3, bit 1
buttonC = 0            → byte  3, bit 2
                       → byte  3:    03  (bits 3..7 are padding)

data = "\x67\x2B\x6F\x03"
```

`int` は 2 の補数の下位 n ビット、`range` / `fixed` / `enum` は「最小値からの
番号」を符号なしで詰める。

`fixed` の番号は**最も近い刻み**に丸める（B2）［補足］。両端から半刻みまでは
端の刻みに入り、それより外は範囲外である。

### 3.5 誤り [binary.codec.errors]

誤りは、誰が間違えたかで 2 つに分ける（04 の線引き）。

- **符号化に渡したテーブルの誤りは panic。** 範囲外の値、型の違い（`uint` に
  文字列）、欠けたフィールド、整数でない数（`number^` は内部で整数と実数を
  持ち、溢れると実数になる。[syntax.obj.number-widen]）。どれも書き手のプログラムの誤りである
- **復号に渡したバイト列の誤りはエラー値。** バイト列は外から来るので、壊れて
  いても書き手の誤りではない。`std.binary.Error` を宣言し、変種は何が起きたかで
  名付ける
  - `Truncated` — 形式の長さより短い
  - `Malformed` — 範囲外の番号（`range` の上限を超える、存在しない enum の番号、
    `uint(64)` で `number^` の整数に収まらない値［補足］）

形式より長い入力は誤りにしない（末尾の余りは無視する）。`at` から後ろが形式より
短ければ `Truncated` である。`at` が 0 以上の整数でないのは書き手の誤りで、panic に
なる［補足］。

メモリが尽きたときも panic である［補足］。`std.sys.Error.OutOfMemory` を答えの
合併に入れると、毎フレーム呼ぶ `encode` の答えをいちいち絞り込むことになる。
`dispose` した種類・形式・`Bytes` を使うのも書き手の誤りで、panic になる。

### 3.6 先例 [binary.codec.prior-art]

- **Erlang のビット構文。** `<<Aaa:16, Bbb:8, A:1, B:1, C:1>>` で組み立て、
  同じ形のパターンで分ける。順序付きの並びにビット幅を書く点、組み立てと分解が
  同じ記述である点で、この章に一番近い。バイナリを文字列と別の型として持つ点は
  違う（2 節）
- **Glenn Fiedler の「Serialization Strategies」「Reading and Writing Packets」**
  （C++ の実装は yojimbo の `serialize.h`）。幅を範囲で決める、読みと書きを
  1 つの記述で済ませる、float を量子化する、入力履歴を冗長に送る。ゲームの
  ネットコードの定番
- **Python の `bitstruct`**（`pack('u16u8b1b1b1', ...)`）と **`construct`**
  （`BitStruct` を宣言的に書き、読み書き両方に使う）
- **Kaitai Struct**（YAML のスキーマ、`b1`・`b16`）、**Rust の `deku`**
  （`#[deku(bits = 16)]`）
- **ASN.1 PER**（範囲からビット数を決める規格）
- **Lua 5.3 の `string.pack`**（バイト単位。文字列にバイト列を詰める先例）

## 4. std.net（UDP） [binary.net]

### 4.1 API [binary.net.api]

```lhat
import^std.net

let^sock = try^ std.net.udp()
try^ sock.bind("0.0.0.0", 7777)         # port 0 picks any free port
try^ sock.sendTo(data, "192.168.0.12", 7777)

# or name one peer and send to it
try^ sock.setPeer("192.168.0.12", 7777)
try^ sock.send(data)
```

| 呼び出し | 答え |
| --- | --- |
| `std.net.udp()` | `std.net.Udp\|std.net.Error` |
| `sock.bind(host, port)` | `nil^\|std.net.Error` |
| `sock.setPeer(host, port)` | `nil^\|std.net.Error` |
| `sock.send(data)` | `nil^\|std.net.Error` |
| `sock.sendTo(data, host, port)` | `nil^\|std.net.Error` |
| `sock.receive()` | `(string^\|nil^, string^, number^)\|std.net.Error` |
| `sock.receiveInto(bytes)` | `(std.binary.Bytes\|nil^, string^, number^)\|std.net.Error` |
| `sock.getLocal()` | `string^, number^` |
| `sock.setBroadcast(bool^)` | `nil^\|std.net.Error` |
| `sock.dispose()` | — |

`data` は `string^` か `std.binary.Bytes` で、どちらも1本ずつのオーバーロード
である（[binary.containers.bytes]）。`receiveInto` は届いたデータグラムを渡した `Bytes` に書き込み、
データの位置にその `Bytes` を返す。`Bytes` を取る3つは、std.binary が先に
登録されているときだけある（[binary.containers.outside]）。

誤りは答え全体と合併する。[syntax.type.tuple-parens] は**タプルの位置に誤り型を置かない**
ので、`receive` の誤りは 3 つの答えを覆う。受け側は `try^`（か `catch^`）で
誤りを除いてから分解する［補足］。

`Udp` は hostdata で、`dispose` でソケットを閉じる。回収時にも閉じる（[modules.hostdata] の
`dispose` の規則のまま）。

#### ソケットは最初の宛先で作る［補足］

`udp()` はまだ OS のソケットを作らない。`bind`・`sendTo`・`setPeer` の後の
`send` のどれかが、最初に渡したアドレスの族（IPv4 か IPv6）で作る。名前を
引くまでどちらの族か分からないからである。作る前の `receive` は何も届いて
いない答えを返し、`getLocal` は `""` と `0` を返す。

`setPeer` を呼ばずに `send` するのと、0〜65535 の外のポートは、書き手の誤り
なので panic である［補足］。

### 4.2 受信は待たない［重要］ [binary.net.no-wait]

**`receive` は常にすぐ返る。** 答えは 3 つ — データ、送り主のアドレス、
送り主のポート。届いているデータグラムが無ければ、データの位置が `nil^` で、
アドレスは `""`、ポートは `0` になる。ブロックする版は持たない。

`nil^` を取るのはデータの位置だけにしてある（[syntax.type.tuple-parens] の `B, C|nil^` の形）。
タプル全体を `nil^` と合併させる形（`(B, C, D)|nil^`）にすると、受け側は
分解する前に絞り込まなければならない。位置ごとにしておけば、誤りを除いた後は
いつも `let^ data, host, port = try^ sock.receive()` と 3 つに受けられる。

ゲームのループは毎フレーム、空になるまで読む。

```lhat
repeat^ {
    let^ data, host, port = try^ sock.receive()
    if^ data = nil^ { break^ }
    # data is a string^ here
}
```

LuaSocket で `settimeout(0)` にして使うのと同じ形である。ブロックしないので、
スレッドを別に立てる必要が無い。到着を待ちたい場合は、5 節の `std.async`
による受信待ちを後で足す。

何も届いていない答えの `""` は機械ごとに 1 つ作って使い回す［補足］。毎フレーム
空になるまで読むと、空の答えは毎フレーム返るからである。

### 4.3 アドレス [binary.net.address]

- IPv4 と IPv6 の両方を受ける。名前は `bind` / `setPeer` / `sendTo` の中で
  解決する（`getaddrinfo`、同期）。最初の答えを使う
- `setBroadcast(true^)` で、LAN 内の相手探しに使うブロードキャストを許す
- 1 回の `receive` で受ける最大の大きさは 65536 バイトで、変えられない
  （N2）［補足］。UDP が運べる最大のデータグラムが収まるので、届いたものが
  切り詰められることは無い。受け用の領域はソケットごとに最初の `receive` で作る

### 4.4 誤り [binary.net.errors]

ネットワークの失敗は外から来るのでエラー値にする。`std.net.Error` を宣言し、
変種は何が起きたかで名付ける。

- `AddressInUse` — そのポートは使われている
- `Resolve` — 名前を引けなかった
- `Unreachable` — 相手やネットワークに届かない
- `TooLarge` — データグラムが大きすぎる
- `Closed` — 閉じたソケットを使った
- `Failed` — 上のどれでもない OS の失敗（権限が無い、ソケットを作れない
  など）［補足］

Windows は、閉じたポートへ送ったデータグラムの ICMP を次の受信の「接続の
リセット」として返す。接続を持たないソケットには関係が無いので、その報告は
切ってある（`SIO_UDP_CONNRESET`）［補足］。送信バッファが満ちて送れなかった
ときは、UDP が落としてよいものとして誤りにしない［補足］。

### 4.5 実装の置き場 [binary.net.where]

lhat には DAP（[debug]）のためのソケット層 `port/socket.c`（`lhatsocket`）が
ある。TCP はループバック専用のまま、UDP をここに足した。

- `lhatsocket` は DAP と `std.net` のどちらかが要るときに組む
- UDP の関数（ノンブロッキング、`sendto` / `recvfrom`、`getaddrinfo`）は
  `port/socket.h` に並ぶ
- Winsock の初期化（`WSAStartup`）は Winsock 自身が数える。DAP はセッション
  ごと、`std.net` は `Udp` ごとに `WSAStartup` と `WSACleanup` を対にし、
  最後の `WSACleanup` だけが効く［補足］
- CMake は `option(LHAT_BUILD_STDLIB_NET)`（既定 ON）で任意にする。Windows は
  `ws2_32` をリンクする

`std.net` は `std.binary` に依らない。`Bytes` を使うホストは `std.binary` を
先に登録する（[binary.containers.outside]）。どちらの登録も 2 回呼んでよい［補足］。

### 4.6 機械をまたがない [binary.net.one-machine]

`Udp` は作った一つの機械だけが使う。共有契約（carry、[modules.hostdata]）は宣言せず、別の
機械へは渡さない。ソケットを task に持たせるなら、その task の中で `Udp` を
作る。送受信するデータは `string^` として Task の結果や `Channel<string^>` で
機械の間を渡す。継続する送受信には Channel を使い、その一つの所有者が送信と
受信を直列化する。

`Kind`・`Format`・`Bytes` も共有契約を宣言しない［補足］。`Bytes` を使う
`receiveInto`・`encodeInto` は、その Bytes を作った機械の中で使う。機械をまたぐ
ときは `receive`・`encode` の `string^` を使う。

## 5. 展望（この章では作らない） [binary.future]

- **TCP。** POSIX で切れた相手へ書いたときの SIGPIPE 対策（`MSG_NOSIGNAL` /
  `SO_NOSIGPIPE`）を含める
- **受信待ち。** `std.async` の外部完了（`lhatstdlib_async_external` /
  `lhatstdlib_async_complete`）で、到着したら `await^` が進む形
- **HTTPS。** OS の仕組みを使う。LÖVE の lua-https に当たる
- **信頼性の層。** 再送・順序保証・チャネルを持つ、ENet に当たるもの
- **LÖVE 側の糊。** LuaSocket に似せた `socket` を LÖVE が別に用意するか

### 5.1 ソケットの待ちと OS の多重化［提案］ [binary.future.socket-waits]

TCP を足す、または UDP の受信を待てるようにすると、ソケットを `std.async` の
待ちに入れることになる。そのとき OS の多重化の仕組みをどう使うかを、今の
インターフェースのまま 3 つの OS でそろえる計画である。

**今の状態［補足］。** ソケットは待ちの表に入らない（`receive` は待たない、
[binary.net.no-wait]）。`std.async` の待ちはタイマーと外部完了だけで、`wait(s)` は
表を見ながら最長 20ms ずつ眠る。別スレッドからの完了には、その仮眠の切れ目で
気付く。OS の待ちの仕組みとして使っているのは DAP 用の `select`（ソケット 1 本）
だけである。

**readiness 型でそろえる。** 「読める・書けるようになったら知らせる」型を使い、
知らされたら呼び手のバッファへノンブロッキングで読み書きする。読み込み先を
呼び出しのときに渡す今の形（`receive` と [binary.store.contracts] の `read(buffer)`）
がそのまま使え、インターフェースを変えずに済む。

| OS | 仕組み |
| --- | --- |
| Linux | `epoll` |
| macOS / BSD | `kqueue` |
| Windows | IOCP に readiness を届ける Winsock の `ProcessSocketNotifications`（Windows 10 Build 20348 以降。実質 Windows 11 / Server 2022 以降）。それより古い版を支えるなら `\Device\Afd` の poll（wepoll 方式。Rust の mio が使う、文書化されていない仕組み）か `WSAPoll` に戻る |

- ソケットは一度登録し、待つたびに全部を渡し直さない。1 回の待ちが待ちの数に
  比例しない
- 待ちは OS の仕組みで眠る。20ms ずつの仮眠をやめ、次のタイマーの期限を
  待ちの上限にする
- 別スレッドからの外部完了（`lhatstdlib_async_complete`）も OS の仕組みで起こす。
  IOCP は `PostQueuedCompletionStatus`、Linux は `eventfd`、kqueue は
  `EVFILT_USER` を使う。これで完了にすぐ気付く
- 上の「受信待ち」は、ホストが外部完了で済ませる形でなく、この登録で
  `std.async` 自身が到着を知る形になる

**完了型は採らない。** IOCP の overlapped I/O や Linux の io_uring のような
「I/O を出しておき、終わったら知らされる」型は、インターフェースを変えなければ
使えない。

- I/O が終わるまで OS がバッファへ書くので、バッファを呼び出しの間だけ貸す
  今の形では渡せない。ライブラリが持つバッファか、動かないバッファを受け渡す形になる
- 待ちを捨てるとき、出した I/O を取り消し（`CancelIoEx` など）、取り消しが
  終わるまでバッファを生かしておく必要がある
- 「今できなければすぐ返る」という `receive` の意味と合わない。出した I/O は
  進行中になる

完了型の利点は、1 回の操作のシステムコールが 1 つ少ないことである。多数の接続を
持つときはソケットごとのバッファが常に要らない readiness 型の方がメモリで有利で、
Microsoft もこの点を挙げて WSAPoll・select の代わりに勧めている。必要になったら、
所有バッファの別のインターフェースとして後から足す。

**R^ と同じ判断である［補足］。** R^（L^ を元にしたシステム言語）の
`std.async`・`std.net` は TCP を持ち、`poll`（Windows は `WSAPoll`）と起こし用の
ソケットで既に待ちを OS に任せている。同じ readiness 型でそろえる計画で、
L^ に入れるときはその作りが参考になる。R^ が L^ をホストする場合、L^ の
`std.async` は R^ の待ちの表で眠る。

## 6. 未決事項 [binary.open]

現在はない。

閉じたもの:

- **B1** `format(type, fields)` が `Format<T>` を作り、`decode` は `T` を返す。
  種類の木と型は `format` 時に一度照合する（[binary.codec.format] ・[binary.codec.encode]）
- **B2** `fixed` の丸めは最も近い刻み（[pipeline.infer]）［補足］
- **B3** Format は固定ビット長だけを扱う。可変長の種類は持たず、`bits()` と
  `size()` は常に形式だけから決まる（[binary.codec.kinds]）
- **N1** `Udp` は共有契約を持たず、作った一つの機械だけが所有する。機械間の
  送受信データは `string^` として Task または Channel で渡す（[binary.net.one-machine]）
- **N2** 受信の最大は 65536 バイト固定（[binary.net.address]）［補足］
- **N3** VM のみのビルド（`LHAT_WITH_FRONTEND=OFF`）にも両方が入る。どちらも
  構文解析器に依らない［補足］

## 7. スキーマ不要のテーブル保存とストリーム [binary.store]

ゲームの進行状態など、可変の構造を持つデータには、Format の固定ビット列とは別に
型タグ付きの形式を用いる。コンパイル済み LTON やバイトコードとは異なる**データ専用形式**。
読み書きとも字句解析器・構文解析器・型検査器に依存せず、VMOnly でも動く。

### 7.1 API [binary.store.api]

| 呼び出し | 動作 |
| --- | --- |
| `std.binary.encode(table)` | データ全体をバイナリ `string^` にする（f^） |
| `std.binary.decode(string_or_bytes)` | 1件を復元。末尾の余分なバイトは誤り（f^） |
| `std.binary.serialize(writer, table)` | Writer に1件を順次書く（p^） |
| `std.binary.deserialize(reader)` | Reader から1件だけ読む（p^） |
| `std.stream.writer()` | 空のメモリ Writer |
| `std.stream.reader(string_or_bytes)` | 入力をコピーして保持するメモリ Reader |
| `writer.toString()` | 蓄積した出力全体のコピー |
| `reader.dispose()` / `writer.dispose()` | メモリを解放 |

`encode` と `decode` は `std.binary.DataError|std.sys.Error.OutOfMemory` を返しうる。
`serialize` の成功は `nil^`、`deserialize` の成功は `t^{}`。
ストリーム経由の操作は、下層からの `error^` もそのまま返す。
型情報は保存しないので、復元後は `fits^` などで期待する形を検査する。

### 7.2 Reader／Writer の構造的な契約 [binary.store.contracts]

共通の Stream 基底型は作らない。ホスト型やテーブルが次のメソッドを持てばよい。

```lhat
t^{ read : p^self^, std.binary.Bytes -> number^|error^; }
t^{ write : p^self^, std.binary.Bytes -> nil^|error^; }
```

- `read(buffer)` は、**呼び出し時の buffer.size() を最大要求量**とする。
  読んだバイトを先頭から書き、buffer の長さもそのバイト数にして、同じ数を返す。
  正の要求に対する 0 は EOF。短い読み込みも許し、必要量の確保は呼び手が繰り返す。
- `write(buffer)` は、返るまでに全バイトを消費する。部分書き込みを呼び手へ返さない。
- どちらも渡されたバッファを処理後に保持しない。呼び手は直ちに再利用できる。
- `Bytes.resize(n)` は長さを変更し、拡張部を 0 で埋める（p^）。保持済み領域は再利用する。
- `seek`・`flush`・`finish`・所有権の移譲は、この最小契約には含めない。
  シリアライザーは借りたストリームを終了・破棄しない。

メモリ Reader の入力は呼び出し時にコピーするので、元の Bytes を変更・破棄しても影響しない。
破棄済みのメモリストリーム・バッファへのアクセスは `std.stream.Error.Disposed`。
圧縮・暗号化は将来、この契約を持つラッパーとして追加できる。
その正常終了はラッパー固有の `finish()` などで明示し、シリアライザーの責務にはしない。

### 7.3 保存対象と制限 [binary.store.limits]

素のテーブル、nil^、bool^、整数、有限の実数、任意のバイト文字列を扱う。
配列部と辞書部を区別し、配列内の nil^ も含めて長さを保持する。
辞書キーは整数・有限の実数・bool^・文字列。辞書の格納順は保証しない。
def^・そのインスタンス・エラー・enum・関数・コルーチン・ホスト値は対象外。
循環は `Cycle`、共有された子テーブルは複製として復元する。

`DataError` は `Truncated`、`Malformed`、`Unsupported`、`Cycle`、`Limit`、`IO`。
標準の上限は1件256 MiB、配列位置と辞書項目の合計100万、テーブルの深さ96。
上限は長さを信用して確保する前に検査する。C API は `LhatBinaryDataLimits` で変更できる
（深さの最大は96）。失敗時に部分的なテーブルは返さない。

保存中は対象とその子を変更しない。構造変更の一部は検出するが、整合した
スナップショットを自動取得する機能ではない。ゲーム側で保存時点を決める。
ストリームへの書き込みに失敗すると、不完全なレコードが残りうる。

### 7.4 バイナリ形式 v1 [binary.store.format]

先頭8バイトは `89 4C 48 54 01 0D 0A 1A`。01 がバージョン。
続けて「u32 little-endian の長さ、ペイロード」を繰り返す。
チャンク長は1〜16384、長さ0の u32 がレコード終端。1件の後ろへ次の1件を続けてよい。
フレームにより、Reader は次のレコードを先読みせず、まとめたバッファで処理できる。

ペイロードは次の値を再帰的に並べる。根は必ずテーブル。

| タグ | 内容 |
| --- | --- |
| 0 | nil^ |
| 1 / 2 | false^ / true^ |
| 3 | i64、little-endian、2の補数 |
| 4 | IEEE 754 binary64、little-endian、有限値のみ |
| 5 | バイト長（unsigned LEB128）、そのバイト列 |
| 6 | 配列長・辞書項目数（unsigned LEB128）、配列値、辞書のキー・値 |

整数と実数の区別、文字列内の NUL・不正UTF-8も保持する。
過長・オーバーフローの長さ、重複辞書キー、不正なタグ・フレームは拒否する。
これは圧縮形式でも暗号化形式でもなく、上層・下層でそれらと組み合わせるための形式。

### 7.5 C API と検証 [binary.store.c-api]

`stdlib/binary.h` の `lhatstdlib_binary_serialize` / `deserialize` は、
Machine と C の read/write コールバックで直接使える。Program やモジュール登録は不要。
読み込み途中のテーブル・キーはコールバック中の GC をまたいで保持する。
返された値は、次に VM を動かす前にホスト側で root に置く。

`tests/test_binary_data.c` が形式、短い読み、GC、連続レコード、構造的なラッパー、
エラー伝播、途中切れ、不正値、上限を検証する。
`tests/binary_vm_only` はフロントエンドをビルド対象から除いて同じ C API を実行する。

## 改定履歴（要約）

- **案として起こした。** 対戦ゲームのロールバックの入力を詰める `std.binary` と、
  LAN・検証用の UDP を持つ `std.net` を、LÖVE 専用でなく標準ライブラリとして
  設計した。入れ物は `string^`、形式は順序付きの列、受信は待たない
- **v1 を実装した。** 使い回す入れ物 `std.binary.Bytes` を足した（[binary.containers.bytes]）。
  `receive` の答えは、[syntax.type.tuple-parens] に合わせて誤りがタプル全体を覆う形にした（[binary.net.api]）。
  B2・N2・N3 を閉じた
- **std.net を std.binary から切り離した（2026-10-10）。** `send`・`sendTo` の
  `string^|std.binary.Bytes` を2本のオーバーロードに分け、`Bytes` 版は
  std.binary が登録されているときだけ足す。std.binary の外から `Bytes` に触る口
  （[binary.containers.outside]）を設け、拡張の関数表に `lhat_lookup_host_context` を足した
- **B1 を閉じた（2026-10-11）。** `format(type, fields)` が型記述子を所有する
  `Format<T>` を作り、`decode` の答えと `encode` の引数を `T` にした。種類の木が
  復号する構造と `T` は形式の作成時に照合する
- **B3 を閉じた（2026-10-11）。** Format は固定ビット長だけを扱う。可変長の
  バイト列・文字列・列は種類に加えず、必要なら固定長ヘッダの外で境界を管理する
- **N1 を閉じた（2026-10-11）。** `Udp` は共有せず、作った一つの機械が所有する。
  機械間では送受信データだけを `string^` として Task または Channel で渡す
- **復号に読み始めの位置を足した（2026-10-15）。** `decode`・`decodeInto` が
  開始位置を取り、次の形式が始まる位置を返す。固定長ヘッダの後ろに続く可変個の
  レコードを、Format の固定長の契約を変えずに読める。バイト単位で切り出す口は、
  コピーを作るので大きなバイナリでは避けられがちになるとして置かなかった
