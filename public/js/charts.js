/** Lightweight canvas charts with cursor/hover data inspection */

const COLORS = {
  temperature: '#f0b429',
  humidity: '#3db8f0',
  pressure: '#a78bfa',
  grid: '#243044',
  text: '#8b9bb4',
  ink: '#e8eef5',
  bg: '#0b1220',
  cross: 'rgba(232, 238, 245, 0.35)',
};

const SERIES_LABEL = {
  temperature: '温度',
  humidity: '湿度',
  pressure: '气压',
};

const SERIES_UNIT = {
  temperature: '°C',
  humidity: '%RH',
  pressure: 'hPa',
};

/** Default CSS heights — JS overrides inline; keep these as fallbacks only. */
const CSS_HEIGHT = {
  combined: 360,
  split: 180,
};

function viewportWidth() {
  return (typeof window !== 'undefined' && window.innerWidth) || 1024;
}

/** Adaptive chart CSS height for the current viewport width. */
export function chartCssHeight(kind) {
  const w = viewportWidth();
  if (kind === 'split') {
    if (w <= 360) return 130;
    if (w <= 480) return 140;
    if (w <= 640) return 160;
    return CSS_HEIGHT.split;
  }
  if (w <= 360) return 220;
  if (w <= 480) return 260;
  if (w <= 640) return 300;
  return CSS_HEIGHT.combined;
}

function resolveCssHeight(canvas, kind) {
  const fromStyle = parseFloat(canvas?.style?.height);
  if (Number.isFinite(fromStyle) && fromStyle > 40) return fromStyle;
  return chartCssHeight(kind);
}

