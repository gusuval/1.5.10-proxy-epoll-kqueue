// Genera docs/presentacion/proxy-l7.pptx
//   npm i pptxgenjs@3 && node docs/presentacion/generar.mjs
// Las cifras del coste se rellenan en COSTE (salida de /cost de Claude Code).
import { createRequire } from 'node:module';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(process.env.PPTX_MODULES ? path.join(process.env.PPTX_MODULES, 'x.js') : import.meta.url);
const PptxGenJS = require('pptxgenjs');
const here = path.dirname(fileURLToPath(import.meta.url));

// ---- datos -----------------------------------------------------------
const COSTE = {
  // null = pendiente de la salida de /cost
  usd: process.env.COSTE_USD || null,
  tokens_in: process.env.COSTE_TOKENS_IN || null,
  tokens_out: process.env.COSTE_TOKENS_OUT || null,
  duracion_api: process.env.COSTE_DURACION_API || null,
};
const BENCH = [
  { esc: 'HTTPS c100', rps: 223705, p99: '1,29 ms' },
  { esc: 'HTTPS c200', rps: 213565, p99: '2,37 ms' },
  { esc: 'HTTPS c400', rps: 172075, p99: '4,78 ms' },
  { esc: 'HTTP c200', rps: 337904, p99: '1,80 ms' },
];

// ---- estilo ----------------------------------------------------------
const C = { ink: '1D2330', muted: '5B6475', rule: 'D9DEE7', accent: '0B6E69', soft: 'E6F2F1', warn: '9A5B00', warnSoft: 'FFF4E0', bg: 'FFFFFF', dark: '0F2A2E' };
const FONT = 'Calibri';

const pptx = new PptxGenJS();
pptx.layout = 'LAYOUT_WIDE'; // 13.33 x 7.5 in
pptx.title = 'Proxy inverso L7 en C11 — epoll/kqueue';
pptx.author = 'Gustavo Uval';

pptx.defineSlideMaster({
  title: 'BASE',
  background: { color: C.bg },
  objects: [
    { rect: { x: 0, y: 0, w: 0.12, h: 7.5, fill: { color: C.accent } } },
    { text: { text: 'Proxy L7 · C11 · epoll/kqueue', options: { x: 0.6, y: 7.0, w: 6, h: 0.3, fontFace: FONT, fontSize: 10, color: C.muted } } },
  ],
  slideNumber: { x: 12.3, y: 7.0, w: 0.6, h: 0.3, fontFace: FONT, fontSize: 10, color: C.muted, align: 'right' },
});

function titled(title, kicker) {
  const s = pptx.addSlide({ masterName: 'BASE' });
  if (kicker) s.addText(kicker.toUpperCase(), { x: 0.6, y: 0.35, w: 12, h: 0.35, fontFace: FONT, fontSize: 12, bold: true, color: C.accent, charSpacing: 2 });
  s.addText(title, { x: 0.6, y: 0.65, w: 12.1, h: 0.8, fontFace: FONT, fontSize: 30, bold: true, color: C.ink });
  return s;
}

function bullets(s, items, box) {
  const runs = [];
  items.forEach((it, i) => {
    const [head, body] = Array.isArray(it) ? it : [it, null];
    runs.push({ text: head, options: { bold: !!body, bullet: { code: '25A0' }, color: C.ink, breakLine: !body } });
    if (body) runs.push({ text: ' — ' + body, options: { color: C.muted, breakLine: true } });
    if (i < items.length - 1) runs.push({ text: '', options: { fontSize: 6, breakLine: true } });
  });
  s.addText(runs, { fontFace: FONT, fontSize: 17, valign: 'top', paraSpaceAfter: 2, ...box });
}

function card(s, x, y, w, h, title, lines, opts = {}) {
  s.addShape(pptx.ShapeType.roundRect, { x, y, w, h, rectRadius: 0.08, fill: { color: opts.fill || C.soft }, line: { color: opts.line || C.accent, width: 1 } });
  s.addText(title, { x: x + 0.2, y: y + 0.12, w: w - 0.4, h: 0.45, fontFace: FONT, fontSize: 16, bold: true, color: opts.line || C.accent });
  s.addText(lines.map((t) => ({ text: t, options: { bullet: { code: '2022' }, breakLine: true } })),
    { x: x + 0.2, y: y + 0.58, w: w - 0.4, h: h - 0.7, fontFace: FONT, fontSize: 13.5, color: C.ink, valign: 'top', paraSpaceAfter: 3 });
}

