// 智能体 CPU 对比（Pi 真实节奏）：用 Pi 同一套 Playwright MCP 扩展模式（broker → relay → 浏览器侧扩展/桥），
// 在同一组站点上按「执行一步 → 等待（模拟模型思考）」驱动浏览器，统计浏览器整棵进程树的 CPU 时间（Job Object 记账，
// 已退出的渲染进程也算）。浏览器都用临时配置目录、窗口放在屏幕外且不激活，不碰日常配置、不打扰。
//
// 用法：
//   node agent-bench.mjs --browser jisu|thorium --exe <浏览器 exe> --jobrun <jobrun.exe>
//        --bundle <playwright-core lib/coreBundle.js> --patch <Pi background-mcp-patch.mjs>
//        [--extension <Pi playwright-extension 目录>（thorium 必需）] [--out <结果.json>]
//        [--gap 10] [--steps goto,text,snapshot,scroll] [--sites <逗号分隔网址>] [--settle 20] [--idle-tail 30]
//        [--agent-extra '<JSON>']（极速：并入 agent-bridge.json，例如 {"blockImages":true}）
//        [--gap-profile]（间隙内每秒记一次 CPU，看挂起是否生效）
//        [--gaps <逗号分隔秒数>]（每步后的等待按这串依次取，循环使用；不给则每步都等 --gap 秒）
import crypto from 'node:crypto';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import readline from 'node:readline';
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';

// 默认站点：几类常见页面（搜索、百科、新闻、视频、行情）。对比 Pi 的真实节奏时用 --sites 传入它实际访问的站点
const DEFAULT_SITES = [
  'https://cn.bing.com/',
  'https://zh.wikipedia.org/wiki/Web%E6%B5%8F%E8%A7%88%E5%99%A8',
  'https://news.sina.com.cn/',
  'https://www.bilibili.com/',
  'https://quote.eastmoney.com/sh000001.html',
];

const argv = process.argv.slice(2);
const opt = (name, fallback) => {
  const i = argv.indexOf('--' + name);
  return i >= 0 && i + 1 < argv.length ? argv[i + 1] : fallback;
};
const browser = opt('browser');
const exe = opt('exe');
const jobrun = opt('jobrun');
const bundle = opt('bundle');
const patchModule = opt('patch');
const extensionDir = opt('extension');
const gapMs = Number(opt('gap', '10')) * 1000;
const settleMs = Number(opt('settle', '20')) * 1000;
const idleTailMs = Number(opt('idle-tail', '30')) * 1000;
const steps = opt('steps', 'goto,text,snapshot,scroll').split(',');
const sites = (opt('sites') ? opt('sites').split(',') : DEFAULT_SITES).filter(Boolean);
const agentExtra = JSON.parse(opt('agent-extra', '{}'));
const gapProfile = argv.includes('--gap-profile');
const gapList = opt('gaps') ? opt('gaps').split(',').map(Number).filter((n) => n > 0).map((n) => n * 1000) : [];
let gapIndex = 0;
const nextGapMs = () => (gapList.length ? gapList[gapIndex++ % gapList.length] : gapMs);
const outFile = opt('out', path.join(os.tmpdir(), `agent-bench-${browser}-${Date.now()}.json`));
if (!['jisu', 'thorium'].includes(browser) || !exe || !jobrun || !bundle || !patchModule) {
  console.error('missing arguments; see header');
  process.exit(64);
}
if (browser === 'thorium' && !extensionDir) {
  console.error('--extension is required for thorium');
  process.exit(64);
}

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const log = (...args) => console.log(new Date().toISOString().slice(11, 19), ...args);
const work = fs.mkdtempSync(path.join(os.tmpdir(), `agent-bench-${browser}-`));
const token = crypto.randomBytes(32).toString('base64url');  // 43 位，与 Pi 配对令牌同格式

// ---------------------------------------------------------------------------
// 测试 broker：协议同 Pi 的 background-broker（/poll /ack），邀请直接在进程内排队

