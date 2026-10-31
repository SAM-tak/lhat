# ユーザー向けドキュメントの編集

日本語の `ja/` を正本とし、英語の `en/` に同じ相対パスで翻訳を置きます。
このファイルは編集者向けで、公開サイトには含めません。
設計上の決定や未決事項は、従来どおり `../DesignDocuments/` に記録してください。

## ローカルで読む・ビルドする

Node.js 22 以降を用意し、リポジトリのルートで実行します。
Node.js はドキュメント用で、L^ 本体のビルドには不要です。
VitePress 1.x の Vite 5 依存には既知の脆弱性があるため、`package.json` の
`overrides` で修正版の Vite 6.4.5 を使用します。更新時はビルドとプレビューを確認してください。

```sh
npm ci
npm run docs:dev
```

表示された URL をブラウザで開きます。公開前の確認には次を使います。

```sh
npm run docs:translations
npm run docs:build
npm run docs:preview
```

VitePress のビルドでは内部ページへのリンク切れも検査します。
生成物とキャッシュは Git に含めません。

## ページを追加・翻訳する

1. `ja/` に日本語のページを追加・更新します。ファイル名は英語の kebab-case にします。
2. `.vitepress/config.ts` の日本語側の目次に追加します。
3. 英訳したら同じ相対パスで `en/` に置き、英語側の目次にも追加します。
4. 内容を照合してから、翻訳元の状態を記録します。

```sh
npm run docs:translations -- --record guide/installation.md
```

複数のファイルを続けて指定できます。`translations.json` に日本語本文の SHA-256 を記録します。
改行コードの違いは無視します。翻訳元の更新は通常の `npm run docs:translations` で検出します。
この記録は内容の一致を人が確認した印であり、翻訳の正確さを自動判定するものではありません。
英語の文章だけを修正した場合は、日本語のハッシュを記録し直す必要はありません。

未翻訳・要更新は CI の警告にし、日本語版の公開は止めません。
未翻訳のページは英語側の目次に「Japanese only」と明示して日本語ページへ案内し、
存在しない英語ページへのリンクは作らないでください。
翻訳が揃っているページは、サイト上部の言語メニューで切り替えられます。

## コード例

実行できる例は `sample/` に保存し、日英から同じファイルを読み込みます。
たとえば次の記法でコードを表示できます。

```md
<<< @/../sample/docs/hello.lh{text}
```

L^ の例は現時点では `text` を指定してプレーンテキストとして表示します。
言語の規則を誤って示す他言語のハイライトは割り当てません。
例を変更した場合は実行結果も確認し、日英両方の説明を更新してください。

## GitHub Pages の初回設定

リポジトリの **Settings → Pages → Build and deployment → Source** を **GitHub Actions** にします。
`main` にこの構成を push すると、`.github/workflows/docs.yml` がサイトをビルド・公開します。
PR ではビルドだけを実行し、公開しません。手動実行も可能です。
公開先は `https://sam-tak.github.io/lhat/`、日本語は `/lhat/ja/`、英語は `/lhat/en/` です。

公開内容は `main` に追従する単一バージョンです。
リポジトリ名や独自ドメインを変更するときは `.vitepress/config.ts` の `base` も見直してください。
GitHub Pages の設定方法は [GitHub の公式ドキュメント](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages)を参照してください。