function kpi(s, x, y, w, value, label, color = C.accent) {
  s.addShape(pptx.ShapeType.rect, { x, y, w, h: 1.35, fill: { color: 'FFFFFF' }, line: { color: C.rule, width: 1 } });
  s.addShape(pptx.ShapeType.rect, { x, y, w, h: 0.08, fill: { color }, line: { color, width: 0 } });
  s.addText(value, { x: x + 0.15, y: y + 0.18, w: w - 0.3, h: 0.65, fontFace: FONT, fontSize: 28, bold: true, color: C.ink });
  s.addText(label, { x: x + 0.15, y: y + 0.8, w: w - 0.3, h: 0.52, fontFace: FONT, fontSize: 12, color: C.muted, valign: 'top' });
}

const fmt = (n) => n.toLocaleString('es-ES');

// ---- 1. portada ------------------------------------------------------
{
  const s = pptx.addSlide();
  s.background = { color: C.dark };
  s.addShape(pptx.ShapeType.rect, { x: 0.8, y: 2.2, w: 0.12, h: 2.3, fill: { color: '3FB8AF' }, line: { color: '3FB8AF', width: 0 } });
  s.addText('PROYECTO DE SISTEMAS', { x: 1.15, y: 2.15, w: 11, h: 0.4, fontFace: FONT, fontSize: 14, bold: true, color: '3FB8AF', charSpacing: 3 });
  s.addText('Proxy inverso L7 de alto rendimiento', { x: 1.15, y: 2.6, w: 11.5, h: 0.9, fontFace: FONT, fontSize: 40, bold: true, color: 'FFFFFF' });
  s.addText('C11 sin frameworks · epoll / kqueue · TLS con SNI · recarga en caliente', { x: 1.15, y: 3.5, w: 11.5, h: 0.6, fontFace: FONT, fontSize: 20, color: 'C9D6D8' });
  s.addText('Decisiones técnicas · Benchmark · Coste de generarlo', { x: 1.15, y: 4.05, w: 11.5, h: 0.5, fontFace: FONT, fontSize: 16, color: '8FA7AB' });
  s.addText('Gustavo Uval · 26 de septiembre de 2026 · MR !1 (feat/proxy-l7)', { x: 1.15, y: 6.5, w: 11.5, h: 0.4, fontFace: FONT, fontSize: 13, color: '8FA7AB' });
}

// ---- 2. el reto ------------------------------------------------------
{
  const s = titled('De 7 líneas de requisitos a una especificación verificable', 'El reto');
  card(s, 0.6, 1.7, 5.9, 4.9, 'Lo que pedía el enunciado', [
    'Proxy nivel 7 en C, epoll / kqueue / IOCP',
    'Redirección por nombre de dominio',
    'Varias entradas (puertos), n salidas por dominio',
    'Round robin o balanceo por indicadores de carga',
    'Keep-alive HTTP/1.1 · recarga automática',
    'Proyecto con Meson, tests con dominios reales',
    'Benchmark de 50.000 peticiones por segundo',
  ], { fill: 'F4F6F9', line: C.muted });
  card(s, 6.85, 1.7, 5.9, 4.9, 'Lo que se acordó (12 preguntas)', [
    'IOCP fuera de alcance; Linux + macOS/BSD',
    'TLS obligatorio con SNI, y 50k req/s sobre HTTPS',
    'Carga informada por el backend: X-Backend-Load',
    'Recarga vigilando el fichero y por SIGHUP',
    'Keep-alive en ambos lados + pipelining, chunked, WebSocket',
    'Dominios con curl --resolve (sin sudo)',
    'Resultado: SPEC.md + 64 requerimientos (54 RF, 10 RNF)',
  ]);
}

