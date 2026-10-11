/** 隔离认证服务器：只使用测试目录，日志写句柄持续到本进程退出。 */
import { createServer } from 'node:http';
import { mkdirSync, openSync, writeFileSync, writeSync } from 'node:fs';
import { join } from 'node:path';

/** 本进程的测试目录与响应模式，由测试程序显式传入。 */
const [directory, mode] = process.argv.slice(2);
if (!directory || !mode) throw new Error('需要测试目录与响应模式');
mkdirSync(directory, { recursive: true });
/** 保留日志 fd 至退出，覆盖真实 stdio 的持久写句柄特性；不复刻市场进程树。 */
const output = openSync(join(directory, 'dsh-market-restart-fixture.out.log'), 'a');
/** 测试服务器接受的固定假令牌，仅用于此子进程。 */
const token = 'fixture-current';
/** 测试服务器签发的假 Cookie，仅存在于测试请求中。 */
const cookie = 'dsh-auth-fixture=fixture-cookie';
/** 陈旧令牌请求计数，不保存请求内容。 */
let staleRequests = 0;
/** 当前测试监听端口，由操作系统分配。 */
let port = 0;

/** 按模式模拟兑换、首页验证和失败响应，不记录秘密值。 */
function respond(request, response) {
  const url = new URL(request.url, 'http://127.0.0.1');
  if (url.searchParams.has('token')) {
    if (url.searchParams.get('token') !== token) {
      staleRequests += 1;
      writeFileSync(join(directory, 'stale-count.txt'), String(staleRequests));
      response.writeHead(401);
      response.end('旧测试令牌');
      return;
    }
    if (mode === 'slow') return;
    if (mode === 'recovery' || mode === 'server-error') {
      response.writeHead(mode === 'server-error' ? 500 : 200, { 'content-type': 'text/html' });
      response.end('<html>测试恢复页面</html>');
      return;
    }
    const location = mode === 'foreign-location' ? 'http://example.invalid/' : '/';
    const headers = { location };
    if (mode !== 'no-cookie') headers['set-cookie'] = `${cookie}; Path=/; HttpOnly`;
    response.writeHead(303, headers);
    response.end();
    return;
  }
  if (request.headers.cookie !== cookie) {
    response.writeHead(401);
    response.end('未兑换测试 Cookie');
    return;
  }
  response.writeHead(200, {
    'content-type': mode === 'wrong-content' ? 'text/plain' : 'text/html; charset=utf-8',
  });
  response.end(mode === 'no-boot' ? '<html>普通页面</html>' : '<html><script>window.__DSH_BOOT__={}</script></html>');
}

/** 监听先就绪，延迟模式再分块追加 URL，最后才追加换行。 */
const server = createServer(respond);
server.listen(0, '127.0.0.1', () => {
  port = server.address().port;
  if (mode !== 'silent') {
    writeSync(output, `dsh web: http://127.0.0.1:${port}/?token=fixture-stale\n`);
    if (mode === 'delayed') {
      setTimeout(() => { writeSync(output, `dsh web: http://127.0.0.1:${port}/?token=`); }, 1500);
      setTimeout(() => { writeSync(output, token); }, 1800);
      setTimeout(() => { writeSync(output, '\n'); }, 2100);
    } else {
      writeSync(output, `dsh web: http://127.0.0.1:${port}/?token=${token}\n`);
    }
  }
  writeFileSync(join(directory, 'port.txt'), String(port));
});
