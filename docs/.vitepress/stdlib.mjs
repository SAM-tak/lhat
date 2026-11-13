// Shared catalog for reference navigation and API documentation generation.
const entry = (name, source, ja, en, notes = [], api = {}) => ({ name: `std.${name}`, source, ja, en, notes, api })
const pair = (ja, en) => ({ ja, en })

export const standardLibraries = [
  entry('math', 'math', '数学関数と定数', 'Mathematical functions and constants', [
    pair('三角関数の角度はラジアンです。`atan2` は `(y, x)`、底を指定する `log` は `(値, 底)` の順です。', 'Trigonometric angles are radians. `atan2` takes `(y, x)`; two-argument `log` takes `(value, base)`.'),
    pair('`lerp(a, b, t)` は `a + (b - a) * t`。`t` を0〜1に制限しません。', '`lerp(a, b, t)` computes `a + (b - a) * t` without clamping `t` to 0–1.')
  ], {
    sin: pair('正弦。', 'Sine.'), cos: pair('余弦。', 'Cosine.'), tan: pair('正接。', 'Tangent.'),
    asin: pair('逆正弦。', 'Inverse sine.'), acos: pair('逆余弦。', 'Inverse cosine.'), atan: pair('逆正接。', 'Inverse tangent.'),
    atan2: pair('座標 `(y, x)` の偏角。', 'Angle of coordinates `(y, x)`.'), deg: pair('ラジアンを度へ変換。', 'Convert radians to degrees.'), rad: pair('度をラジアンへ変換。', 'Convert degrees to radians.'),
    sqrt: pair('平方根。', 'Square root.'), cbrt: pair('立方根。', 'Cube root.'), exp: pair('自然指数関数。', 'Natural exponential.'),
    log: pair('自然対数、または第2引数で指定した底の対数。', 'Natural logarithm, or logarithm in the base supplied as the second argument.'), log2: pair('底2の対数。', 'Base-2 logarithm.'), log10: pair('底10の対数。', 'Base-10 logarithm.'),
    hypot: pair('2成分のユークリッド距離。', 'Euclidean length of two components.'), fmod: pair('実数の剰余。', 'Floating-point remainder.'),
    min: pair('1個以上の数値の最小値。', 'Minimum of one or more numbers.'), max: pair('1個以上の数値の最大値。', 'Maximum of one or more numbers.'), lerp: pair('2つの値の線形補間。', 'Linear interpolation of two values.')
  }),
  entry('math.complex', 'mathcomplex', '複素数', 'Complex numbers', [], {
    one: pair('実部1、虚部0の値。', 'The value with real part 1 and imaginary part 0.')
  }),
  entry('math.vector2', 'mathvector2', '2次元ベクトル', '2D vectors'),
  entry('math.vector3', 'mathvector3', '3次元ベクトル', '3D vectors'),
  entry('math.vector4', 'mathvector4', '4次元ベクトル', '4D vectors'),
  entry('math.quaternion', 'mathquaternion', 'クォータニオン', 'Quaternions'),
  entry('io', 'io', '標準入出力とファイル', 'Standard I/O and files', [
    pair('`open(path, mode)` のモードは C のファイルモード（`"r"`・`"w"`・`"rb"` など）です。ファイルは `with^` または `dispose()` で閉じます。', '`open(path, mode)` uses C file modes such as `"r"`, `"w"`, and `"rb"`. Close files with `with^` or `dispose()`.')
  ], {
    print: pair('値を標準出力へ表示。', 'Print values to standard output.'), readLine: pair('標準入力から1行を読む。入力終端は `IOError.Eof`。', 'Read a line from standard input; end of input returns `IOError.Eof`.'), open: pair('パスとモードを指定してファイルを開く。', 'Open a file with a path and mode.'),
    'File.readLine': pair('ファイルから1行を読む。入力終端は `IOError.Eof`。', 'Read a file line; end of input returns `IOError.Eof`.'), 'File.readAll': pair('ファイルの残りを文字列として読む。', 'Read the remainder of the file as a string.'), 'File.write': pair('文字列を書き込む。', 'Write a string.')
  }),
  entry('json', 'json', 'JSON の保存と復元', 'JSON encoding and decoding', [
    pair('空でない配列部分だけのテーブルは JSON 配列、それ以外はオブジェクトです。空テーブルは `{}` になります。', 'A nonempty table containing only an array part becomes a JSON array; other tables become objects. An empty table becomes `{}`.'),
    pair('循環、対応しない値、非有限数などはエラーです。`pretty` の字下げ幅は0〜16の整数です。', 'Cycles, unsupported values, and nonfinite numbers produce errors. `pretty` accepts an indentation width from 0 to 16.')
  ], {
    encode: pair('テーブルを JSON 文字列にする。', 'Encode a table as JSON.'), pretty: pair('テーブルと字下げ幅から整形済み JSON を作る。', 'Encode a table as formatted JSON with the given indentation width.'), decode: pair('JSON 文字列をテーブルへ復元。', 'Decode JSON into a table.')
  }),
  entry('lton', 'lton', 'L^ の記法を使う設定データ', 'Configuration data using L^ notation', [
    pair('LTON はテーブルリテラルの内側を書く形式です。先頭のインポートも使えます。読み込みでは手続きの呼び出しを許しません。', 'LTON contains the inside of a table literal, optionally preceded by imports. Reading it does not permit procedure calls.'),
    pair('書き出しは通常のデータテーブル、有限数、文字列、真偽値、公開された列挙体に対応します。循環はエラーで、共有する子は展開して書きます。', 'Writing supports plain data tables, finite numbers, strings, booleans, and public enums. Cycles are errors; shared children are expanded.')
  ], {
    parse: pair('文字列からテーブルを読む。', 'Read a table from text.'), load: pair('パスを指定してテーブルを読む。', 'Read a table from a path.'), stringify: pair('テーブルを LTON 文字列にする。', 'Write a table as LTON text.'), save: pair('パスとテーブルを指定してファイルへ保存。', 'Save a table to the supplied path.')
  }),
  entry('regex', 'regex', '正規表現', 'Regular expressions', [
    pair('繰り返し使うパターンは `new(pattern)` でコンパイルします。モジュールの `match`・`gmatch`・`gsub` は `(パターン, 対象文字列, …)` の順です。', 'Compile reused patterns with `new(pattern)`. Module-level `match`, `gmatch`, and `gsub` take `(pattern, subject, …)`.'),
    pair('捕捉の0番はマッチ全体、1番以降はグループです。`gsub` の置換文字列では `$1` などを使えます。探索予算の超過は `Error.Exhausted`、`gmatch` では走査終了になります。', 'Capture 0 is the whole match; higher numbers are groups. Replacement strings support `$1` and similar references. Exhausting the search budget returns `Error.Exhausted`, or ends a `gmatch` walk.')
  ], {
    new: pair('パターンをコンパイルする。', 'Compile a pattern.'), match: pair('最初のマッチ、または不一致の `nil^` を返す。', 'Return the first match, or `nil^` if none.'), gmatch: pair('マッチの序数と文字列を返す走査を作る。', 'Create an iterator yielding match ordinals and strings.'), gsub: pair('マッチを文字列または関数で置換する。', 'Replace matches using a string or function.'),
    'Regex.match': pair('コンパイル済みパターンで最初のマッチを探す。', 'Find the first match of a compiled pattern.'), 'Regex.captures': pair('マッチ全体と捕捉グループを返す。不一致は `nil^`。', 'Return the whole match and captured groups, or `nil^` if none.'), 'Regex.gmatch': pair('コンパイル済みパターンの全マッチを走査する。', 'Iterate matches of a compiled pattern.'), 'Regex.gsub': pair('コンパイル済みパターンによる置換。関数にはマッチ、序数、捕捉を渡す。', 'Replace compiled-pattern matches; callbacks receive the match, ordinal, and captures.'), 'Regex.split': pair('マッチ間の文字列を配列にする。空の要素も保持。', 'Collect text between matches into an array, retaining empty pieces.')
  }),
  entry('random', 'random', '乱数と再現可能な状態', 'Random numbers and reproducible state', [
    pair('`new(seed)` は0を含む同じシードから同じ系列を作ります。引数なしは OS からシードを得ます。暗号用途の乱数ではありません。', '`new(seed)` produces the same sequence for the same seed, including zero. With no argument, seeding uses the OS. This is not a cryptographic generator.'),
    pair('保存状態は整数のまま保持してください。実数を経由するとビットを失うことがあります。状態の互換性は乱数アルゴリズムの版に依存します。', 'Preserve saved states as integers; floating-point conversion can lose bits. State compatibility depends on the generator algorithm version.')
  ], {
    new: pair('OS または指定したシードで生成器を作る。', 'Create a generator from the OS or a supplied seed.'), 'Random.reseed': pair('シードから系列を再開始。', 'Restart the sequence from a seed.'), 'Random.save': pair('生成を進めず現在の整数状態を取得。', 'Read the integer state without advancing.'), 'Random.load': pair('保存した整数状態を復元。', 'Restore a saved integer state.'), 'Random.next': pair('`[0, 1)` の一様実数乱数を取得し状態を進める。', 'Draw a uniform real in `[0, 1)` and advance the state.'), 'Random.rangeint': pair('`(下限, 上限)` の両端を含む整数乱数。', 'Integer draw including both lower and upper bounds.'), 'Random.uniform': pair('`[下限, 上限)` の一様実数乱数。', 'Uniform real draw in `[lower, upper)`.'), 'Random.normal': pair('`(平均, 標準偏差)` の正規乱数。', 'Normal draw with `(mean, standard deviation)`.')
  }),
  entry('task', 'task', 'ワーカープールでの並行タスク', 'Concurrent tasks on a worker pool', [
    pair('`start(n)` はワーカー数を指定し、引数なしならコア数を使います。`async` は未開始コルーチンか引数なしの閉包を受け取ります。', '`start(n)` selects the worker count; omission uses the core count. `async` accepts an unstarted coroutine or a no-argument closure.'),
    pair('捕捉と引数はスナップショットとして渡します。結果を `await` で受け取れるのは一度だけで、2度目以降は `TaskError.Taken` です。', 'Captures and arguments cross as snapshots. A result can be collected by `await` exactly once; subsequent attempts return `TaskError.Taken`.'),
    pair('`await` は呼び出した機械を待たせます。ジョブの中で別のタスクを待つときは `await^ t.join()` を使うと、待つ間ワーカーを他のジョブに譲ります。自分のイベントループを持つ場合は `done()` や `awaitable()` を使います。`stop()` は未完了の仕事を破棄してワーカーを停止します。', '`await` blocks the calling machine. Inside a job, wait for another task with `await^ t.join()`, which gives the worker to other jobs while it waits. Use `done()` or `awaitable()` with your own event loop. `stop()` drops unfinished jobs and stops workers.'),
    pair('使用例は[並行処理](../../guide/tasks.md)を参照してください。', 'See [Concurrency](../../guide/tasks.md) for examples.')
  ], {
    start: pair('ワーカープールを開始し、ワーカー数を返す。', 'Start the pool and return its worker count.'), stop: pair('プールを停止する。', 'Stop the pool.'), workers: pair('ワーカー数を取得。', 'Get the worker count.'), async: pair('仕事を投入して結果型を保持する `Task<T>` を返す。', 'Submit work and return `Task<T>` preserving its result type.'), await: pair('タスクの最終結果を一度だけ取得。', 'Collect a task result exactly once.'), 'Task.done': pair('タスクが完了したかを確認。結果は消費しない。', 'Check completion without consuming the result.'), 'Task.failed': pair('失敗の説明文字列、または `nil^`。', 'Failure description, or `nil^`.'), 'Task.awaitable': pair('完了を待つための `std.async` の ID を取得。', 'Get a `std.async` ID for completion.'), 'Task.join': pair('完了を待つコルーチンを作る。`await^` で `await` と同じ結果を受け取る。', 'Create a coroutine awaiting completion; `await^` it for what `await` answers.')
  }),
  entry('thread', 'thread', '個別の OS スレッド', 'Individual OS threads', [
    pair('`spawn(手続き, 引数…)` は新しいスレッドで手続きを実行します。引数と捕捉はコピー可能な形で受け渡します。', '`spawn(procedure, arguments…)` runs a procedure on a new thread, transporting arguments and captured values.'),
    pair('`sleep` の単位は秒です。`join()` は完了を待ち、未取得のハンドルの `dispose()` もスレッドの完了を待ちます。', '`sleep` uses seconds. `join()` waits for completion; disposing an unjoined handle also waits for the thread.')
  ], {
    spawn: pair('手続きと引数から新しいスレッドを開始。', 'Start a new thread with a procedure and arguments.'), sleep: pair('現在のスレッドを指定秒数休止。', 'Sleep the calling thread for the supplied seconds.'), 'ThreadHandle.join': pair('完了まで待って戻り値を取得。再取得は `AlreadyJoined`。', 'Wait and collect the result; another collection returns `AlreadyJoined`.'), 'ThreadHandle.done': pair('スレッドの終了を確認。', 'Test completion.'), 'ThreadHandle.awaitable': pair('完了を待つ `std.async` の ID。', 'A `std.async` completion ID.'), 'ThreadHandle.failed': pair('失敗の説明文字列、または `nil^`。', 'Failure description, or `nil^`.')
  }),
  entry('channel', 'channel', '機械間の値のキュー', 'Value queues between machines', [
    pair('`new(T)` は `Channel<T>`、`named(name, T)` は名前付きの共有チャネルです。型引数を省略した形も使えます。', '`new(T)` creates `Channel<T>`; `named(name, T)` accesses a shared named channel. Untyped forms are also available.'),
    pair('`demand`・`supply` の時間指定は秒です。`atomic` の内側ではこれらは待機せず、`pop`・`push` 相当として動きます。', '`demand` and `supply` timeouts use seconds. Inside `atomic`, they do not wait and act like `pop` and `push`.'),
    pair('受け渡しできない値や、既存の名前付きチャネルとの型の不一致は `ChannelError.Refused` です。', 'Untransportable values or mismatched types for an existing named channel return `ChannelError.Refused`.')
  ], {
    new: pair('独立したチャネルを作る。', 'Create an independent channel.'), named: pair('同じ名前で共有するチャネルを取得。', 'Access a channel shared by name.'), 'Channel.push': pair('値を積み、投入番号を返す。', 'Enqueue a value and return its sequence number.'), 'Channel.supply': pair('値を積み、受け取られるまで待つ。第2引数は待機秒数。', 'Enqueue and wait for consumption; an optional second argument is the timeout in seconds.'), 'Channel.pop': pair('先頭を取得。不在なら `nil^`。', 'Take the first value, or `nil^` if empty.'), 'Channel.demand': pair('値が届くまで待って取得。時間切れなどは `nil^`。', 'Wait for a value; timeout and unavailable results produce `nil^`.'), 'Channel.awaitable': pair('読める状態を待つ `std.async` の ID。', 'A `std.async` ID for readability.'), 'Channel.take': pair('受信を待つコルーチンを作る。', 'Create a coroutine awaiting a value.'), 'Channel.peek': pair('先頭を取り出さずに読む。', 'Read the first value without removing it.'), 'Channel.count': pair('待機中の要素数。', 'Number of queued values.'), 'Channel.hasRead': pair('投入番号の値が受け取られたかを確認。', 'Test whether a submitted sequence number has been consumed.'), 'Channel.clear': pair('キューを空にする。', 'Empty the queue.'), 'Channel.atomic': pair('チャネルをロックして手続きへ渡す。', 'Hold the channel lock while calling a procedure with it.')
  }),
  entry('async', 'async', '期限と外部完了の待機', 'Timers and external completion waits', [
    pair('ID は機械ごとに管理されます。期限と待機上限の単位は秒です。`wait(0)` はスリープせず準備済みの完了を取得します。', 'IDs are managed per machine. Delays and wait limits use seconds. `wait(0)` polls ready completions without sleeping.'),
    pair('`external()` の完了はホスト側から通知します。タスクの順番を決めるスケジューラ自体はこのモジュールに含めません。', 'The host signals completion for `external()`. This module does not itself schedule tasks.')
  ], {
    timer: pair('指定秒数後に準備できる待機 ID を作る。', 'Create a wait ID ready after a delay in seconds.'), external: pair('ホストから完了通知を受ける待機 ID を作る。', 'Create an externally completed wait ID.'), drop: pair('待機 ID を破棄。', 'Drop a wait ID.'), wait: pair('上限秒数まで待ち、準備済み ID または `nil^` を返す。', 'Wait up to the supplied seconds, returning a ready ID or `nil^`.'), next: pair('次の期限までの秒数、または `nil^`。', 'Seconds until the next deadline, or `nil^`.'), pending: pair('残っている待機の数。', 'Number of outstanding waits.')
  }),
  entry('binary', 'binary', 'バイナリ保存と固定ビット形式', 'Binary data and fixed-bit formats', [
    pair('モジュールの `encode/decode` はスキーマ不要のデータ保存、`Format.encode/decode` は型とビット幅を決める固定形式です。', 'Module-level `encode/decode` store data without a schema; `Format.encode/decode` use a typed, fixed-bit layout.'),
    pair('`format(型, 設定)` の型引数は結果の `Format<T>` に保持します。`decode` は値と次の位置のタプルを返します。', '`format(type, layout)` preserves the represented type as `Format<T>`. Format decoding returns a tuple of value and next position.'),
    pair('使用例は[バイナリ保存とストリーム](../../guide/binary-data.md)を参照してください。', 'See [Binary data and streams](../../guide/binary-data.md) for examples.')
  ], {
    uint: pair('指定ビット幅の符号なし整数の種類。', 'Unsigned integer kind with the supplied bit width.'), int: pair('指定ビット幅の符号付き整数の種類。', 'Signed integer kind with the supplied bit width.'), bool: pair('真偽値の種類。', 'Boolean kind.'), range: pair('下限・上限を指定した整数の種類。', 'Integer kind bounded by lower and upper values.'), fixed: pair('下限・上限・刻みを指定した固定小数の種類。', 'Quantized kind with lower bound, upper bound, and step.'), enum: pair('列挙体の種類。', 'Enum kind.'), array: pair('要素数と種類を指定した固定配列。', 'Fixed array specified by element count and kind.'), format: pair('型とメンバの設定から固定形式を作る。', 'Create a fixed format from a type and member layout.'), bytes: pair('再利用できるバイトバッファを作る。', 'Create a reusable byte buffer.'), 'Kind.bits': pair('必要なビット数。', 'Required bit count.'), 'Format.size': pair('固定形式のサイズ。', 'Fixed-format size.'), 'Format.encode': pair('値を固定形式の文字列へ符号化。', 'Encode a value as a fixed-format string.'), 'Format.encodeInto': pair('既存の `Bytes` へ符号化。', 'Encode into an existing `Bytes` buffer.'), 'Format.decode': pair('データと開始位置から値と次の位置を返す。', 'Decode data from a starting position, returning value and next position.'), 'Format.decodeInto': pair('既存のテーブルへ復号し次の位置を返す。', 'Decode into an existing table and return the next position.'), 'Bytes.size': pair('バッファのバイト数。', 'Buffer size in bytes.'), 'Bytes.toString': pair('バッファを文字列にする。', 'Convert the buffer to a string.'), 'Bytes.resize': pair('バッファのサイズを変更。', 'Resize the buffer.'), encode: pair('テーブルをスキーマ不要のバイト列へ保存。', 'Encode a table as schema-free binary data.'), decode: pair('スキーマ不要のバイト列からテーブルを復元。', 'Decode a table from schema-free binary data.'), serialize: pair('書き込みストリームとテーブルを指定して保存。', 'Serialize a table to a supplied write stream.'), deserialize: pair('読み込みストリームからテーブルを復元。', 'Deserialize a table from a read stream.')
  }),
  entry('stream', 'stream', 'メモリ上のバイトストリーム', 'In-memory byte streams', [
    pair('`MemoryReader.read(buffer)` は実際に読めたバイト数を返します。終端は0です。`MemoryWriter.toString()` で書き込み結果を取得します。', '`MemoryReader.read(buffer)` returns the bytes read, or 0 at end of input. Obtain written data with `MemoryWriter.toString()`.'),
    pair('`dispose()` 後の使用は `Error.Disposed` です。', 'Using a disposed stream returns `Error.Disposed`.')
  ], {
    reader: pair('文字列または `Bytes` からリーダーを作る。', 'Create a reader from a string or `Bytes`.'), writer: pair('空のメモリライターを作る。', 'Create an empty memory writer.'), 'MemoryReader.read': pair('指定バッファへ読み込み、読めたバイト数を返す。', 'Read into the supplied buffer and return the bytes read.'), 'MemoryWriter.write': pair('バイトバッファを追記。', 'Append a byte buffer.'), 'MemoryWriter.toString': pair('書き込み済みデータを文字列として取得。', 'Get written data as a string.')
  }),
  entry('net', 'net', '待機しない UDP 通信', 'Nonblocking UDP networking', [
    pair('アドレスとポートを指定して送受信します。受信は待機せず、データがなければタプルの先頭が `nil^` です。', 'Send and receive using addresses and ports. Receiving never waits; the first tuple item is `nil^` when no data is available.'),
    pair('`Bytes` を使うオーバーロードには `std.binary` の登録も必要です。', 'The `Bytes` overloads also require `std.binary` registration.')
  ], {
    udp: pair('UDP ソケットを作る。', 'Create a UDP socket.'), 'Udp.bind': pair('ローカルのアドレス・ポートへ結び付ける。', 'Bind to a local address and port.'), 'Udp.setPeer': pair('既定の送信先アドレス・ポートを設定。', 'Set the default destination address and port.'), 'Udp.send': pair('既定の相手へデータを送る。', 'Send data to the default peer.'), 'Udp.sendTo': pair('`(データ, アドレス, ポート)` を指定して送信。', 'Send using `(data, address, port)`.'), 'Udp.receive': pair('データ・送信元アドレス・ポートのタプルを取得。', 'Receive a tuple of data, sender address, and port.'), 'Udp.getLocal': pair('ローカルのアドレス・ポートを取得。', 'Get the local address and port.'), 'Udp.setBroadcast': pair('ブロードキャスト送信の有効・無効。', 'Enable or disable broadcast sending.'), 'Udp.receiveInto': pair('既存バッファで受信し、バッファ・送信元・ポートを返す。', 'Receive into an existing buffer, returning the buffer, sender, and port.')
  }),
  entry('load', 'load', '実行時のソース読み込み', 'Runtime source loading', [
    pair('`file(path)` と `text(source, name)` は呼び出せる手続きを返します。読み込んだ本体の実行は、その手続きを呼んだときです。', '`file(path)` and `text(source, name)` return a callable procedure; call it to run the loaded body.'),
    pair('`require^` と異なり読み込みごとにコンパイルします。モジュールの実行結果は公開メンバのテーブル、スクリプトの結果はその `return^` です。', 'Unlike `require^`, each load compiles anew. Modules return their exports table; scripts return their own `return^` result.'),
    pair('`text` の第2引数は診断用の名前で、内側の相対 `require^` の基準にもなります。', 'The second argument of `text` supplies the diagnostic name and base for relative `require^` paths.')
  ], {
    file: pair('実行時のパスからソースを読み込む。', 'Load source from a runtime path.'), text: pair('ソース文字列と名前から読み込む。', 'Load source text with a supplied name.')
  }),
  entry('sys', 'sys', '共通のシステムエラー', 'Shared system errors', [
    pair('`std.sys.Error.OutOfMemory` は複数のライブラリが共有するメモリ不足の型です。このモジュールには独立した関数はありません。', '`std.sys.Error.OutOfMemory` is the out-of-memory type shared by multiple libraries. This module has no standalone functions.')
  ]),
  entry('debug', 'debug', 'デバッグ出力とスタック情報', 'Debug output and stack information', [
    pair('`log` はデバッグのため `f^` として公開され、関数内からも呼べます。出力先は標準出力です。', '`log` is exposed as `f^` for debugging and can be called inside functions. It writes to standard output.')
  ], {
    log: pair('文字列を改行付きでデバッグ出力。', 'Write a debug string followed by a newline.'), traceback: pair('現在の呼び出しスタックを文字列で取得。', 'Get the current call stack as a string.')
  })
]

