# 開発環境と診断をつなぐ

## ホストが公開する型をエディタへ渡す

`app.Node` のようなホスト固有の型は、通常の CLI や言語サーバには最初から存在しません。
実行時と同じ登録を行った Program から、`lhat_program_dump_host_api` で JSON を生成します。
言語サーバはその型、シグネチャ、初期束縛を読み込み、ホストを起動せずに検査できます。
コールバックの実装やホストのポインタは含まれません。

[最小ホスト](./getting-started.md)にも出力モードがあります。
リポジトリのルートから、作業するスクリプトのフォルダに出力します。

```sh
./build/embedding/embedding_host --dump-host-api > sample/embedding/lhat-host.json
```

Windows では `embedding_host.exe` を使います。リダイレクトは UTF-8 を保つシェルで行ってください。
ファイル出力モードをホストに実装するなら、UTF-8 のバイト列をそのまま書き込みます。

VS Code で `sample/embedding` をフォルダとして開くと、その `lhat-host.json` を使えます。
ファイルは API 登録の変更後に再生成します。このリポジトリでは生成物として Git の対象から外しています。
通常の `lhat --dump-host-api` が出力するのは CLI 自身の API であり、あなたのアプリケーションの API ではありません。

## 型検査の診断を表示する

診断には二つの入口があります。

| 入口 | 内容 |
| --- | --- |
| `lhat_program_diagnostic_count` / `lhat_program_diagnostic` | ファイルを読めない、依存関係が循環するなどの Program の問題 |
| `lhat_program_units` → `lhat_unit_diagnostic_count` / `lhat_unit_diagnostic_write` | 各ファイルの構文・型などの問題 |

`lhat_program_check` が非 NULL を返すだけでは成功とは限りません。
`lhat_program_has_errors` と `lhat_unit_ok` も確認します。
コンパイル失敗は `lhat_program_compile_failure` と `lhat_compile_message_write` で報告します。

多くの出力関数は `(NULL, 0)` で必要バイト数を調べられます。
戻り値は終端 NUL を含まないため、バッファを `needed + 1` バイト確保して再度呼びます。
短い固定長バッファを使う場合は、切り詰めが起こることを考慮してください。

## 実行エラーを表示する

`lhat_run_error_write` は実行失敗の場所とメッセージを整形します。
より細かい UI を作る場合は `lhat_machine_fault_depth` / `lhat_machine_fault_frame` でフレームを取得できます。
これらの情報は次の実行で変わるため、先に文字列やホスト側のデータへコピーします。

## スクリプトの契約と Inspector

実行前に `lhat_unit_export_conforms` で入口の型を検査できます。
`lhat_unit_export_type` はコンパイル済みの公開値の型を、`LhatRuntimeType` の記述子として返します。
オブジェクトのメンバを列挙する Inspector やプロパティ編集に使えます。
再読込で無効になる記述子を UI が保持し続けないようにしてください。
アノテーションを受け付ける場合は、使える名前と対象を `lhat_register_annotation` で登録します。

デバッグのためにホストへ DAP を組み込む場合は、[デバッガの設計](https://github.com/SAM-tak/lhat/blob/main/DesignDocuments/09-debugger.md)を参照してください。
VS Code 拡張をインストールするだけで、任意のホストへの接続が自動設定されるわけではありません。
