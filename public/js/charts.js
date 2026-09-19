/** Lightweight canvas charts — no external CDN */

const COLORS = {
  temperature: '#f0b429',
  humidity: '#3db8f0',
  pressure: '#a78bfa',
  grid: '#243044',
  text: '#8b9bb4',
  ink: '#e8eef5',
  bg: '#0b1220',
};

function dprCanvas(canvas) {
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const cssW = Math.max(canvas.clientWidth || rect.width || 800, 320);
  const cssH = Number(canvas.getAttribute('height')) || 200;
  canvas.style.height = cssH + 'px';
  canvas.width = Math.floor(cssW * dpr);
  canvas.height = Math.floor(cssH * dpr);
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, width: cssW, height: cssH };
}

function niceBounds(min, max, padRatio = 0.08) {
  if (!Number.isFinite(min) || !Number.isFinite(max)) return { min: 0, max: 1 };
  if (min === max) {
    const p = Math.abs(min) * 0.05 || 1;
    return { min: min - p, max: max + p };
  }
  const span = max - min;
  const pad = span * padRatio;
  return { min: min - pad, max: max + pad };
}

function formatTick(v) {
  if (Math.abs(v) >= 1000) return v.toFixed(0);
  if (Math.abs(v) >= 100) return v.toFixed(1);
  return v.toFixed(2).replace(/\.?0+$/, '');
}

function formatTs(ts) {
  const d = new Date(ts);
  return d.toLocaleString(undefined, {
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  });
}

/**
 * @param {HTMLCanvasElement} canvas
 * @param {Array<{ts:string, temperature:number, humidity:number, pressure:number}>} points
 * @param {{ series?: string[], showAxes?: Record<string,boolean> }} opts
 */
export function drawCombined(canvas, points, opts = {}) {
  const seriesList = opts.series && opts.series.length ? opts.series : ['temperature', 'humidity', 'pressure'];
  const { ctx, width, height } = dprCanvas(canvas);
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = COLORS.bg;
  ctx.fillRect(0, 0, width, height);

  if (!points.length) return;

  const padL = 52;
  const padR = 56;
  const padT = 16;
  const padB = 36;
  const plotW = width - padL - padR;
  const plotH = height - padT - padB;

  const xs = points.map((p) => Date.parse(p.ts));
  const xMin = Math.min(...xs);
  const xMax = Math.max(...xs);
  const xSpan = xMax - xMin || 1;

  // Left axis: temp+humidity; Right axis: pressure
  const tVals = points.map((p) => p.temperature).filter(Number.isFinite);
  const hVals = points.map((p) => p.humidity).filter(Number.isFinite);
  const pVals = points.map((p) => p.pressure).filter(Number.isFinite);
  const leftVals = [...tVals, ...hVals];
  const left = niceBounds(Math.min(...leftVals), Math.max(...leftVals));
  const right = niceBounds(Math.min(...pVals), Math.max(...pVals));

  // Grid
  ctx.strokeStyle = COLORS.grid;
  ctx.lineWidth = 1;
  const gridLines = 4;
  for (let i = 0; i <= gridLines; i++) {
    const y = padT + (plotH * i) / gridLines;
    ctx.beginPath();
    ctx.moveTo(padL, y);
    ctx.lineTo(padL + plotW, y);
    ctx.stroke();
  }

  // Axis labels
  ctx.fillStyle = COLORS.text;
  ctx.font = '11px system-ui, sans-serif';
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  for (let i = 0; i <= gridLines; i++) {
    const y = padT + (plotH * i) / gridLines;
    const lv = left.max - ((left.max - left.min) * i) / gridLines;
    const rv = right.max - ((right.max - right.min) * i) / gridLines;
    ctx.fillText(formatTick(lv), padL - 8, y);
    ctx.textAlign = 'left';
    ctx.fillText(formatTick(rv), padL + plotW + 8, y);
    ctx.textAlign = 'right';
  }

  // X labels
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  const xTicks = 5;
  for (let i = 0; i <= xTicks; i++) {
    const ratio = i / xTicks;
    const x = padL + plotW * ratio;
    const ts = new Date(xMin + xSpan * ratio).toISOString();
    ctx.fillText(formatTs(ts), x, padT + plotH + 10);
  }

  function drawSeries(key, bounds) {
    ctx.strokeStyle = COLORS[key];
    ctx.lineWidth = 1.8;
    ctx.beginPath();
    points.forEach((p, i) => {
      const x = padL + ((Date.parse(p.ts) - xMin) / xSpan) * plotW;
      const y =
        padT + plotH - ((p[key] - bounds.min) / (bounds.max - bounds.min || 1)) * plotH;
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  if (seriesList.includes('temperature')) drawSeries('temperature', left);
  if (seriesList.includes('humidity')) drawSeries('humidity', left);
  if (seriesList.includes('pressure')) drawSeries('pressure', right);

  // Frame
  ctx.strokeStyle = COLORS.grid;
  ctx.strokeRect(padL, padT, plotW, plotH);
}

/**
 * Single-series chart
 */
export function drawSeries(canvas, points, key) {
  const { ctx, width, height } = dprCanvas(canvas);
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = COLORS.bg;
  ctx.fillRect(0, 0, width, height);

  if (!points.length) return;

  const padL = 48;
  const padR = 16;
  const padT = 12;
  const padB = 28;
  const plotW = width - padL - padR;
  const plotH = height - padT - padB;

  const xs = points.map((p) => Date.parse(p.ts));
  const xMin = Math.min(...xs);
  const xMax = Math.max(...xs);
  const xSpan = xMax - xMin || 1;
  const vals = points.map((p) => p[key]).filter(Number.isFinite);
  const bounds = niceBounds(Math.min(...vals), Math.max(...vals));

  ctx.strokeStyle = COLORS.grid;
  ctx.lineWidth = 1;
  const gridLines = 3;
  for (let i = 0; i <= gridLines; i++) {
    const y = padT + (plotH * i) / gridLines;
    ctx.beginPath();
    ctx.moveTo(padL, y);
    ctx.lineTo(padL + plotW, y);
    ctx.stroke();
    const lv = bounds.max - ((bounds.max - bounds.min) * i) / gridLines;
    ctx.fillStyle = COLORS.text;
    ctx.font = '11px system-ui, sans-serif';
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ctx.fillText(formatTick(lv), padL - 6, y);
  }

  ctx.strokeStyle = COLORS[key];
  ctx.lineWidth = 1.8;
  ctx.beginPath();
  points.forEach((p, i) => {
    const x = padL + ((Date.parse(p.ts) - xMin) / xSpan) * plotW;
    const y = padT + plotH - ((p[key] - bounds.min) / (bounds.max - bounds.min || 1)) * plotH;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();

  ctx.fillStyle = COLORS.text;
  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  ctx.fillText(formatTs(new Date(xMin).toISOString()), padL + 8, padT + plotH + 8);
  ctx.fillText(formatTs(new Date(xMax).toISOString()), padL + plotW - 8, padT + plotH + 8);

  ctx.strokeStyle = COLORS.grid;
  ctx.strokeRect(padL, padT, plotW, plotH);
}

export function drawAll(combinedCanvas, splitCanvases, points, view, seriesSet) {
  if (view === 'split') {
    drawSeries(splitCanvases.t, points, 'temperature');
    drawSeries(splitCanvases.h, points, 'humidity');
    drawSeries(splitCanvases.p, points, 'pressure');
  } else {
    drawCombined(combinedCanvas, points, { series: [...seriesSet] });
  }
}
