/**
 * Deploy script: gzip the bundle, its chunks and the language catalogs into data/www/.
 * Usage: npm run deploy (builds + deploys)
 *
 * The device's static server sends <path>.gz with Content-Encoding: gzip when it exists
 * (http_service.cpp), so only .gz files are deployed. bundle.js loads on every visit; a
 * chunk-*.js is a page fetched the first time it is opened (rollup.config.js). Ukrainian is
 * inside bundle.js; the other catalogs are fetched as /lang/<lang>.json when chosen.
 *
 * data/www/build.txt lists what was deployed with each file's hash. data.bin depends on it
 * (CMakeLists.txt), so a new or renamed chunk rebuilds the image without naming it there.
 * Uses Node.js zlib instead of CLI gzip (Windows compatibility).
 */
import { createReadStream, createWriteStream, copyFileSync, existsSync, mkdirSync,
         readdirSync, readFileSync, statSync, unlinkSync, writeFileSync } from 'fs';
import { createHash } from 'crypto';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { createGzip } from 'zlib';
import { pipeline } from 'stream/promises';

const __dirname = dirname(fileURLToPath(import.meta.url));
const dist = join(__dirname, '..', 'dist');
const pub = join(__dirname, '..', 'public');
const i18n = join(__dirname, '..', 'src', 'i18n');
const www = join(__dirname, '..', '..', 'data', 'www');
const LANGS = ['en', 'de', 'pl'];
const manifest = [];

async function gz(src, dst) {
  if (!existsSync(src)) {
    console.error(`Missing: ${src}`);
    process.exit(1);
  }
  await pipeline(createReadStream(src), createGzip({ level: 9 }), createWriteStream(dst));
  const rel = dst.slice(www.length + 1);
  const hash = createHash('sha256').update(readFileSync(src)).digest('hex').slice(0, 16);
  manifest.push(`${hash}  ${rel}`);
  console.log(`  data/www/${rel}  ${statSync(dst).size} B`);
}

// A watch build (npm run dev, the dev server) is unminified and carries source maps: deployed, it
// about doubles bundle.js.gz. Only a production build (npx rollup -c) has no map.
if (existsSync(join(dist, 'bundle.js.map'))) {
  console.error('dist/ is a development build (it has bundle.js.map). Run: rm -rf dist && npx rollup -c');
  process.exit(1);
}

console.log('Deploying to data/www');
// chunks from an earlier build that this one no longer makes
for (const f of readdirSync(www)) {
  if (/^chunk-.*\.js\.gz$/.test(f)) unlinkSync(join(www, f));
}
await gz(join(dist, 'bundle.js'), join(www, 'bundle.js.gz'));
for (const f of readdirSync(dist).filter((f) => /^chunk-.*\.js$/.test(f)).sort()) {
  await gz(join(dist, f), join(www, `${f}.gz`));
}
await gz(join(dist, 'bundle.css'), join(www, 'bundle.css.gz'));
mkdirSync(join(www, 'lang'), { recursive: true });
for (const lang of LANGS) {
  await gz(join(i18n, `${lang}.json`), join(www, 'lang', `${lang}.json.gz`));
}

// The page itself is not built: rollup writes only the scripts and styles it loads.
const indexSrc = join(pub, 'index.html');
copyFileSync(indexSrc, join(www, 'index.html'));
manifest.push(`${createHash('sha256').update(readFileSync(indexSrc)).digest('hex').slice(0, 16)}  index.html`);

writeFileSync(join(www, 'build.txt'), manifest.join('\n') + '\n');
console.log('Done.');
