# L^ (lhat) Language Support

`lhat-lsp` を起動し、VSCode に L^ の型検査の診断（赤波線）だけを届ける薄いクライアント。
ホバー・補完・定義ジャンプは今回のスコープ外（MVP は診断のみ）。

## セットアップ

1. サーバー本体（リポジトリルート）をビルド:

   ```powershell
   . .\scripts\devshell.ps1
   cmake --preset debug
   cmake --build --preset debug --target lhat_lsp
   ```

   `build\debug\lhat-lsp.exe` ができる。

2. この拡張の依存関係を入れてコンパイル:

   ```powershell
   cd vscode-extension
   npm install
   npm run compile
   ```

3. VSCode で **`vscode-extension` フォルダそのものを** 開く
   （`File > Open Folder`）。リポジトリルートを開いたままだと
   `.vscode/launch.json` が認識されず、F5 は今開いているファイルを
   素で（Plain Text として）デバッグしようとしてしまう。
   `vscode-extension` を開いた状態で F5 → 拡張開発ホストが起動する。
   `lhat-lsp.exe` が PATH 上になければ、設定 `lhat.serverPath` に
   `build\debug\lhat-lsp.exe` への絶対パスを指定する。

4. `.lh` ファイルを開くと、保存前の編集内容がそのまま型検査され、
   `require^` で参照する同じワークスペース内の他ファイルも辿って検査される。

## 既知の制約

- 診断のみ。ホバー・補完・定義ジャンプは無い。
- ホストが `lhat_register_func` 等で登録する独自 API は、このサーバーは知らない
  （`print`/`collectgarbage` のみ登録済み）。独自 API を使うスクリプトは
  「no such name in scope」の偽陽性が出る。
- ワークスペース内の `*.lh` を毎回それぞれ独立したルートとして検査するため、
  共有される依存ファイルが多いほど検査コストが増える（`lsp/workspace.c` 参照）。
