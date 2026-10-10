# std.binary と std.net — ビット単位の直列化と UDP

**状態: v1 実装済み**（`stdlib/binary.c`・`stdlib/net.c`・`port/socket.c`）。
残る未決事項は末尾（B1・B3・N1）に並べる。

`std.binary` はテーブルをビット単位で詰めてバイト列にし、戻す。`std.net` は UDP の
ソケットを開き、そのバイト列を送受信する。どちらも標準ライブラリで、特定のホスト
（LÔVE など）に依らない。

## 1. 動機

対戦ゲームのロールバック型ネットコードは、毎フレームの入力を相手へ送る。
1 フレームの入力はボタンが数個とスティックの向きくらいで、詰めれば数バイトに
収まる。テキストやテーブルの汎用の書き出し（LTON、8 章）で送ると何倍にも
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

## 2. 入れ物は `string^` と `std.binary.Bytes`

送受信するバイト列は `string^` で運ぶ。

L^ の文字列はバイトの列であって、文字の列ではない（01 の 5 章）。作るときに
UTF-8 として検査しないので、任意のバイトも NUL もそのまま持てる。バイト数は
`.size`、文字数は `.length`（02 の 14.18）。**直列化したデータの大きさは
`.size` で測る。** `.length` は先頭バイトの数を数えるだけなので、バイト列に
対しては意味を持たない。

Lua 5.3 の `string.pack` が Lua の文字列にバイト列を詰めるのと同じ立場である。
文字列のまま運べるので、`std.channel` にも LTON にもそのまま流せる。

**使い回す入れ物として `std.binary.Bytes` も持つ。** 文字列は作るたびに新しい
オブジェクトになるので、毎フレーム符号化して送るとそのたびにごみが出る。
`Bytes` は中身を書き換えられる hostdata で、一度作って同じものに書き込み続ける。

言語にはビット演算子が無い（01 の 7.5）。ビットを触るのは `std.binary` が C の
中で行い、L^ 側はテーブルと文字列（と `Bytes`）だけを扱う。

### 2.1 Bytes［補足］

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
`string^` 版と `Bytes` 版の2本のオーバーロード（02 の 14.12）である。
書き込む側は `fmt.encodeInto` と `sock.receiveInto` で、どちらも中身を
置き換える。中身を 1 バイトずつ読み書きする口は持たない — 読み書きは
形式（3 章）を通す。

### 2.2 std.binary の外から Bytes に触る［補足］

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

## 3. std.binary

### 3.1 形式（Format）

まず**形式**を作る。形式はフィールドの並びで、各行は `{ 名前, 種類 }` である。

```lhat
import^ std.binary

let^ b = std.binary
let^ input = b.format({
    { "aaa", b.uint(16) },
    { "bbb", b.uint(8) },
    { "buttonA", b.bool() },
    { "buttonB", b.bool() },
    { "buttonC", b.bool() },
})
```

**並びは列で書く。** テーブルの名前付きのキーは書いた順を保たない（02 の
16.3。ハッシュ部の順序は約束されない）ので、`{ aaa = 16, bbb = 8 }` の形では
ビットの並び順を決められない。

`std.binary.format` は列を一度だけ読んで、**形式オブジェクト**
（`std.binary.Format`、8.8 の hostdata）にする。次をここで検査する。

- 名前が文字列で、重複していない
- 各種類の幅が 1〜64 ビットに収まる
- 総ビット数（上限 2³² ビット［補足］）

誤りは panic である。形式はプログラムの書き手が書くものなので、間違いは
プログラマの誤りになる（04 の線引き）。

形式は可変長のデータを持つので、ホスト値（8.9、248 バイト上限・スタック限定）
にはできず、hostdata になる。

#### 種類も hostdata である［補足］

`b.uint(16)` などが返す種類は `std.binary.Kind`（hostdata）で、`Format` は
その派生型（8.8改）である。だから形式をそのまま種類として書ける（3.2）。
`format` と `array` は渡された種類を**写して**持つので、元の種類や形式を
`dispose` しても、それを含む形式は壊れない。