function dprCanvas(canvas, cssH) {
  const rect = canvas.getBoundingClientRect();
  const dpr = (typeof globalThis !== 'undefined' && globalThis.devicePixelRatio) || 1;
  const cssW = Math.max(canvas.clientWidth || rect.width || 800, 200);
  const h = Number(cssH) || 240;
  // Inline style wins over CSS height:auto and keeps aspect stable across redraws.
  canvas.style.width = '100%';
  canvas.style.height = `${h}px`;
  canvas.style.display = 'block';
  const wPx = Math.floor(cssW * dpr);
  const hPx = Math.floor(h * dpr);
  if (canvas.width !== wPx) canvas.width = wPx;
  if (canvas.height !== hPx) canvas.height = hPx;
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  return { ctx, width: cssW, height: h };
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

export function formatTs(ts) {
  const d = new Date(ts);
  return d.toLocaleString(undefined, {
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

function formatTsShort(ts) {
  const d = new Date(ts);
  return d.toLocaleString(undefined, {
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  });
}

function plotBox(width, height, kind) {
  if (kind === 'split') {
    const padL = width < 400 ? 36 : 48;
    const padR = width < 400 ? 10 : 16;
    const padT = 10;
    const padB = width < 400 ? 22 : 28;
    return {
      padL,
      padR,
      padT,
      padB,
      plotW: Math.max(width - padL - padR, 40),
      plotH: Math.max(height - padT - padB, 30),
    };
  }
  let padL, padR, padT, padB;
  if (width < 400) {
    padL = 34; padR = 34; padT = 10; padB = 26;
  } else if (width < 640) {
    padL = 40; padR = 42; padT = 12; padB = 30;
  } else {
    padL = 52; padR = 56; padT = 16; padB = 36;
  }
  return {
    padL,
    padR,
    padT,
    padB,
    plotW: Math.max(width - padL - padR, 40),
    plotH: Math.max(height - padT - padB, 30),
  };
}

function xTickCount(width) {
  if (width < 400) return 2;
  if (width < 640) return 3;
  return 5;
}

function axisFont(width) {
  return width < 400 ? '10px system-ui, sans-serif' : '11px system-ui, sans-serif';
}

function xOf(tsMs, xMin, xSpan, padL, plotW) {
  return padL + ((tsMs - xMin) / xSpan) * plotW;
}

function yOf(val, bounds, padT, plotH) {
  return padT + plotH - ((val - bounds.min) / (bounds.max - bounds.min || 1)) * plotH;
}

/** Binary-search nearest point index by timestamp ms */
export function findNearestIndex(points, targetMs) {
  if (!points || !points.length) return -1;
  let lo = 0;
  let hi = points.length - 1;
  const times = points.map((p) => Date.parse(p.ts));
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (times[mid] < targetMs) lo = mid + 1;
    else hi = mid;
  }
  const i = lo;
  const prev = Math.max(0, i - 1);
  return Math.abs(times[prev] - targetMs) <= Math.abs(times[i] - targetMs) ? prev : i;
}

function drawCrosshair(ctx, x, box) {
  ctx.save();
  ctx.strokeStyle = COLORS.cross;
  ctx.lineWidth = 1;
  ctx.setLineDash([4, 4]);
  ctx.beginPath();
  ctx.moveTo(x, box.padT);
  ctx.lineTo(x, box.padT + box.plotH);
  ctx.stroke();
  ctx.restore();
}

function drawDot(ctx, x, y, color, r = 3.5) {
  ctx.save();
  ctx.fillStyle = color;
  ctx.strokeStyle = COLORS.bg;
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.arc(x, y, r, 0, Math.PI * 2);
  ctx.fill();
  ctx.stroke();
  ctx.restore();
}

/**
 * @param {HTMLCanvasElement} canvas
 * @param {Array} points
 * @param {{ series?: string[], cursorIndex?: number|null }} opts
 */
export function drawCombined(canvas, points, opts = {}) {
  const seriesList =
    opts.series && opts.series.length
      ? opts.series
      : ['temperature', 'humidity', 'pressure'];
  const cursorIndex = opts.cursorIndex == null ? null : opts.cursorIndex;
  const { ctx, width, height } = dprCanvas(canvas, opts.cssHeight || chartCssHeight('combined'));
  const box = plotBox(width, height, 'combined');
  const { padL, padT, plotW, plotH } = box;

  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = COLORS.bg;
  ctx.fillRect(0, 0, width, height);
  if (!points.length) return;

  const xs = points.map((p) => Date.parse(p.ts));
  const xMin = Math.min(...xs);
  const xMax = Math.max(...xs);
  const xSpan = xMax - xMin || 1;

  const tVals = points.map((p) => p.temperature).filter(Number.isFinite);
  const hVals = points.map((p) => p.humidity).filter(Number.isFinite);
  const pVals = points.map((p) => p.pressure).filter(Number.isFinite);
  const leftVals = [...tVals, ...hVals];
  const left = niceBounds(Math.min(...leftVals), Math.max(...leftVals));
  const right = niceBounds(Math.min(...pVals), Math.max(...pVals));

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

  ctx.fillStyle = COLORS.text;
  ctx.font = axisFont(width);
  ctx.textAlign = 'right';
  ctx.textBaseline = 'middle';
  for (let i = 0; i <= gridLines; i++) {
    const y = padT + (plotH * i) / gridLines;
    const lv = left.max - ((left.max - left.min) * i) / gridLines;
    const rv = right.max - ((right.max - right.min) * i) / gridLines;
    ctx.fillText(formatTick(lv), padL - 6, y);
    ctx.textAlign = 'left';
    ctx.fillText(formatTick(rv), padL + plotW + 6, y);
    ctx.textAlign = 'right';
  }

  ctx.textAlign = 'center';
  ctx.textBaseline = 'top';
  const xTicks = xTickCount(width);
  for (let i = 0; i <= xTicks; i++) {
    const ratio = i / xTicks;
    const x = padL + plotW * ratio;
    const ts = new Date(xMin + xSpan * ratio).toISOString();
    ctx.fillText(formatTsShort(ts), x, padT + plotH + 8);
  }

  function drawSeries(key, bounds) {
    ctx.strokeStyle = COLORS[key];
    ctx.lineWidth = width < 400 ? 1.5 : 1.8;
    ctx.beginPath();
    points.forEach((p, i) => {
      const x = xOf(Date.parse(p.ts), xMin, xSpan, padL, plotW);
      const y = yOf(p[key], bounds, padT, plotH);
      if (i === 0) ctx.moveTo(x, y);
      else ctx.lineTo(x, y);
    });
    ctx.stroke();
  }

  if (seriesList.includes('temperature')) drawSeries('temperature', left);
  if (seriesList.includes('humidity')) drawSeries('humidity', left);
  if (seriesList.includes('pressure')) drawSeries('pressure', right);

  ctx.strokeStyle = COLORS.grid;
  ctx.strokeRect(padL, padT, plotW, plotH);

  if (cursorIndex != null && cursorIndex >= 0 && cursorIndex < points.length) {
    const p = points[cursorIndex];
    const x = xOf(Date.parse(p.ts), xMin, xSpan, padL, plotW);
    drawCrosshair(ctx, x, box);
    if (seriesList.includes('temperature')) {
      drawDot(ctx, x, yOf(p.temperature, left, padT, plotH), COLORS.temperature);
    }
    if (seriesList.includes('humidity')) {
      drawDot(ctx, x, yOf(p.humidity, left, padT, plotH), COLORS.humidity);
    }
    if (seriesList.includes('pressure')) {
      drawDot(ctx, x, yOf(p.pressure, right, padT, plotH), COLORS.pressure);
    }
  }

  return { xMin, xMax, xSpan, box, left, right, kind: 'combined' };
}

export function drawSeriesChart(canvas, points, key, opts = {}) {
  const cursorIndex = opts.cursorIndex == null ? null : opts.cursorIndex;
  const { ctx, width, height } = dprCanvas(canvas, opts.cssHeight || chartCssHeight('split'));
  const box = plotBox(width, height, 'split');
  const { padL, padT, plotW, plotH } = box;

  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = COLORS.bg;
  ctx.fillRect(0, 0, width, height);
  if (!points.length) return;

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
    ctx.font = axisFont(width);
    ctx.textAlign = 'right';
    ctx.textBaseline = 'middle';
    ctx.fillText(formatTick(lv), padL - 4, y);
  }

  ctx.strokeStyle = COLORS[key];
  ctx.lineWidth = width < 400 ? 1.5 : 1.8;
  ctx.beginPath();
  points.forEach((p, i) => {
    const x = xOf(Date.parse(p.ts), xMin, xSpan, padL, plotW);
    const y = yOf(p[key], bounds, padT, plotH);
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();

  ctx.fillStyle = COLORS.text;
  ctx.font = axisFont(width);
  ctx.textBaseline = 'top';
  if (width < 400) {
    ctx.textAlign = 'left';
    ctx.fillText(formatTsShort(new Date(xMin).toISOString()), padL + 2, padT + plotH + 6);
    ctx.textAlign = 'right';
    ctx.fillText(formatTsShort(new Date(xMax).toISOString()), padL + plotW - 2, padT + plotH + 6);
  } else {
    ctx.textAlign = 'center';
    ctx.fillText(formatTsShort(new Date(xMin).toISOString()), padL + 8, padT + plotH + 6);
    ctx.fillText(formatTsShort(new Date(xMax).toISOString()), padL + plotW - 8, padT + plotH + 6);
  }

  ctx.strokeStyle = COLORS.grid;
  ctx.strokeRect(padL, padT, plotW, plotH);

  if (cursorIndex != null && cursorIndex >= 0 && cursorIndex < points.length) {
    const p = points[cursorIndex];
    const x = xOf(Date.parse(p.ts), xMin, xSpan, padL, plotW);
    drawCrosshair(ctx, x, box);
    drawDot(ctx, x, yOf(p[key], bounds, padT, plotH), COLORS[key]);
  }

  return { xMin, xMax, xSpan, box, bounds, kind: 'split', key };
}

export function drawAll(combinedCanvas, splitCanvases, points, view, seriesSet, cursorIndex = null) {
  if (view === 'split') {
    drawSeriesChart(splitCanvases.t, points, 'temperature', { cursorIndex });
    drawSeriesChart(splitCanvases.h, points, 'humidity', { cursorIndex });
    drawSeriesChart(splitCanvases.p, points, 'pressure', { cursorIndex });
  } else {
    drawCombined(combinedCanvas, points, { series: [...seriesSet], cursorIndex });
  }
}

/** Public payload for tooltips / readout */
export function pointAt(points, index) {
  if (!points || index == null || index < 0 || index >= points.length) return null;
  const p = points[index];
  return {
    index,
    ts: p.ts,
    label: formatTs(p.ts),
    values: [
      { key: 'temperature', label: SERIES_LABEL.temperature, unit: SERIES_UNIT.temperature, value: p.temperature, color: COLORS.temperature },
      { key: 'humidity', label: SERIES_LABEL.humidity, unit: SERIES_UNIT.humidity, value: p.humidity, color: COLORS.humidity },
      { key: 'pressure', label: SERIES_LABEL.pressure, unit: SERIES_UNIT.pressure, value: p.pressure, color: COLORS.pressure },
    ],
  };
}

/**
 * Bind hover / click / touch cursor inspection on chart canvases.
 */
export function bindChartCursor({
  combinedCanvas,
  splitCanvases,
  getPoints,
  getView,
  onChange,
}) {
  let index = null;
  let pinned = false;

  function emit() {
    onChange?.({ index, pinned });
  }

  function setIndex(i, opts = {}) {
    const pts = getPoints();
    if (!pts.length) {
      index = null;
      pinned = false;
      emit();
      return;
    }
    const next = i == null ? null : Math.max(0, Math.min(pts.length - 1, i));
    if (opts.pin != null) pinned = opts.pin;
    index = next;
    emit();
  }

  function indexFromEvent(canvas, evt, kind) {
    const pts = getPoints();
    if (!pts.length) return null;
    const rect = canvas.getBoundingClientRect();
    const clientX = evt.clientX ?? evt.touches?.[0]?.clientX ?? 0;
    const width = Math.max(canvas.clientWidth || rect.width || 800, 200);
    const height = resolveCssHeight(canvas, kind);
    const box = plotBox(width, height, kind);
    const xs = pts.map((p) => Date.parse(p.ts));
    const xMin = Math.min(...xs);
    const xMax = Math.max(...xs);
    const xSpan = xMax - xMin || 1;
    const x = clientX - rect.left;
    if (x < box.padL - 12 || x > box.padL + box.plotW + 12) return null;
    const ratio = (x - box.padL) / (box.plotW || 1);
    return findNearestIndex(pts, xMin + ratio * xSpan);
  }

  function bindCanvas(canvas, kind) {
    if (!canvas) return;
    canvas.style.cursor = 'crosshair';
    canvas.tabIndex = 0;

    canvas.addEventListener('pointermove', (evt) => {
      if (pinned) return;
      // Touch: pointermove after pointerdown is noisy; only follow mouse/pen hover.
      if (evt.pointerType === 'touch') return;
      const i = indexFromEvent(canvas, evt, kind);
      setIndex(i, { pin: false });
    });

    canvas.addEventListener('pointerleave', () => {
      if (pinned) return;
      // Keep pinned/selected readout visible on touch / narrow screens.
      if (matchMedia('(hover: none)').matches || (typeof window !== 'undefined' && window.innerWidth <= 640)) return;
      setIndex(null, { pin: false });
    });

    canvas.addEventListener('pointerdown', (evt) => {
      const i = indexFromEvent(canvas, evt, kind);
      if (i == null) {
        setIndex(null, { pin: false });
        return;
      }
      // tap same point → unpin; else pin (works for touch and mouse)
      if (pinned && i === index) {
        setIndex(null, { pin: false });
      } else {
        setIndex(i, { pin: true });
      }
    });

    canvas.addEventListener('keydown', (evt) => {
      const pts = getPoints();
      if (!pts.length) return;
      if (evt.key === 'ArrowLeft' || evt.key === 'ArrowRight') {
        evt.preventDefault();
        const delta = evt.key === 'ArrowLeft' ? -1 : 1;
        const base = index == null ? pts.length - 1 : index;
        setIndex(base + delta, { pin: true });
      } else if (evt.key === 'Escape') {
        setIndex(null, { pin: false });
      } else if (evt.key === 'Home') {
        setIndex(0, { pin: true });
      } else if (evt.key === 'End') {
        setIndex(pts.length - 1, { pin: true });
      }
    });
  }

  bindCanvas(combinedCanvas, 'combined');
  bindCanvas(splitCanvases?.t, 'split');
  bindCanvas(splitCanvases?.h, 'split');
  bindCanvas(splitCanvases?.p, 'split');

  return {
    getIndex: () => index,
    isPinned: () => pinned,
    getPoint: () => pointAt(getPoints(), index),
    clear: () => setIndex(null, { pin: false }),
    setIndex,
  };
}

export { COLORS, SERIES_LABEL, SERIES_UNIT, CSS_HEIGHT };
