import assert from 'node:assert/strict';
import { mkdir } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { chromium } from 'playwright';
const artifacts = process.env.HEARTHLINE_ARTIFACTS ?? tmpdir();
await mkdir(artifacts,{recursive:true});
const executablePath = process.env.HEARTHLINE_CHROMIUM || undefined;
const browser = await chromium.launch({headless:true,executablePath});
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
    await page.locator('#canvas').screenshot({path:join(artifacts,'hearthline-'+name+'.png')});
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
    await button(106);
    await contains('Saved expedition');
    // Browser storage must survive a real reload.
    await page.reload({waitUntil:'domcontentloaded'});
    await page.locator('#loading.hidden').waitFor({state:'attached',timeout:30000});
    await contains('pos 20,64');
    await button(108);
    await contains('Loaded expedition');
    await contains('pos 23,64');
    await contains('wood 4');
    await contains('PAUSED');
    await button(100);
    assert.match(await status.textContent(),/pos 23,64/);
    await button(27);
    await button(100);
    await contains('pos 24,64');
    await button(1073741886);
    await contains('pos 20,64');
    await contains('wood 0');
    await button(109);
    await contains('MAP - M');
    await button(100);
    assert.match(await status.textContent(),/pos 20,64/);
    await page.locator('#canvas').screenshot({path:join(artifacts,'hearthline-map-'+name+'.png')});
    await button(109);
    await page.waitForFunction(()=>!document.querySelector('#status')?.textContent?.includes('MAP - M'));
    if(!hasTouch) {
      await page.locator('#canvas').focus();
      const press=async(key,count=1)=>{for(let i=0;i<count;i++) await page.keyboard.press(key);};
      // Explore a side trail and claim the same cache exactly once.
      await press('d',17);
      await press('w',6);
      await contains('pos 37,58');
      await contains("E open Forester's stash");
      await press('e');
      await contains('caches 1/4');
      await contains('wood 4');
      await press('e');
      await contains('caches 1/4');
      assert.match(await status.textContent(),/wood 4/);
      await button(1073741886);
      await contains('pos 20,64');
      await contains('caches 0/4');
      await page.locator('#canvas').focus();
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
      await press('d',21);
      await contains('pos 82,64');
      // Let the real-time guardian approach; do not advance simulation with fake time.
      await page.waitForFunction(()=> {
        const txt=document.querySelector('#status')?.textContent??'';
        const match=txt.match(/guard (\d+),(\d+)/);
        return match && Math.abs(Number(match[1])-82)+Math.abs(Number(match[2])-64)<=2;
      },null,{timeout:10000});
      // Headless Chromium does not reliably blur when changing tabs; send
      // the same browser notifications directly while combat is active.
      const focusLoss = async (notify, restore) => {
        await page.evaluate(notify);
        await contains('PAUSED');
        const frozen = await status.textContent();
        await page.waitForTimeout(400);
        assert.equal(await status.textContent(),frozen);
        await page.evaluate(restore);
        await page.waitForTimeout(100);
        assert.equal(await status.textContent(),frozen);
        await press('Escape');
        await page.waitForFunction(()=>!document.querySelector('#status').textContent.includes('PAUSED'));
      };
      await focusLoss(
        ()=>window.dispatchEvent(new Event('blur')),
        ()=>window.dispatchEvent(new Event('focus'))
      );
      await focusLoss(
        ()=>{
          Object.defineProperty(document,'hidden',{configurable:true,value:true});
          document.dispatchEvent(new Event('visibilitychange'));
        },
        ()=>{
          delete document.hidden;
          document.dispatchEvent(new Event('visibilitychange'));
        }
      );
      for(let hp=2;hp>=0;hp--) {
        await contains('attack cd 0');
        await press('k');
        await contains('guard HP '+hp);
      }
      await contains('Place P/Q');
      await press('d',22);
      await contains('pos 104,64');
      await press('p'); await press('2');
      await press('d',4);
      await press('q'); await press('e'); // wrong orientation cannot activate
      await contains('energy 0');
      assert.match(await status.textContent(),/Place P\/Q/);
      await press('2'); await press('e');
      await contains('Build F campfire');
      await press('f'); await press('r');
      await press('Space',9); await press('r'); await press('Space',15);
      await contains('VICTORY');
      await page.locator('#canvas').screenshot({path:join(artifacts,'hearthline-victory.png')});
    }
    const box=await page.locator('#canvas').boundingBox();
    assert(box && box.width>50 && box.height>50);
    assert.deepEqual(errors,[]);
    console.log('PASS '+name+' Chromium: boot, inputs, pause, save/reload, reset, map'+(hasTouch?'':', focus loss, combat and victory'));
  } catch(e) {
    failures++; console.error('FAIL '+name+': '+(e.stack ?? e));
    try { await page.screenshot({path:join(artifacts,'hearthline-'+name+'-failure.png'),fullPage:true}); } catch {}
  } finally { await page.close(); }
}
await check('desktop',{width:1280,height:900},false);
await check('mobile',{width:390,height:844},true);
await browser.close();
if(failures) process.exit(1);
