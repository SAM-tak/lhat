import { readFile, mkdir, writeFile } from 'node:fs/promises'
import { fileURLToPath } from 'node:url'
import path from 'node:path'
import { standardLibraries, valueOperations } from '../docs/.vitepress/stdlib.mjs'

const input = process.argv[2]
if (!input || process.argv.length !== 3) {
  throw new Error('Usage: node scripts/docs-stdlib.mjs <lhat --dump-host-api output.json>')
}
const api = JSON.parse(await readFile(input, 'utf8'))
const docs = fileURLToPath(new URL('../docs/', import.meta.url))
const filename = name => name.slice(4).replaceAll('.', '-') + '.md'
const code = text => '`' + String(text).replaceAll('|', '\\|') + '`'
const fence = (text, lang = 'text') => '```' + lang + '\n' + text + '\n```'
const source = file => `https://github.com/SAM-tak/lhat/blob/main/stdlib/${file}`
const moduleNames = new Set(standardLibraries.map(module => module.name))
for (const item of [...api.types, ...api.functions]) {
  if (item.module?.startsWith('std.') && !moduleNames.has(item.module)) {
    throw new Error(`Add documentation for new module: ${item.module}`)
  }
}

const labels = {
  ja: {
    index: '標準ライブラリ', module: 'モジュール', purpose: '用途', usage: '取り込み', examples: '使用例', notes: '使い方と規則', types: '型',
    functions: '関数', members: 'メソッドと演算子', constants: '定数', fields: 'フィールド', variants: 'エラー種別',
    value: '値', name: '名前', type: '型', source: '実装と登録',
    disposal: '資源への参照を解放します。',
    intro: 'ホストが登録して公開する標準モジュールのリファレンスです。各ページに用途、型、関数、メソッド、定数と登録シグネチャを掲載します。',
    availability: 'この一覧は CLI が登録するモジュールを対象にしています。自作ホストでは必要なモジュールを登録してください。ビルド設定によって利用可能なモジュールが変わる場合があります。',
    signatures: 'シグネチャの `f^`・`p^` は関数・手続き、`self^` は受け手です。`A|B` は成功値とエラーなどの合併で、エラーは `try^`・`catch^` などで扱います。同名の複数の署名はオーバーロードです。',
    generic: '`ARG0`・`ARG1` は第0・第1実引数の型です。`.T0` は第0型引数、`.ResultType` はコルーチンの最終結果型、`.ReturnType` はサブルーチンの戻り値型を表します。`self^.T0` は受け手の第0型引数です。',
    valueType: 'ホスト値の成分は書き換え可能な32ビット実数です。L^ からは `number^` として読み書きします。通常のテーブルなどへ保持する場合は `box^` による箱を使います。',
    generated: 'シグネチャは CLI の登録情報から生成しています。表記の読み方は[標準ライブラリ](./index.md)を参照してください。',
    errorType: '各種別はエラーの型です。共通メンバと処理規則は[エラー](../errors.md)を参照してください。',
    inheritance: '基底型から継承するメンバも利用できます。',
    hostType: 'ホストが公開する型です。生成・利用には以下の API を使います。',
    vectorNotes: 'ベクトル同士の `*`・`/` は成分ごと、数値との演算はスカラーによる拡大・縮小です。`lerp` は係数を0〜1に制限しません。零ベクトルの正規化は零のままです。',
    complexNotes: '`new(re, im)` は実部と虚部を取ります。複素数同士の `*` は複素積です。零の正規化は零のままです。',
    quaternionNotes: '`new(x, y, z, w)` の `w` はスカラー部です。クォータニオン同士の `*` はハミルトン積です。`fromAxisAngle(axis, radians)` はラジアンを取り、零の軸では `nil^`。`std.math.vector3` の登録が必要です。',
    vector2Notes: '座標は右が+x、下が+yです。`angle()` は `atan2(y, x)`、`rotate(radians)` はラジアンで回転します。',
    vector3Notes: '+Zが前、−Zが後です。',
    further: '言語コアの規則は[概覧](../index.md)、利用例は[ガイド](../../index.md)を参照してください。'
  },
  en: {
    index: 'Standard library', module: 'Module', purpose: 'Purpose', usage: 'Import', examples: 'Example', notes: 'Usage and rules', types: 'Types',
    functions: 'Functions', members: 'Methods and operators', constants: 'Constants', fields: 'Fields', variants: 'Error kinds',
    value: 'Value', name: 'Name', type: 'Type', source: 'Implementation and registration',
    disposal: 'Release the reference to the resource.',
    intro: 'Reference for standard modules registered and exposed by the host. Each page lists its purpose, types, functions, methods, constants, and registered signatures.',
    availability: 'This list covers modules registered by the CLI. Register the modules you need in a custom host. Availability may vary with build options.',
    signatures: 'In signatures, `f^` and `p^` indicate functions and procedures, and `self^` marks a receiver. `A|B` is a union, such as success and error results; handle errors with `try^`, `catch^`, or narrowing. Multiple signatures for a name are overloads.',
    generic: '`ARG0` and `ARG1` are the types of the zeroth and first arguments. `.T0` extracts the first type argument, `.ResultType` the final coroutine result, and `.ReturnType` the subroutine result. `self^.T0` is the receiver\'s first type argument.',
    valueType: 'Host-value components are writable 32-bit floats, read and written from L^ as `number^`. Use `box^` when retaining these values in ordinary tables or similar storage.',
    generated: 'Signatures are generated from CLI registration data. See [Standard library](./index.md) for notation.',
    errorType: 'Each variant is an error type. See [Errors](../errors.md) for common members and handling rules.',
    inheritance: 'Members inherited from the base type are also available.',
    hostType: 'A host-exposed type, created and used through the APIs below.',
    vectorNotes: 'Vector `*` and `/` are component-wise; numeric operands scale values. `lerp` does not clamp its factor to 0–1. Normalizing a zero vector leaves it zero.',
    complexNotes: '`new(re, im)` takes real and imaginary parts. Complex `*` performs complex multiplication. Normalizing zero leaves it zero.',
    quaternionNotes: 'In `new(x, y, z, w)`, `w` is the scalar part. Quaternion `*` is the Hamilton product. `fromAxisAngle(axis, radians)` takes radians and returns `nil^` for a zero axis. Registration of `std.math.vector3` is required.',
    vector2Notes: 'Coordinates are +x right and +y down. `angle()` is `atan2(y, x)`; `rotate(radians)` takes radians.',
    vector3Notes: '+Z is forward and −Z is back.',
    further: 'See the [overview](../index.md) for language-core rules and the [guide](../../index.md) for examples.'
  }
}