const tickets = new Map();
let lastPoll = 0;
const brokerServer = http.createServer(async (req, res) => {
  const reply = (status, value) => {
    res.writeHead(status, { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Headers': 'Authorization, Content-Type' });
    res.end(JSON.stringify(value));
  };
  if (req.method === 'OPTIONS') return reply(200, {});
  if (req.headers.authorization !== 'Bearer ' + token) return reply(401, { error: 'auth' });
  const url = new URL(req.url, 'http://127.0.0.1');
  let body = {};
  if (req.method === 'POST') {
    let text = '';
    for await (const chunk of req) text += chunk;
    body = JSON.parse(text || '{}');
  }
  if (url.pathname === '/health') return reply(200, { service: 'pi-background-browser', protocol: 1, lastPoll });
  if (url.pathname === '/poll') {
    lastPoll = Date.now();
    return reply(200, { tickets: [...tickets.values()].filter((t) => t.status === 'waiting').map(({ id, url }) => ({ id, url })) });
  }
  if (url.pathname === '/ack' && req.method === 'POST') {
    const t = tickets.get(body.id);
    if (!t) return reply(404, { error: 'expired' });
    t.status = body.success ? 'opened' : 'failed';
    t.error = body.error;
    t.tabId = body.tabId;
    return reply(200, { ok: true });
  }
  reply(404, { error: 'route' });
});
await new Promise((resolve) => brokerServer.listen(0, '127.0.0.1', resolve));
const brokerPort = brokerServer.address().port;
log('broker', brokerPort, 'work', work);

// ---------------------------------------------------------------------------
// 浏览器：在 jobrun 里启动，CPU 按整棵进程树记账

const screen = JSON.parse(await new Promise((resolve, reject) => {
  const p = spawn(jobrun, ['--screen']);
  let out = '';
  p.stdout.on('data', (d) => (out += d));
  p.on('exit', () => resolve(out.trim()));
  p.on('error', reject);
}));
const offX = screen.left - 3000;
const offY = screen.top + 50;
let browserArgs;
if (browser === 'jisu') {
  const profile = path.join(work, 'User Data');
  const tokenFile = path.join(work, 'pairing.json');
  fs.writeFileSync(tokenFile, JSON.stringify({ token }));
  const config = path.join(work, 'agent-bridge.json');
  fs.writeFileSync(config, JSON.stringify({ broker: `http://127.0.0.1:${brokerPort}`, tokenFile, pollMs: 1000, ...agentExtra }));
  browserArgs = ['--udf', profile, '--agent-config', config, '--bounds', `${offX},${offY},1280,860`, '--no-activate'];
} else {
  // Pi Background 扩展的副本：broker 地址改到测试端口，写入测试配对令牌
  const ext = path.join(work, 'extension');
  fs.cpSync(extensionDir, ext, { recursive: true, filter: (src) => !/\.bak-/.test(src) });
  const configFile = path.join(ext, 'pi-background-config.mjs');
  fs.writeFileSync(configFile, fs.readFileSync(configFile, 'utf8').replace(/http:\/\/127\.0\.0\.1:\d+/, `http://127.0.0.1:${brokerPort}`));
  fs.writeFileSync(path.join(ext, 'pi-background-pairing.json'), JSON.stringify({ token }));
  const profile = path.join(work, 'profile');
  browserArgs = [`--user-data-dir=${profile}`, `--load-extension=${ext}`,
    '--silent-debugger-extension-api', '--no-first-run', '--no-default-browser-check',
    `--window-position=${offX},${offY}`, '--window-size=1280,860', 'about:blank'];
}

const job = spawn(jobrun, [exe, ...browserArgs], { stdio: ['pipe', 'pipe', 'inherit'] });
const jobLines = readline.createInterface({ input: job.stdout });
const waiters = [];
jobLines.on('line', (line) => {
  try {
    const value = JSON.parse(line);
    const waiter = waiters.shift();
    if (waiter) waiter(value);
  } catch {}
});
const nextLine = () => new Promise((resolve) => waiters.push(resolve));
const started = await nextLine();
log('browser pid', started.pid);
const marks = [];
async function mark(label) {
  job.stdin.write(`mark ${label}\n`);
  const value = await nextLine();
  marks.push(value);
  return value;
}

// 等浏览器侧开始轮询 broker（扩展 / 桥上线）
const onlineDeadline = Date.now() + 60000;
while (Date.now() - lastPoll > 3000) {
  if (Date.now() > onlineDeadline) throw new Error('browser side never polled the test broker');
  await sleep(500);
}
log('browser side online');

// ---------------------------------------------------------------------------
// Pi 的 Playwright MCP（扩展模式，打 Pi 同样的两个补丁），进程内直接调工具

process.env.PLAYWRIGHT_MCP_EXTENSION_TOKEN = token;
process.env.PLAYWRIGHT_MCP_EXECUTABLE_PATH = exe;
process.env.DEBUG = '';
globalThis.__piOpenBackgroundExtension = async (href) => {
  const id = crypto.randomUUID();
  tickets.set(id, { id, url: href, status: 'waiting' });
  for (let i = 0; i < 100; i++) {
    await sleep(300);
    const t = tickets.get(id);
    if (t.status === 'opened') return;
    if (t.status === 'failed') throw new Error('extension rejected: ' + t.error);
  }
  throw new Error('extension did not accept the invitation');
};
const { patchBackgroundBootstrap, patchBackgroundFocusEmulation } = await import(pathToFileURL(patchModule).href);
const source = patchBackgroundFocusEmulation(patchBackgroundBootstrap(fs.readFileSync(bundle, 'utf8')));
const mod = { exports: {} };
new Function('require', 'module', 'exports', '__filename', '__dirname', source)(createRequire(bundle), mod, mod.exports, bundle, path.dirname(bundle));
const outputDir = path.join(work, 'mcp-output');
fs.mkdirSync(outputDir, { recursive: true });
const server = await mod.exports.tools.createConnection({ extension: true, sharedBrowserContext: true, outputDir });
const pending = new Map();
let sequence = 0;
const transport = {
  async start() {},
  async send(message) {
    if (message.method && message.id !== undefined) {
      this.onmessage?.({ jsonrpc: '2.0', id: message.id, ...(message.method === 'roots/list' ? { result: { roots: [] } } : { error: { code: -32601, message: 'unsupported' } }) });
      return;
    }
    const entry = pending.get(message.id);
    if (entry) {
      pending.delete(message.id);
      message.error ? entry.reject(new Error(JSON.stringify(message.error))) : entry.resolve(message.result);
    }
  },
  async close() {},
};
await server.connect(transport);
function rpc(method, params, timeoutMs = 45000) {
  return new Promise((resolve, reject) => {
    const id = ++sequence;
    const timer = setTimeout(() => { pending.delete(id); reject(new Error('timeout ' + method)); }, timeoutMs);
    pending.set(id, { resolve: (v) => { clearTimeout(timer); resolve(v); }, reject: (e) => { clearTimeout(timer); reject(e); } });
    transport.onmessage({ jsonrpc: '2.0', id, method, params });
  });
}
await rpc('initialize', { protocolVersion: '2024-11-05', capabilities: {}, clientInfo: { name: 'agent-bench', version: '1' } });
transport.onmessage({ jsonrpc: '2.0', method: 'notifications/initialized' });
const tools = new Set((await rpc('tools/list', {})).tools.map((t) => t.name));

const STEP = {
  goto: (url) => ['browser_navigate', { url }],
  text: () => ['browser_evaluate', { function: "() => ({url:location.href,title:document.title,text:(document.body?.innerText||'').slice(0,60000)})" }],
  snapshot: () => ['browser_snapshot', {}],
  scroll: () => ['browser_evaluate', { function: '() => { window.scrollBy(0, 800); return window.scrollY; }' }],
  screenshot: () => ['browser_take_screenshot', { type: 'png' }],
};

async function callTool(name, args) {
  const started = Date.now();
  try {
    const result = await rpc('tools/call', { name, arguments: args });
    const text = (result.content || []).filter((c) => c.type === 'text').map((c) => c.text).join('\n');
    return { ok: !result.isError, ms: Date.now() - started, chars: text.length, error: result.isError ? text.slice(0, 300) : undefined };
  } catch (error) {
    return { ok: false, ms: Date.now() - started, error: String(error.message).slice(0, 300) };
  }
}

// 先连上（第一次工具调用触发邀请 → 桥/扩展领票 → relay 握手），再静置让新配置目录的首启工作做完
const warm = await callTool('browser_navigate', { url: 'about:blank' });
log('connected', JSON.stringify(warm));
await sleep(settleMs);

const results = [];
const begin = await mark('begin');
for (const site of sites) {
  for (const step of steps) {
    const [name, args] = STEP[step](site);
    if (!tools.has(name)) continue;
    const before = await mark(`${site} ${step} start`);
    const outcome = await callTool(name, args);
    const afterStep = await mark(`${site} ${step} done`);
    const profile = [];
    const thisGap = nextGapMs();
    if (gapProfile) {
      let previous = afterStep;
      for (let t = 1000; t <= thisGap; t += 1000) {
        await sleep(1000);
        const now = await mark(`${site} ${step} gap+${t / 1000}s`);
        profile.push(Math.round(now.userMs + now.kernelMs - previous.userMs - previous.kernelMs));
        previous = now;
      }
    } else {
      await sleep(thisGap);
    }
    const afterGap = await mark(`${site} ${step} gap`);
    const cpu = (a, b) => Math.round(b.userMs + b.kernelMs - a.userMs - a.kernelMs);
    const row = { site, step, gapSeconds: thisGap / 1000, ...outcome, stepCpuMs: cpu(before, afterStep), gapCpuMs: cpu(afterStep, afterGap),
      userInput: afterGap.idleMs < afterGap.ms - before.ms, ...(gapProfile ? { gapProfileMs: profile } : {}) };
    results.push(row);
    log(browser, site, step, outcome.ok ? 'ok' : 'FAIL', `${outcome.ms}ms`, `step=${row.stepCpuMs}ms gap=${row.gapCpuMs}ms`, gapProfile ? profile.join(',') : '');
  }
}
const end = await mark('end');
await sleep(idleTailMs);
const tail = await mark('idle-tail');
const total = (a, b) => b.userMs + b.kernelMs - a.userMs - a.kernelMs;
const summary = {
  browser, exe, sites: sites.length, steps, gapMs: gapList.length ? 'list' : gapMs, agentExtra,
  wallMs: end.ms - begin.ms,
  cpuMs: Math.round(total(begin, end)),
  cpuPctOfCore: Math.round((total(begin, end) / (end.ms - begin.ms)) * 1000) / 10,
  stepCpuMs: results.reduce((s, r) => s + r.stepCpuMs, 0),
  gapCpuMs: results.reduce((s, r) => s + r.gapCpuMs, 0),
  idleTailCpuPct: Math.round((total(end, tail) / (tail.ms - end.ms)) * 1000) / 10,
  failures: results.filter((r) => !r.ok).length,
  processesTotal: tail.total,
  userInputDuringRun: results.some((r) => r.userInput),
};
fs.writeFileSync(outFile, JSON.stringify({ summary, results, marks }, null, 1));
log('summary', JSON.stringify(summary));

job.stdin.write('quit\n');
await sleep(1000);
brokerServer.close();
try { await server.close?.(); } catch {}
try { fs.rmSync(work, { recursive: true, force: true, maxRetries: 5, retryDelay: 500 }); } catch {}
process.exit(0);
