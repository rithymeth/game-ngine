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
    async function button(key){const b=page.locator('[data-key="'+key+'"]'); if(hasTouch) await b.tap(); else await b.click();}
    await button(103);
    await contains('wood 4');
    await button(100);
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
    await button(1073741886);
    await contains('pos 20,64');
    await contains('wood 0');
    if(!hasTouch) {
      await page.locator('#canvas').focus();
      const press=async(key,count=1)=>{for(let i=0;i<count;i++) await page.keyboard.press(key);};
      for(let i=0;i<6;i++) {
        if(i) await press('d');
        await press('g');
      }
      await contains('wood 12');
      await contains('energy 2');
      await press('b'); // cannot build remotely
      await contains('wood 12');
      await press('d',36);
      await contains('pos 61,64');
      await press('b');
      await contains('wood 8');
      await press('d',26);
      await contains('pos 87,64');
      for(let i=0;i<3;i++){await press('v');await press('k');}
      await contains('Place P/Q');
      await press('d',17);
      await contains('pos 104,64');
      await press('p'); await press('2');
      await press('d',4);
      await press('q'); await press('e'); // wrong orientation cannot activate
      await contains('energy 0');
      assert.match(await status.textContent(),/Place P\/Q/);
      await press('2'); await press('e');
      await contains('Build F campfire');
      await press('f'); await press('r');
      await press('v',9); await press('r'); await press('v',15);
      await contains('VICTORY');
      await page.locator('#canvas').screenshot({path:'/tmp/hearthline-victory.png'});
    }
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
