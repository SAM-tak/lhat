import { defineConfig } from 'vitepress'
import lhatGrammar from './lhat.tmLanguage.json'
import { standardLibraries } from './stdlib.mjs'

const repository = 'https://github.com/SAM-tak/lhat'
const pages = {
  ja: [
    { text: 'L^ とは', link: '/ja/' },
    { text: 'インストール', link: '/ja/guide/installation' },
    { text: '最初のプログラム', link: '/ja/guide/first-program' },
    { text: '言語の基本', link: '/ja/guide/basics' },
    { text: 'インポートとモジュール', link: '/ja/guide/modules' },
    { text: 'バイナリ保存とストリーム', link: '/ja/guide/binary-data' },
    { text: '並行処理', link: '/ja/guide/tasks' },
    { text: 'VS Code の使い方', link: '/ja/tools/vscode' }
  ],
  en: [
    { text: 'About L^', link: '/en/' },
    { text: 'Installation', link: '/en/guide/installation' },
    { text: 'Your first program', link: '/en/guide/first-program' },
    { text: 'Language basics', link: '/en/guide/basics' },
    { text: 'Imports and modules', link: '/en/guide/modules' },
    { text: 'Binary data and streams', link: '/en/guide/binary-data' },
    { text: 'Concurrency', link: '/en/guide/tasks' },
    { text: 'Using VS Code', link: '/en/tools/vscode' }
  ]
}

const embedding = {
  ja: [
    { text: '組み込みの全体像', link: '/ja/embedding/' },
    { text: '最小のホストアプリケーション', link: '/ja/embedding/getting-started' },
    { text: 'ホスト API と型', link: '/ja/embedding/host-api' },
    { text: 'フレームループと値の寿命', link: '/ja/embedding/runtime' },
    { text: '開発環境と診断', link: '/ja/embedding/tooling' },
    { text: 'リロードと配布', link: '/ja/embedding/deployment' },
    { text: '標準ライブラリのバインド実装', link: '/ja/embedding/binding-examples' }
  ],
  en: [
    { text: 'Embedding overview', link: '/en/embedding/' },
    { text: 'Your first host application', link: '/en/embedding/getting-started' },
    { text: 'Host APIs and types', link: '/en/embedding/host-api' },
    { text: 'Frame loops and lifetimes', link: '/en/embedding/runtime' },
    { text: 'Tooling and diagnostics', link: '/en/embedding/tooling' },
    { text: 'Reloading and distribution', link: '/en/embedding/deployment' },
    { text: 'Standard library binding examples', link: '/en/embedding/binding-examples' }
  ]
}

const reference = {
  ja: [
    { text: '概覧', link: '/ja/reference/' },
    { text: '字句とソース', link: '/ja/reference/lexical' },
    { text: '束縛とスコープ', link: '/ja/reference/bindings' },
    { text: '型', link: '/ja/reference/types' },
    { text: '式と演算子', link: '/ja/reference/expressions' },
    { text: '制御構文', link: '/ja/reference/control-flow' },
    { text: '関数と手続き', link: '/ja/reference/subroutines' },
    { text: 'コルーチン', link: '/ja/reference/coroutines' },
    { text: 'テーブルとオブジェクト', link: '/ja/reference/objects' },
    { text: 'エラー', link: '/ja/reference/errors' },
    { text: 'モジュール', link: '/ja/reference/modules' },
    { text: '注釈', link: '/ja/reference/annotations' },
    {
      text: '標準ライブラリ', link: '/ja/reference/stdlib/', collapsed: true,
      items: standardLibraries.map(({ name }) => ({ text: name, link: `/ja/reference/stdlib/${name.slice(4).replaceAll('.', '-')}` }))
    }
  ],
  en: [
    { text: 'Overview', link: '/en/reference/' },
    { text: 'Lexical structure and source', link: '/en/reference/lexical' },
    { text: 'Bindings and scope', link: '/en/reference/bindings' },
    { text: 'Types', link: '/en/reference/types' },
    { text: 'Expressions and operators', link: '/en/reference/expressions' },
    { text: 'Control flow', link: '/en/reference/control-flow' },
    { text: 'Functions and procedures', link: '/en/reference/subroutines' },
    { text: 'Coroutines', link: '/en/reference/coroutines' },
    { text: 'Tables and objects', link: '/en/reference/objects' },
    { text: 'Errors', link: '/en/reference/errors' },
    { text: 'Modules', link: '/en/reference/modules' },
    { text: 'Annotations', link: '/en/reference/annotations' },
    {
      text: 'Standard library', link: '/en/reference/stdlib/', collapsed: true,
      items: standardLibraries.map(({ name }) => ({ text: name, link: `/en/reference/stdlib/${name.slice(4).replaceAll('.', '-')}` }))
    }
  ]
}