function description(module, item, locale) {
  const key = item.type ? `${item.type}.${item.name}` : item.name
  const text = module.api[key] ?? (module.name.startsWith('std.math.') ? valueOperations[item.name] : undefined)
  if (text) return text[locale]
  if (item.name === 'dispose') return labels[locale].disposal
  throw new Error(`Missing description: ${module.name}.${key}`)
}

for (const locale of ['ja', 'en']) {
  const t = labels[locale]
  const directory = path.join(docs, locale, 'reference', 'stdlib')
  await mkdir(directory, { recursive: true })
  const index = [`# ${t.index}`, t.intro, t.availability,
    `| ${t.module} | ${t.purpose} |\n| --- | --- |\n` + standardLibraries.map(module =>
      `| [${module.name}](./${filename(module.name)}) | ${module[locale]} |`).join('\n'),
    `## ${locale === 'ja' ? 'ホスト値の構築' : 'Constructing host values'}`,
    locale === 'ja'
      ? '標準ライブラリのホスト値は次の5種類です。成分を指定した構築には、それぞれのモジュール直下の `new` 関数を使います。型名は型注釈などに使い、`型.new(...)` や `new^型{…}` による構築には対応していません。'
      : 'The standard library provides the following five host-value types. To construct them from components, use `new` directly on their module. Type names are used in annotations and similar contexts; construction with `Type.new(...)` or `new^Type{…}` is not supported.',
    `| ${t.type} | ${locale === 'ja' ? '構築関数' : 'Constructor'} |\n| --- | --- |\n` + api.types
      .filter(type => type.kind === 'hostvalue' && moduleNames.has(type.module))
      .map(type => `| [${code(`${type.module}.${type.name}`)}](./${filename(type.module)}) | ${code(`${type.module}.new(...)`)} |`).join('\n'),
    locale === 'ja'
      ? '`let^vec2 = import^std.math.vector2` とモジュールに別名を付ければ、`vec2.new(...)` と書けます。`let^Vector2 = std.math.vector2.Vector2` は型の別名で、用途が異なります。`zero()`・`one()`・`identity()` など、値を作る別の関数も各モジュールで提供しています。'
      : 'Aliasing a module with `let^vec2 = import^std.math.vector2` allows `vec2.new(...)`. In contrast, `let^Vector2 = std.math.vector2.Vector2` aliases the type. Individual modules also offer other value factories such as `zero()`, `one()`, and `identity()`.',
    `## ${locale === 'ja' ? 'シグネチャの読み方' : 'Reading signatures'}`, t.signatures, t.generic, t.further]
  await writeFile(path.join(directory, 'index.md'), index.join('\n\n') + '\n')

  for (const module of standardLibraries) {
    const types = api.types.filter(item => item.module === module.name)
    const functions = api.functions.filter(item => item.module === module.name)
    if (!types.length && !functions.length) throw new Error(`Module missing from host API: ${module.name}`)
    const content = [`# ${module.name}`, module[locale] + (locale === 'ja' ? '。' : '.'),
      `## ${t.usage}`, fence(`import^${module.name}`, 'lhat')]
    const sample = `sample/docs/stdlib/${filename(module.name).replace(/\.md$/, '.lh')}`
    // Read before linking so a missing executable example fails generation.
    await readFile(new URL(`../${sample}`, import.meta.url), 'utf8')
    content.push(`## ${t.examples}`)
    const valueType = types.find(type => type.kind === 'hostvalue')
    if (valueType) {
      const fullType = `${module.name}.${valueType.name}`
      content.push(locale === 'ja'
        ? `${code(fullType)} は値の型です。成分を指定して構築するには、モジュール直下の ${code(`${module.name}.new(...)`)} を使います。モジュールに別名を付ければ ${code('別名.new(...)')} と書けます。`
        : `${code(fullType)} is the value type. To construct a value from components, use ${code(`${module.name}.new(...)`)} directly on the module. Aliasing the module also allows ${code('alias.new(...)')}.`,
      locale === 'ja'
        ? `型にも別名を付け、型注釈に使えます。構築関数の ${code('new')} はモジュールの関数であり、${code(`${fullType}.new(...)`)} や ${code(`new^${fullType}{…}`)} では構築できません。`
        : `You can also alias the type for annotations. The constructor ${code('new')} belongs to the module; ${code(`${fullType}.new(...)`)} and ${code(`new^${fullType}{…}`)} are not supported construction forms.`)
    }
    if (module.name === 'std.sys') content.push(locale === 'ja'
      ? 'エラーの処理方法を示すため、例ではエラー値を明示的に作っています。実際にメモリを使い切る操作は行いません。'
      : 'This example constructs an error value explicitly to demonstrate handling; it does not exhaust memory.')
    content.push(`<<< @/../${sample}{lhat}`,
      locale === 'ja' ? 'リポジトリのルートから実行できます。' : 'Run from the repository root:',
      fence(`lhat ${sample}`, 'sh'))
    const notes = module.notes.map(note => note[locale])
    if (module.name.startsWith('std.math.vector')) notes.push(t.vectorNotes)
    if (module.name === 'std.math.vector2') notes.push(t.vector2Notes)
    if (module.name === 'std.math.vector3') notes.push(t.vector3Notes)
    if (module.name === 'std.math.complex') notes.push(t.complexNotes)
    if (module.name === 'std.math.quaternion') notes.push(t.quaternionNotes)
    if (notes.length) content.push(`## ${t.notes}`, ...notes)
    if (types.length) {
      content.push(`## ${t.types}`)
      for (const type of types) {
        content.push(`### ${type.name}`)
        if (type.kind === 'errordef') {
          content.push(t.errorType, type.variants.map(variant => `- ${code(`${type.name}.${variant}`)}`).join('\n'))
        } else {
          content.push(type.kind === 'hostvalue' ? t.valueType : t.hostType)
          if (type.base_name) content.push(`${code(`${type.base_module}.${type.base_name}`)} — ${t.inheritance}`)
          if (type.fields?.length) content.push(`| ${t.fields} | ${t.type} |\n| --- | --- |\n` +
            type.fields.map(field => `| ${code(field.name)} | ${code('number^')} (${field.type}) |`).join('\n'))
        }
      }
    }
    const constants = functions.filter(item => item.kind === 'const')
    if (constants.length) content.push(`## ${t.constants}`,
      `| ${t.name} | ${t.value} |\n| --- | --- |\n` + constants.map(item => `| ${code(item.name)} | ${code(item.value)} |`).join('\n'))
    for (const [heading, entries] of [
      [t.functions, functions.filter(item => item.kind === 'func')],
      [t.members, functions.filter(item => item.kind === 'member' || item.kind === 'hostvalue_member')]
    ]) {
      if (!entries.length) continue
      content.push(`## ${heading}`, t.generated)
      const grouped = new Map()
      for (const item of entries) {
        const key = item.type ? `${item.type}.${item.name}` : item.name
        if (!grouped.has(key)) grouped.set(key, [])
        grouped.get(key).push(item)
      }
      for (const [name, overloads] of grouped) {
        content.push(`### ${name}`, description(module, overloads[0], locale),
          fence(overloads.map(item => item.signature).join('\n')))
      }
    }
    content.push(`## ${t.source}`, `[${module.source}.h](${source(`${module.source}.h`)}) / [${module.source}.c](${source(`${module.source}.c`)})`)
    if (module.name.startsWith('std.math.')) content.push(`[mathvalue_impl.h](${source('mathvalue_impl.h')})`)
    if (module.name === 'std.binary') content.push(`[binary_data.c](${source('binary_data.c')})`)
    await writeFile(path.join(directory, filename(module.name)), content.join('\n\n') + '\n')
  }
}
console.log(`Wrote ${standardLibraries.length + 1} standard-library pages per language.`)
