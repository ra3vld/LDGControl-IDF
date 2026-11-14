// Exercise the actual TypeScript history module, not a reimplementation.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import ts from '../web/node_modules/typescript/lib/typescript.js';
const source=await readFile(new URL('../web/src/history.ts',import.meta.url),'utf8');
const compiled=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2022,module:ts.ModuleKind.ES2022}}).outputText;
const {History}=await import('data:text/javascript;base64,'+Buffer.from(compiled).toString('base64'));
const state=(trx='tx',power=0,extra={})=>({boot_id:'boot',source_token:'local',connected:true,meter_age_ms:0,fwd_power:power,radio:{provider:'tci',state:'ready',rx_tx:trx,tuning:false,age_ms:0},...extra});
const history=new History(true);history.add(state(),0);history.add(state(),1000);history.add(state('tx',20),2000);history.add(state('rx'),3000);
assert.equal(history.elapsed,3000,'TCI counts carrierless TX and RX edge');
history.add(state('rx'),10000);history.add(state(),11000);history.add(state(),12000);assert.equal(history.elapsed,4000,'RX gaps excluded');assert.notEqual(history.samples[0].segment,history.samples.at(-1).segment,'Separate TX segments never join');
history.add(state(),20000);assert.equal(history.elapsed,4000,'Missing telemetry never invents TX time');
history.add(state('rx',0,{radio:{provider:'tci',state:'ready',rx_tx:'rx',tuning:true,age_ms:0}}),21000);history.add(state('rx'),22000);assert.equal(history.elapsed,6000,'TUNE without TRX counts');
const stale=new History(true);stale.add(state('tx',0,{radio:{provider:'tci',state:'ready',rx_tx:'tx',tuning:false,age_ms:4000}}),0);stale.add(state('rx'),1000);assert.equal(stale.elapsed,0,'Stale TCI excluded');
stale.add(state('rx',50,{radio:undefined,meter_age_ms:4000}),2000);stale.add(state('rx'),3000);assert.equal(stale.elapsed,0,'Stale RF excluded');
stale.add(state('rx',50,{radio:undefined}),4000);stale.add(state('rx',0,{radio:undefined}),5000);assert.equal(stale.elapsed,1000,'RF fallback works without TCI');
stale.add(state('tx',0,{source_token:'other'}),6000);assert.equal(stale.elapsed,0);assert.equal(stale.samples.length,1,'Source change resets history');stale.add(state('tx',0,{source_token:'other'}),5000);assert.equal(stale.elapsed,0,'Clock rollback resets');
const wall=new History();wall.add(state('rx'),0);wall.add(state('rx'),1000);assert.equal(wall.samples[0].segment,wall.samples[1].segment,'Normal history RX line remains continuous');wall.add(state(),2000000);assert.equal(wall.samples.length,1,'30 minute pruning');
console.log('TX history: TCI TRX/TUNE, SSB gaps, stale observations, RF fallback, segments, reset and retention PASS');