const api = {
  ja: [
    { text: 'C API の入口と所有権', link: '/ja/api/' },
    { text: 'Program とユニット', link: '/ja/api/program' },
    { text: 'ホスト API の登録', link: '/ja/api/registration' },
    { text: 'Machine と値', link: '/ja/api/machine' }
  ],
  en: [
    { text: 'C API overview and ownership', link: '/en/api/' },
    { text: 'Programs and units', link: '/en/api/program' },
    { text: 'Registering host APIs', link: '/en/api/registration' },
    { text: 'Machines and values', link: '/en/api/machine' }
  ]
}

export default defineConfig({
  markdown: {
    languages: [{ ...lhatGrammar, name: 'lhat' }],
    codeTransformers: [{
      name: 'lhat-dim-hats',
      span(node) {
        if (this.options.lang !== 'lhat') return
        node.children = node.children.flatMap(child => {
          if (child.type !== 'text' || !child.value.includes('^')) return [child]
          const parts = child.value.split('^')
          return parts.flatMap((part, index) => [
            ...(index ? [{ type: 'element' as const, tagName: 'span', properties: { style: 'opacity: 0.5' }, children: [{ type: 'text' as const, value: '^' }] }] : []),
            { type: 'text' as const, value: part }
          ])
        })
      }
    }]
  },
  title: 'L^',
  lang: 'ja',
  description: 'L^ programming language documentation / L^ ユーザーガイド',
  base: '/lhat/',
  srcExclude: ['README.md'],
  lastUpdated: true,
  locales: {
    ja: {
      label: '日本語', lang: 'ja', description: 'プログラミング言語 L^ のユーザーガイド',
      themeConfig: {
        nav: [
          { text: 'ガイド', link: '/ja/' },
          { text: 'アプリケーションへの組み込み', link: '/ja/embedding/' },
          { text: 'L^ リファレンス', link: '/ja/reference/' },
          { text: 'C API', link: '/ja/api/' }
        ],
        sidebar: [
          { text: '使い始める', items: pages.ja },
          { text: 'アプリケーションへの組み込み', items: embedding.ja },
          { text: 'L^ リファレンス', items: reference.ja },
          { text: 'C API リファレンス', items: api.ja }
        ],
        outline: { label: 'このページ', level: [2, 3] },
        docFooter: { prev: '前のページ', next: '次のページ' },
        lastUpdated: { text: '最終更新' },
        editLink: { pattern: `${repository}/edit/main/docs/:path`, text: 'このページを GitHub で編集' },
        sidebarMenuLabel: 'メニュー', returnToTopLabel: '先頭に戻る',
        darkModeSwitchLabel: '表示テーマ', langMenuLabel: '言語を選択'
      }
    },
    en: {
      label: 'English', lang: 'en', description: 'User guide for the L^ programming language',
      themeConfig: {
        nav: [
          { text: 'Guide', link: '/en/' },
          { text: 'Embedding', link: '/en/embedding/' },
          { text: 'L^ reference', link: '/en/reference/' },
          { text: 'C API', link: '/en/api/' }
        ],
        sidebar: [
          { text: 'Getting started', items: pages.en },
          { text: 'Embedding in an application', items: embedding.en },
          { text: 'L^ reference', items: reference.en },
          { text: 'C API reference', items: api.en }
        ],
        editLink: { pattern: `${repository}/edit/main/docs/:path`, text: 'Edit this page on GitHub' }
      }
    }
  },
  themeConfig: {
    siteTitle: 'L^ Docs',
    socialLinks: [{ icon: 'github', link: repository }],
    search: {
      provider: 'local',
      options: {
        locales: {
          ja: {
            translations: {
              button: { buttonText: '検索', buttonAriaLabel: 'ドキュメントを検索' },
              modal: {
                noResultsText: '検索結果がありません', resetButtonTitle: '検索をクリア',
                backButtonTitle: '検索を閉じる', displayDetails: '詳細を表示',
                footer: { selectText: '選択', navigateText: '移動', closeText: '閉じる' }
              }
            }
          }
        }
      }
    },
    footer: { message: 'Released under the Apache License 2.0.' }
  }
})
