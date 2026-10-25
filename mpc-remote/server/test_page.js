// Load the remote page in a headless browser and fail on any script error (build.sh runs this when Node and
// Playwright are there). A page whose script stops early can't even connect, so this must never ship.
const path = require('path');
let pw;
try { pw = require(require('child_process').execSync('npm root -g').toString().trim() + '/playwright'); }
catch (e) { console.log('page check skipped (no Playwright)'); process.exit(0); }
(async () => {
  const b = await pw.chromium.launch();
  const p = await b.newPage();
  const errors = [];
  p.on('pageerror', (e) => errors.push(e.message));
  await p.goto('file://' + path.resolve(__dirname, 'index.html'));
  await p.waitForTimeout(500);
  for (const id of ['browserbtn', 'audiobtn', 'keysbtn', 'browserbtn'])
    if (!errors.length && await p.$('#' + id)) await p.click('#' + id, {timeout: 3000});
  await b.close();
  if (errors.length) { console.error('page script errors:\n  ' + errors.join('\n  ')); process.exit(1); }
  console.log('page check: no script errors');
})();