// ---- 3. arquitectura -------------------------------------------------
{
  const s = titled('Master + N workers, un event loop por worker', 'Arquitectura');
  const box = (x, y, w, h, t, sub, fill = C.soft, line = C.accent) => {
    s.addShape(pptx.ShapeType.roundRect, { x, y, w, h, rectRadius: 0.08, fill: { color: fill }, line: { color: line, width: 1.25 } });
    s.addText([{ text: t, options: { bold: true, breakLine: true, fontSize: 15 } }, { text: sub, options: { fontSize: 12, color: C.muted } }],
      { x, y, w, h, fontFace: FONT, color: C.ink, align: 'center', valign: 'middle' });
  };
  const arrow = (x1, y1, x2, y2, color = C.muted) => s.addShape(pptx.ShapeType.line, { x: Math.min(x1, x2), y: Math.min(y1, y2), w: Math.abs(x2 - x1) || 0.001, h: Math.abs(y2 - y1) || 0.001, flipV: y2 < y1, line: { color, width: 1.5, endArrowType: 'triangle' } });
  box(0.7, 3.05, 1.8, 1.1, 'Clientes', 'HTTP / HTTPS', 'FFFFFF', C.muted);
  box(3.3, 1.65, 5.2, 1.05, 'Master', 'valida config · vigila fichero · SIGHUP · relanza workers · stats');
  box(3.3, 3.0, 2.45, 1.7, 'Worker 0', 'event loop (1 hilo)\n+ hilo de health\n+ hilo de log');
  box(6.05, 3.0, 2.45, 1.7, 'Worker N-1', 'event loop (1 hilo)\n+ hilo de health\n+ hilo de log');
  box(3.3, 5.1, 5.2, 0.85, 'Memoria compartida (mmap)', 'contadores atómicos + tabla de servidores (seqlock)', 'F4F6F9', C.muted);
  box(9.6, 2.3, 2.4, 0.75, 'backend A', '', 'FFFFFF', C.muted);
  box(9.6, 3.45, 2.4, 0.75, 'backend B', '', 'FFFFFF', C.muted);
  box(9.6, 4.6, 2.4, 0.75, 'backend C', '', 'FFFFFF', C.muted);
  arrow(2.5, 3.6, 3.3, 3.6);
  arrow(8.5, 3.5, 9.6, 2.7); arrow(8.5, 3.85, 9.6, 3.85); arrow(8.5, 4.2, 9.6, 4.95);
  arrow(5.9, 2.7, 4.5, 3.0, C.accent); arrow(5.9, 2.7, 7.3, 3.0, C.accent);
  s.addText('SO_REUSEPORT', { x: 2.3, y: 3.15, w: 1.2, h: 0.3, fontFace: FONT, fontSize: 10, color: C.muted, align: 'center' });
  s.addText('pool keep-alive', { x: 8.45, y: 2.55, w: 1.3, h: 0.3, fontFace: FONT, fontSize: 10, color: C.muted });
  s.addText('Aislamiento: un fallo en un worker no tumba el proxy; el master lo relanza. En macOS/BSD el master pasa los sockets a los workers por SCM_RIGHTS.',
    { x: 0.6, y: 6.2, w: 12.1, h: 0.6, fontFace: FONT, fontSize: 14, color: C.muted, italic: true });
}

// ---- 4-6. decisiones -------------------------------------------------
{
  const s = titled('Event loop: edge-triggered sin perder eventos', 'Decisiones técnicas · 1/3');
  card(s, 0.6, 1.7, 3.95, 4.95, 'Un registro por fd', [
    'EPOLLET / EV_CLEAR con lectura + escritura, una sola vez',
    'Flags rd/wr: "¿el último intento dio EAGAIN?"',
    'Sin epoll_ctl(MOD) por cambio de estado',
  ]);
  card(s, 4.7, 1.7, 3.95, 4.95, 'sess_drive()', [
    'leer → procesar → E/S backend → procesar → escribir',
    'Se repite mientras haya progreso: nunca queda trabajo sin evento',
    'Presupuesto de 32 vueltas y replanificación: nadie acapara el loop',
  ]);
  card(s, 8.8, 1.7, 3.95, 4.95, 'Timers y memoria', [
    'Min-heap perezoso: refrescar un timeout cuesta O(1)',
    'Liberación diferida: sin use-after-free con eventos del mismo lote',
    'Pool mmap de buffers de 16 KB: 0 buffers por conexión ociosa',
  ]);
}
{
  const s = titled('HTTP/1.1 correcto por construcción', 'Decisiones técnicas · 2/3');
  bullets(s, [
    ['Parser por cabecera completa', 'busca \\r\\n\\r\\n recordando lo escaneado; tolera cualquier fragmentación, sin copias'],
    ['Cuerpos en streaming', '4 buffers de 16 KB por conexión; la contrapresión es natural (10 MB de subida → 13 MB RSS)'],
    ['Anti request smuggling', 'rechaza CL+TE, CL duplicados y obs-fold; nunca quita el encuadre aunque lo pida Connection'],
    ['Pipelining en serie', 'orden de respuestas garantizado sin colas; cada petición puede ir a otro backend'],
    ['Reintento acotado', 'solo si la petición es idempotente, sin cuerpo, en una conexión reutilizada y sin un byte de respuesta. Un POST nunca se reintenta'],
    ['TLS con SNI', 'un SSL_CTX por certificado; wildcard de una etiqueta (RFC 6125); 421 si SNI ≠ Host'],
  ], { x: 0.6, y: 1.65, w: 12.1, h: 5.2 });
}
{
  const s = titled('Balanceo, salud y recarga sin cortes', 'Decisiones técnicas · 3/3');
  card(s, 0.6, 1.7, 3.95, 4.95, 'least_load', [
    'X-Backend-Load (0-1) en cada respuesta, eliminada hacia el cliente',
    'EMA α=0,3 y caducidad de 5 s',
    'Power of two choices: evita que todos los workers ataquen al mismo servidor',
  ]);
  card(s, 4.7, 1.7, 3.95, 4.95, 'Salud', [
    'Pasiva: fallos de connect/reset',
    'Activa: sondas TCP/HTTP en un hilo por generación',
    'Estado atómico; sin health activo, reintento half-open',
  ]);
  card(s, 8.8, 1.7, 3.95, 4.95, 'Recarga (RCU)', [
    'inotify sobre el directorio: cubre vim y sed -i',
    'El master valida y envía el texto a los workers',
    'Generaciones con refcount: las peticiones en vuelo terminan con la config vieja',
  ]);
}

