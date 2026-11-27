import assert from 'node:assert/strict';
import { chromium } from 'playwright';
const browser = await chromium.launch({headless:true});
const base = process.env.HEARTHLINE_URL ?? 'http://127.0.0.1:8765';
let failures=0;
async function check(name, viewport, hasTouch) {
  const page=await browser.newPage({viewport,hasTouch});
  const errors=[];
  page.on('pageerror', e=>errors.push(e.message));
  try {
    const response=await page.goto(base,{waitUntil:'domcontentloaded'});
    assert.equal(response.status(),200);
    await page.locator('#loading.hidden').waitFor({state:'attached',timeout:30000});
    const status=page.locator('#status');
    async function contains(value) { await page.waitForFunction(v=>document.querySelector('#status')?.textContent?.includes(v),value,{timeout:10000}); }
    await contains('pos 20,64');
    await page.locator('#canvas').screenshot({path:'/tmp/hearthline-'+name+'.png'});
    await page.locator('[data-key="103"]').click();
    await contains('wood 4');
    await page.locator('[data-key="100"]').click();
    await contains('pos 21,64');
    await page.locator('#canvas').focus();
    await page.keyboard.press('d');
    await contains('pos 22,64');
    await page.keyboard.press('Escape');
    await contains('PAUSED');
    await page.keyboard.press('d');
    assert.match(await status.textContent(),/pos 22,64/);
    await page.keyboard.press('Escape');
    await page.keyboard.press('d');
    await contains('pos 23,64');
    await page.locator('[data-key="1073741886"]').click();
    await contains('pos 20,64');
    await contains('wood 0');
    const box=await page.locator('#canvas').boundingBox();
    assert(box && box.width>50 && box.height>50);
    assert.deepEqual(errors,[]);
    console.log('PASS '+name+' browser boot, keyboard, touch, pause, reset');
  } catch(e) {
    failures++; console.error('FAIL '+name+': '+(e.stack ?? e));
    try { await page.screenshot({path:'/tmp/hearthline-'+name+'-failure.png',fullPage:true}); } catch {}
  } finally { await page.close(); }
}
await check('desktop',{width:1280,height:900},false);
await check('mobile',{width:390,height:844},true);
await browser.close();
if(failures) process.exit(1);