export const valueOperations = {
  new: pair('成分を指定して値を作る。', 'Construct a value from components.'),
  '+': pair('和。', 'Addition.'), '-': pair('差、または単項の符号反転。', 'Subtraction or unary negation.'),
  '*': pair('積。対応する型はシグネチャを参照。', 'Multiplication; supported operand types are listed in the signatures.'),
  '/': pair('商。対応する型はシグネチャを参照。', 'Division; supported operand types are listed in the signatures.'),
  abs: pair('絶対値。', 'Absolute value.'), length: pair('長さ。', 'Length.'), normalized: pair('正規化した値。', 'Normalized value.'),
  dot: pair('内積。', 'Dot product.'), cross: pair('外積。', 'Cross product.'),
  lerp: pair('`(相手, 係数)` の線形補間。', 'Linear interpolation with `(other, factor)`.'),
  slerp: pair('`(相手, 係数)` の球面線形補間。', 'Spherical interpolation with `(other, factor)`.'),
  clamp: pair('下限・上限へ成分を制限。', 'Clamp components between lower and upper bounds.'),
  tostring: pair('文字列表現。', 'String representation.'),
  zero: pair('零の値。', 'Zero value.'), one: pair('各成分が1の値。', 'Value with every component equal to one.'),
  right: pair('右方向の単位ベクトル。', 'Right unit vector.'), left: pair('左方向の単位ベクトル。', 'Left unit vector.'),
  up: pair('上方向の単位ベクトル。', 'Up unit vector.'), down: pair('下方向の単位ベクトル。', 'Down unit vector.'),
  forward: pair('前方向の単位ベクトル。', 'Forward unit vector.'), back: pair('後方向の単位ベクトル。', 'Back unit vector.'),
  identity: pair('単位回転。', 'Identity rotation.'), conjugate: pair('共役。', 'Conjugate.'), inverse: pair('逆元。', 'Inverse.'),
  i: pair('実部0、虚部1の虚数単位。', 'Imaginary unit with real part 0 and imaginary part 1.'),
  fromTo: pair('2つの方向ベクトルを結ぶ回転。零方向では `nil^`。', 'Rotation between two direction vectors; zero directions return `nil^`.'),
  angle: pair('角度。', 'Angle.'), phase: pair('偏角。', 'Phase.'), norm: pair('ノルム。', 'Norm.'),
  fromAxisAngle: pair('軸と角度から回転を作る。', 'Construct a rotation from an axis and angle.'),
  fromEuler: pair('オイラー角から回転を作る。', 'Construct a rotation from Euler angles.'),
  rotate: pair('ベクトルを回転。', 'Rotate a vector.')
}