// ---- 7. calidad ------------------------------------------------------
{
  const s = titled('Verificado de extremo a extremo', 'Calidad');
  kpi(s, 0.6, 1.75, 2.85, '31', 'tests unitarios (cmocka)');
  kpi(s, 3.65, 1.75, 2.85, '96', 'comprobaciones de integración');
  kpi(s, 6.7, 1.75, 2.85, '0', 'informes de ASan / UBSan / LSan');
  kpi(s, 9.75, 1.75, 2.95, '0', 'warnings (-Wall -Wextra -Wpedantic)');
  s.addText('Bugs reales que encontraron los tests', { x: 0.6, y: 3.4, w: 12, h: 0.45, fontFace: FONT, fontSize: 18, bold: true, color: C.ink });
  bullets(s, [
    ['Reloj del event loop atrasado', 'tras un rato ocioso, los deadlines nacían vencidos → 408/504 espurios. Corregido, con test de regresión'],
    ['least_load inestable', 'desempate aleatorio; ahora se explora primero el servidor sin carga conocida'],
    ['Backend de pruebas', 'corrompía cuerpos grandes al rebobinar el buffer'],
  ], { x: 0.6, y: 3.85, w: 12.1, h: 2.9 });
}

// ---- 8. benchmark ----------------------------------------------------
{
  const s = titled('Meta de 50.000 req/s HTTPS: superada ×3,4 – ×4,5', 'Benchmark');
  s.addChart(pptx.ChartType.bar, [{ name: 'req/s', labels: BENCH.map((b) => b.esc), values: BENCH.map((b) => b.rps) }], {
    x: 0.5, y: 1.6, w: 7.6, h: 5.2, barDir: 'bar', chartColors: [C.accent],
    catAxisLabelFontFace: FONT, catAxisLabelFontSize: 13, catAxisOrientation: 'maxMin',
    valAxisLabelFontFace: FONT, valAxisLabelFontSize: 11, valAxisLabelFormatCode: '#,##0', valAxisMaxVal: 350000, valAxisMinVal: 0,
    valGridLine: { color: 'E9ECF1', size: 0.75 },
    showValue: true, dataLabelFontFace: FONT, dataLabelFontSize: 12, dataLabelFormatCode: '#,##0', dataLabelPosition: 'outEnd',
  });
  // línea de meta: 50k sobre el eje (área de trazado aproximada)
  const rows = [[
    { text: 'Escenario', options: { bold: true, fill: { color: C.soft } } },
    { text: 'req/s', options: { bold: true, fill: { color: C.soft }, align: 'right' } },
    { text: 'p99', options: { bold: true, fill: { color: C.soft }, align: 'right' } },
  ], ...BENCH.map((b) => [b.esc, { text: fmt(b.rps), options: { align: 'right', bold: true } }, { text: b.p99, options: { align: 'right' } }])];
  s.addTable(rows, { x: 8.4, y: 1.75, w: 4.4, colW: [1.8, 1.4, 1.2], fontFace: FONT, fontSize: 13, color: C.ink, border: { type: 'solid', color: C.rule, pt: 0.75 }, rowH: 0.42 });
  s.addText([
    { text: '0 errores', options: { bold: true, color: C.accent } },
    { text: ' en 28,5 M peticiones (todas 2xx). Solo 816 conexiones a los backends gracias al pool keep-alive.', options: { breakLine: true } },
    { text: ' ', options: { fontSize: 6, breakLine: true } },
    { text: 'Ryzen 7 7735HS (16 hilos) en WSL2 · 8 workers · release + LTO · respuesta de 100 B · 30 s por escenario · wrk, proxy y backends en la misma máquina (cota inferior).', options: { color: C.muted, fontSize: 12 } },
  ], { x: 8.4, y: 4.05, w: 4.4, h: 2.7, fontFace: FONT, fontSize: 14, color: C.ink, valign: 'top' });
}