### 3.2 種類

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
let^ frame = b.format({
    { "stick", b.range(0, 8) },     # 9 directions, 4 bits
    { "buttons", b.uint(6) },
})
let^ packet = b.format({
    { "first", b.uint(32) },             # the frame number of history[0]
    { "history", b.array(8, frame) },    # a format is itself a kind
})
```

形式はそのまま種類として入れ子にできる。

`enum(E)` は E を作った機械のホスト根（05 の 8.12）に E を置く［補足］。種類は
GC の根ではないので、そうしないと復号で返すメンバが回収されうる。E 1 つにつき
1 つで、同じ E を何度渡しても増えない。

可変長の種類（長さ付きのバイト列 `bytes()`、文字列 `text()`、存在ビット付きの
`optional(種類)`）は、総ビット数が固定でなくなる。入れるかどうかは B3 で決める。

### 3.3 符号化と復号

```lhat
let^ data = input.encode({
    aaa = 11111, bbb = 111,
    buttonA = true^, buttonB = true^, buttonC = false^,
})
data.size                  # 4

let^ got = try^ input.decode(data)
got["aaa"]                 # 11111
```

- `fmt.encode(t) -> string^` — テーブルから形式どおりに詰める
- `fmt.encodeInto(t, bytes)` — 同じものを `Bytes` に書き込む（2.1）
- `fmt.decode(s) -> t^{}|std.binary.Error` — 新しいテーブルに戻す。`s` は
  `string^|std.binary.Bytes`
- `fmt.decodeInto(s, t) -> nil^|std.binary.Error` — **既存のテーブルへ書き込む**。
  毎フレーム `decode` で新しいテーブルを作ると、そのたびにごみが出る。受け側が
  同じテーブルを使い回せるようにする。入れ子の形式と `array` も、そこにある
  テーブルへ書き込む。テーブルが既に持つキーの文字列をそのまま使うので、
  2 回目からは文字列も作らない［補足］
- `fmt.bits()`、`fmt.size()` — 総ビット数とバイト数。`bits()` は種類にもある。
  hostdata のメンバは関数なので、呼び出しで書く［補足］

`decode` が返すテーブルは `t^{}` で、形を約束しない。名前で読む `got.aaa` は
検査を通らず、添字の `got["aaa"]` で読む（`std.json.decode` と同じ）。形式の
並びから型を作るかは B1 で決める。

### 3.4 ビット順とバイト順［重要］

並びは OS にも CPU にも依らず、常に同じにする。

- 1 バイトの中は**下位ビットから**詰める
- バイトをまたぐ値は**リトルエンディアン**（下位バイトが先）
- 最後のバイトの余りは 0 で埋める

3.3 の例は 16 + 8 + 1 + 1 + 1 = 27 ビットで、4 バイトになる。

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

### 3.5 誤り

誤りは、誰が間違えたかで 2 つに分ける（04 の線引き）。

- **符号化に渡したテーブルの誤りは panic。** 範囲外の値、型の違い（`uint` に
  文字列）、欠けたフィールド、整数でない数（`number^` は内部で整数と実数を
  持ち、溢れると実数になる。14.8改）。どれも書き手のプログラムの誤りである
- **復号に渡したバイト列の誤りはエラー値。** バイト列は外から来るので、壊れて
  いても書き手の誤りではない。`std.binary.Error` を宣言し、変種は何が起きたかで
  名付ける
  - `Truncated` — 形式の長さより短い
  - `Malformed` — 範囲外の番号（`range` の上限を超える、存在しない enum の番号、
    `uint(64)` で `number^` の整数に収まらない値［補足］）

形式より長い入力は誤りにしない（末尾の余りは無視する）。

メモリが尽きたときも panic である［補足］。`std.error.OutOfMemory` を答えの
合併に入れると、毎フレーム呼ぶ `encode` の答えをいちいち絞り込むことになる。
`dispose` した種類・形式・`Bytes` を使うのも書き手の誤りで、panic になる。

### 3.6 先例

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

## 4. std.net（UDP）

### 4.1 API

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
である（2.1）。`receiveInto` は届いたデータグラムを渡した `Bytes` に書き込み、
データの位置にその `Bytes` を返す。`Bytes` を取る3つは、std.binary が先に
登録されているときだけある（2.2）。

誤りは答え全体と合併する。02 の 13.8改2 は**タプルの位置に誤り型を置かない**
ので、`receive` の誤りは 3 つの答えを覆う。受け側は `try^`（か `catch^`）で
誤りを除いてから分解する［補足］。

`Udp` は hostdata で、`dispose` でソケットを閉じる。回収時にも閉じる（8.8 の
`dispose` の規則のまま）。

#### ソケットは最初の宛先で作る［補足］

`udp()` はまだ OS のソケットを作らない。`bind`・`sendTo`・`setPeer` の後の
`send` のどれかが、最初に渡したアドレスの族（IPv4 か IPv6）で作る。名前を
引くまでどちらの族か分からないからである。作る前の `receive` は何も届いて
いない答えを返し、`getLocal` は `""` と `0` を返す。

`setPeer` を呼ばずに `send` するのと、0〜65535 の外のポートは、書き手の誤り
なので panic である［補足］。

### 4.2 受信は待たない［重要］

**`receive` は常にすぐ返る。** 答えは 3 つ — データ、送り主のアドレス、
送り主のポート。届いているデータグラムが無ければ、データの位置が `nil^` で、
アドレスは `""`、ポートは `0` になる。ブロックする版は持たない。

`nil^` を取るのはデータの位置だけにしてある（13.8改2 の `B, C|nil^` の形）。
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

### 4.3 アドレス

- IPv4 と IPv6 の両方を受ける。名前は `bind` / `setPeer` / `sendTo` の中で
  解決する（`getaddrinfo`、同期）。最初の答えを使う
- `setBroadcast(true^)` で、LAN 内の相手探しに使うブロードキャストを許す
- 1 回の `receive` で受ける最大の大きさは 65536 バイトで、変えられない
  （N2）［補足］。UDP が運べる最大のデータグラムが収まるので、届いたものが
  切り詰められることは無い。受け用の領域はソケットごとに最初の `receive` で作る

### 4.4 誤り

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

### 4.5 実装の置き場

lhat には DAP（9 章）のためのソケット層 `port/socket.c`（`lhatsocket`）が
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
先に登録する（2.2）。どちらの登録も 2 回呼んでよい［補足］。

### 4.6 機械をまたがない

`Udp` は作った機械が使う。共有契約（carry、8.8）は宣言しない。別の機械へ
渡すか（N1）は後で決める。`Kind`・`Format`・`Bytes` も宣言しない［補足］。

## 5. 展望（この章では作らない）

- **TCP。** POSIX で切れた相手へ書いたときの SIGPIPE 対策（`MSG_NOSIGNAL` /
  `SO_NOSIGPIPE`）を含める
- **受信待ち。** `std.async` の外部完了（`lhatstdlib_async_external` /
  `lhatstdlib_async_complete`）で、到着したら `await^` が進む形
- **HTTPS。** OS の仕組みを使う。LÖVE の lua-https に当たる
- **信頼性の層。** 再送・順序保証・チャネルを持つ、ENet に当たるもの
- **LÖVE 側の糊。** LuaSocket に似せた `socket` を LÖVE が別に用意するか

## 6. 未決事項

- **B1** `decode` が返すテーブルの型を、形式の並びから静的に作れるか。作れる
  なら `got.aaa` が型検査を通り、`got.aaaa` の綴り間違いが起動前に見つかる
- **B3** 可変長の種類（`bytes()`、`text()`、`optional(種類)`）を入れるか。
  入れると `fmt.bits()` / `fmt.size()` が固定でなくなる
- **N1** `Udp` を機械をまたいで渡せるようにするか（共有契約を宣言するか）

閉じたもの:

- **B2** `fixed` の丸めは最も近い刻み（3.4）［補足］
- **N2** 受信の最大は 65536 バイト固定（4.3）［補足］
- **N3** VM のみのビルド（`LHAT_WITH_FRONTEND=OFF`）にも両方が入る。どちらも
  構文解析器に依らない［補足］

## 改定履歴（要約）

- **案として起こした。** 対戦ゲームのロールバックの入力を詰める `std.binary` と、
  LAN・検証用の UDP を持つ `std.net` を、LÖVE 専用でなく標準ライブラリとして
  設計した。入れ物は `string^`、形式は順序付きの列、受信は待たない
- **v1 を実装した。** 使い回す入れ物 `std.binary.Bytes` を足した（2.1）。
  `receive` の答えは、13.8改2 に合わせて誤りがタプル全体を覆う形にした（4.1）。
  B2・N2・N3 を閉じた
- **std.net を std.binary から切り離した（2026-10-10）。** `send`・`sendTo` の
  `string^|std.binary.Bytes` を2本のオーバーロードに分け、`Bytes` 版は
  std.binary が登録されているときだけ足す。std.binary の外から `Bytes` に触る口
  （2.2）を設け、拡張の関数表に `lhat_lookup_host_context` を足した
