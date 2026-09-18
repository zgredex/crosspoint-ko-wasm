// Pick representative pages: per-spine page counts + ink/intermediate-level percentages.
const path = require('path');
const fs = require('fs');
const createKoEngine = require(path.join(__dirname, '..', '..', 'build-wasm', 'ko_xtch_wasm.js'));

(async () => {
  const api = await createKoEngine();
  api._ko_init(464, 764); // advisory: geometry is derived from the spec margins (14/8/22/8)
  for (const book of process.argv.slice(2)) {
    const buf = fs.readFileSync(book);
    const p = api._malloc(buf.length);
    api.HEAPU8.set(buf, p);
    if (api._ko_load_epub(p, buf.length, '/b.epub') < 0) { console.log(book, 'load fail'); continue; }
    const n = api._ko_spine_count();
    const counts = [];
    for (let s = 0; s < n; s++) counts.push(api._ko_build_spine(s));
    console.log(path.basename(book), '| spines', n, '| pages per spine:', counts.join(','));
    for (let s = 0; s < n; s++) {
      if (counts[s] < 2) continue;
      const perPage = [];
      for (let pg = 0; pg < Math.min(counts[s], 3); pg++) {
        // render_page() renders the CURRENTLY BUILT spine, so rebuild before each page -
        // building them all up front made every spine report the last spine's numbers.
        api._ko_build_spine(s);
        api._ko_render_page(pg);
        api._ko_compose_rgba(0);
        const rgba = new Uint32Array(api.HEAPU8.buffer, api._ko_rgba_ptr(), 480 * 800);
        let ink = 0, mid = 0;
        for (let i = 0; i < rgba.length; i++) {
          const g = rgba[i] & 0xff;
          if (g <= 128) ink++;
          if (g === 128 || g === 205) mid++;
        }
        perPage.push(`p${pg}: ink ${(100 * ink / rgba.length).toFixed(1)}% inter ${(100 * mid / rgba.length).toFixed(1)}%`);
      }
      console.log('   spine', String(s).padStart(2), 'pages', String(counts[s]).padStart(3), ' ', perPage.join('   '));
    }
  }
})();