// ---- 9. coste --------------------------------------------------------
{
  const s = titled('Coste de generarlo', 'Claude Code · Claude Opus 5.5');
  const pending = 'pendiente de /cost';
  const has = COSTE.usd !== null;
  kpi(s, 0.6, 1.75, 2.85, has ? COSTE.usd : '—', has ? 'coste total (USD)' : 'coste USD · ' + pending, has ? C.accent : C.warn);
  kpi(s, 3.65, 1.75, 2.85, COSTE.tokens_in || '—', COSTE.tokens_in ? 'tokens de entrada' : 'tokens de entrada · ' + pending, COSTE.tokens_in ? C.accent : C.warn);
  kpi(s, 6.7, 1.75, 2.85, COSTE.tokens_out || '—', COSTE.tokens_out ? 'tokens de salida' : 'tokens de salida · ' + pending, COSTE.tokens_out ? C.accent : C.warn);
  kpi(s, 9.75, 1.75, 2.95, COSTE.duracion_api || '—', COSTE.duracion_api ? 'tiempo de API' : 'tiempo de API · ' + pending, COSTE.duracion_api ? C.accent : C.warn);
  s.addText('Esfuerzo medido en la sesión', { x: 0.6, y: 3.4, w: 12, h: 0.45, fontFace: FONT, fontSize: 18, bold: true, color: C.ink });
  const rows = [
    ['Tiempo total de la sesión (spec → MR → documentación)', '≈ 1 h 50 min'],
    ['Preguntas de aclaración al usuario', '12 en 3 rondas'],
    ['Código C y scripts generados', '≈ 9.300 líneas · 38 ficheros en src/'],
    ['Documentación', 'SPEC, requerimientos, decisiones, verificación, 2 PDF, esta presentación'],
    ['Iteraciones de prueba hasta el verde', '1.ª ejecución 86/93 → 96/96; 3 bugs reales corregidos'],
    ['Intervención humana', 'instalar dependencias (sudo) y autenticarse en GitLab'],
  ].map(([a, b]) => [{ text: a }, { text: b, options: { bold: true } }]);
  s.addTable(rows, { x: 0.6, y: 3.9, w: 12.1, colW: [6.2, 5.9], fontFace: FONT, fontSize: 13.5, color: C.ink, border: { type: 'solid', color: C.rule, pt: 0.75 }, rowH: 0.43 });
}

// ---- 10. estado ------------------------------------------------------
{
  const s = titled('Estado y próximos pasos', 'Cierre');
  card(s, 0.6, 1.7, 5.9, 4.9, 'Entregado', [
    'Proxy completo y verificado en Linux',
    'MR !1 con 4 commits temáticos',
    'Documentación técnica y manual de usuario (PDF)',
    'Benchmark reproducible: bench/bench_proxy.sh',
    'Trazabilidad requerimiento → test (docs/verificacion.md)',
  ]);
  card(s, 6.85, 1.7, 5.9, 4.9, 'Pendiente / limitaciones', [
    'Probar en macOS/BSD (kqueue: solo sintaxis comprobada)',
    'Vídeo demo',
    'Cliente HTTP/1.0 recibe chunked sin convertir',
    'Cambiar el nº de workers requiere reiniciar',
    'upstream_connect sin test automático (en loopback un connect rechazado falla al instante)',
  ], { fill: C.warnSoft, line: C.warn });
}

const out = path.join(here, 'proxy-l7.pptx');
await pptx.writeFile({ fileName: out });
console.log('pptx:', out);
