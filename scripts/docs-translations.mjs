import { createHash } from 'node:crypto'
import { readdir, readFile, writeFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import path from 'node:path'

const root = fileURLToPath(new URL('../docs/', import.meta.url))
const manifestPath = path.join(root, 'translations.json')
const args = process.argv.slice(2)
const record = args[0] === '--record'
if (args.length && (!record || args.length < 2)) {
  throw new Error('Usage: npm run docs:translations [-- --record <relative-page.md> ...]')
}

async function pages(directory, prefix = '') {
  const result = []
  for (const entry of await readdir(directory, { withFileTypes: true })) {
    const relative = path.posix.join(prefix, entry.name)
    if (entry.isDirectory()) result.push(...await pages(path.join(directory, entry.name), relative))
    else if (entry.name.endsWith('.md')) result.push(relative)
  }
  return result.sort()
}

// Normalize line endings so Windows and Linux produce the same source hash.
async function hash(locale, page) {
  const source = (await readFile(path.join(root, locale, page), 'utf8')).replace(/\r\n/g, '\n')
  return createHash('sha256').update(source).digest('hex')
}

const japanese = await pages(path.join(root, 'ja'))
const english = await pages(path.join(root, 'en'))
const manifest = JSON.parse(await readFile(manifestPath, 'utf8'))
if (record) {
  for (const page of args.slice(1)) {
    if (!japanese.includes(page) || !english.includes(page)) {
      throw new Error(`Both ja/${page} and en/${page} must exist before recording a reviewed translation.`)
    }
    manifest[page] = await hash('ja', page)
  }
  const sorted = Object.fromEntries(Object.entries(manifest).sort(([a], [b]) => a.localeCompare(b)))
  await writeFile(manifestPath, `${JSON.stringify(sorted, null, 2)}\n`)
}

let pending = 0
for (const page of japanese) {
  let status
  if (!english.includes(page)) status = 'MISSING'
  else if (manifest[page] !== await hash('ja', page)) status = 'NEEDS REVIEW'
  if (status) {
    pending++
    console.log(`${status}: en/${page} (source: ja/${page})`)
    if (process.env.GITHUB_ACTIONS) {
      console.log(`::warning file=docs/ja/${page}::English translation ${status.toLowerCase()}.`)
    }
  }
}
for (const page of english) {
  if (!japanese.includes(page)) throw new Error(`English page has no Japanese source: ${page}`)
}
console.log(`${japanese.length - pending}/${japanese.length} translations match their reviewed Japanese source.`)
