import { defineConfig } from 'vitepress'

const repository = 'https://github.com/SAM-tak/lhat'
const pages = {
  ja: [
    { text: 'L^ とは', link: '/ja/' },
    { text: 'インストール', link: '/ja/guide/installation' },
    { text: '最初のプログラム', link: '/ja/guide/first-program' },
    { text: '言語の基本', link: '/ja/guide/basics' },
    { text: 'VS Code の使い方', link: '/ja/tools/vscode' }
  ],
  en: [
    { text: 'About L^', link: '/en/' },
    { text: 'Installation', link: '/en/guide/installation' },
    { text: 'Your first program', link: '/en/guide/first-program' },
    { text: 'Language basics', link: '/en/guide/basics' },
    { text: 'Using VS Code', link: '/en/tools/vscode' }
  ]
}

export default defineConfig({
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
        nav: [{ text: 'ガイド', link: '/ja/' }],
        sidebar: [{ text: '使い始める', items: pages.ja }],
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
        nav: [{ text: 'Guide', link: '/en/' }],
        sidebar: [{ text: 'Getting started', items: pages.en }],
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
