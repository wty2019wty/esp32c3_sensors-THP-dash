/** Lightweight canvas charts with cursor inspection + time-axis zoom/pan */

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

/** Absolute floor for the visible time window (deep zoom still allowed). */
const MIN_SPAN_MS = 3 * 1000;
const PAN_THRESHOLD_PX = 8;

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

function formatTsDetail(ts) {
  const d = new Date(ts);
  return d.toLocaleString(undefined, {
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
  });
}

function xLabelForSpan(ts, spanMs) {
  return spanMs <= 2 * 3600 * 1000 ? formatTsDetail(ts) : formatTsShort(ts);
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

/** Full timestamp bounds of a series. Single-point / equal-ts series get a small pad window. */
export function dataBounds(points) {
  if (!points || !points.length) return null;
  let min = Infinity;
  let max = -Infinity;
  for (const p of points) {
    const t = Date.parse(p.ts);
    if (!Number.isFinite(t)) continue;
    if (t < min) min = t;
    if (t > max) max = t;
  }
  if (!Number.isFinite(min) || !Number.isFinite(max) || min > max) return null;
  if (min === max) {
    // One sample (or all identical ts): expand so the point is drawable, not blank.
    const pad = Math.max(MIN_SPAN_MS, estimateIntervalMs(points));
    return { min: min - pad / 2, max: max + pad / 2 };
  }
  return { min, max };
}

/**
 * Median sampling interval (ms) from timestamps.
 * Used as the zoom floor so long ranges can zoom to ~1–2 sample periods.
 */
export function estimateIntervalMs(points) {
  if (!points || points.length < 2) return MIN_SPAN_MS;
  const times = [];
  for (const p of points) {
    const t = Date.parse(p.ts);
    if (Number.isFinite(t)) times.push(t);
  }
  if (times.length < 2) return MIN_SPAN_MS;
  times.sort((a, b) => a - b);
  const n = times.length;
  const samples = Math.min(60, n - 1);
  const step = Math.max(1, Math.floor((n - 1) / samples));
  const deltas = [];
  for (let i = step; i < n; i += step) {
    const d = (times[i] - times[i - step]) / step;
    if (d > 0) deltas.push(d);
  }
  if (!deltas.length) {
    const raw = times[n - 1] - times[0];
    return Math.max(MIN_SPAN_MS, raw / Math.max(1, n - 1));
  }
  deltas.sort((a, b) => a - b);
  return Math.max(1000, deltas[Math.floor(deltas.length / 2)]);
}

/** Min visible span for a dataset: ~1 sample period, never below absolute floor. */
export function minSpanForPoints(points) {
  const interval = estimateIntervalMs(points);
  return Math.max(MIN_SPAN_MS, interval * 0.8);
}

/** Points inside the zoom window (keep sparse windows for Y-scale; empty → all) */
export function pointsInWindow(points, win) {
  if (!points?.length) return [];
  if (!win) return points;
  const list = points.filter((p) => {
    const t = Date.parse(p.ts);
    return t >= win.min && t <= win.max;
  });
  return list.length ? list : points;
}

/**
 * Clamp a zoom window into data bounds.
 * Returns null when the view is effectively the full range.
 * @param {{min:number,max:number}|null} win
 * @param {number} dataMin
 * @param {number} dataMax
 * @param {number} [minSpanHint] preferred minimum span (e.g. one sample interval)
 */
export function clampWin(win, dataMin, dataMax, minSpanHint) {
  if (!win || !Number.isFinite(dataMin) || !Number.isFinite(dataMax) || dataMax <= dataMin) {
    return null;
  }
  const full = dataMax - dataMin;
  // Deep zoom: floor is data resolution (or 3s), NOT a % of the full range.
  const hint = Number.isFinite(minSpanHint) && minSpanHint > 0 ? minSpanHint : MIN_SPAN_MS;
  const minSpan = Math.min(full, Math.max(MIN_SPAN_MS, hint));
  let min = win.min;
  let max = win.max;
  let span = max - min;
  if (!Number.isFinite(span) || span <= 0) return null;
  if (span < minSpan) {
    const mid = (min + max) / 2;
    min = mid - minSpan / 2;
    max = mid + minSpan / 2;
    span = minSpan;
  }
  if (span >= full * 0.995) return null;
  if (min < dataMin) {
    min = dataMin;
    max = min + span;
  }
  if (max > dataMax) {
    max = dataMax;
    min = max - span;
  }
  if (max <= min) return null;
  return { min, max };
}

/** Zoom/pan the time window around a plot-x ratio [0..1] */
export function zoomWindow(win, dataMin, dataMax, factor, anchorRatio = 0.5, minSpanHint) {
  if (!Number.isFinite(dataMin) || !Number.isFinite(dataMax) || dataMax <= dataMin) return null;
  const full = { min: dataMin, max: dataMax };
  const base = win || full;
  const span = base.max - base.min;
  const ratio = Math.min(1, Math.max(0, anchorRatio));
  const anchor = base.min + span * ratio;
  const nextSpan = span * (factor > 0 ? factor : 1);
  const min = anchor - nextSpan * ratio;
  const max = min + nextSpan;
  return clampWin({ min, max }, dataMin, dataMax, minSpanHint);
}

export function panWindow(win, dataMin, dataMax, deltaMs, minSpanHint) {
  if (!win) return null;
  if (!Number.isFinite(deltaMs) || deltaMs === 0) return win;
  return clampWin({ min: win.min + deltaMs, max: win.max + deltaMs }, dataMin, dataMax, minSpanHint);
}

export function isZoomed(win, dataMin, dataMax) {
  if (!win || !Number.isFinite(dataMin) || !Number.isFinite(dataMax)) return false;
  const full = dataMax - dataMin;
  if (full <= 0) return false;
  return win.max - win.min < full * 0.995;
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

function clipPlot(ctx, box) {
  ctx.save();
  ctx.beginPath();
  ctx.rect(box.padL, box.padT, box.plotW, box.plotH);
  ctx.clip();
}

/**
 * @param {HTMLCanvasElement} canvas
 * @param {Array} points
 * @param {{ series?: string[], cursorIndex?: number|null, xWindow?: {min:number,max:number}|null }} opts
 */
export function drawCombined(canvas, points, opts = {}) {
  const seriesList =
    opts.series && opts.series.length
      ? opts.series
      : ['temperature', 'humidity', 'pressure'];
  const cursorIndex = opts.cursorIndex == null ? null : opts.cursorIndex;
  const xWindow = opts.xWindow || null;
  const { ctx, width, height } = dprCanvas(canvas, opts.cssHeight || chartCssHeight('combined'));
  const box = plotBox(width, height, 'combined');
  const { padL, padT, plotW, plotH } = box;

  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = COLORS.bg;
  ctx.fillRect(0, 0, width, height);
  if (!points.length) return;

  const fullBounds = dataBounds(points);
  if (!fullBounds) return;
  const dataMin = fullBounds.min;
  const dataMax = fullBounds.max;
  const xMin = xWindow ? xWindow.min : dataMin;
  const xMax = xWindow ? xWindow.max : dataMax;
  const xSpan = xMax - xMin || 1;

  const vis = pointsInWindow(points, xWindow || null);
  const tVals = vis.map((p) => p.temperature).filter(Number.isFinite);
  const hVals = vis.map((p) => p.humidity).filter(Number.isFinite);
  const pVals = vis.map((p) => p.pressure).filter(Number.isFinite);
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
    ctx.fillText(xLabelForSpan(ts, xSpan), x, padT + plotH + 8);
  }

  function drawSeries(key, bounds) {
    ctx.strokeStyle = COLORS[key];
    ctx.lineWidth = width < 400 ? 1.5 : 1.8;
    ctx.beginPath();
    let started = false;
    points.forEach((p) => {
      const t = Date.parse(p.ts);
      if (t < xMin - xSpan * 0.02 || t > xMax + xSpan * 0.02) {
        started = false;
        return;
      }
      if (!Number.isFinite(p[key])) {
        /* 部分字段缺省：断线而非连 0 */
        started = false;
        return;
      }
      const x = xOf(t, xMin, xSpan, padL, plotW);
      const y = yOf(p[key], bounds, padT, plotH);
      if (!started) {
        ctx.moveTo(x, y);
        started = true;
      } else {
        ctx.lineTo(x, y);
      }
    });
    ctx.stroke();
  }

  clipPlot(ctx, box);
  if (seriesList.includes('temperature')) drawSeries('temperature', left);
  if (seriesList.includes('humidity')) drawSeries('humidity', left);
  if (seriesList.includes('pressure')) drawSeries('pressure', right);
  ctx.restore();

  ctx.strokeStyle = COLORS.grid;
  ctx.strokeRect(padL, padT, plotW, plotH);

  if (cursorIndex != null && cursorIndex >= 0 && cursorIndex < points.length) {
    const p = points[cursorIndex];
    const t = Date.parse(p.ts);
    if (t >= xMin && t <= xMax) {
      const x = xOf(t, xMin, xSpan, padL, plotW);
      drawCrosshair(ctx, x, box);
      if (seriesList.includes('temperature') && Number.isFinite(p.temperature)) {
        drawDot(ctx, x, yOf(p.temperature, left, padT, plotH), COLORS.temperature);
      }
      if (seriesList.includes('humidity') && Number.isFinite(p.humidity)) {
        drawDot(ctx, x, yOf(p.humidity, left, padT, plotH), COLORS.humidity);
      }
      if (seriesList.includes('pressure') && Number.isFinite(p.pressure)) {
        drawDot(ctx, x, yOf(p.pressure, right, padT, plotH), COLORS.pressure);
      }
    }
  }

  return { xMin, xMax, xSpan, box, left, right, kind: 'combined', dataMin, dataMax };
}

export function drawSeriesChart(canvas, points, key, opts = {}) {
  const cursorIndex = opts.cursorIndex == null ? null : opts.cursorIndex;
  const xWindow = opts.xWindow || null;
  const { ctx, width, height } = dprCanvas(canvas, opts.cssHeight || chartCssHeight('split'));
  const box = plotBox(width, height, 'split');
  const { padL, padT, plotW, plotH } = box;

  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = COLORS.bg;
  ctx.fillRect(0, 0, width, height);
  if (!points.length) return;

  const fullBounds = dataBounds(points);
  if (!fullBounds) return;
  const dataMin = fullBounds.min;
  const dataMax = fullBounds.max;
  const xMin = xWindow ? xWindow.min : dataMin;
  const xMax = xWindow ? xWindow.max : dataMax;
  const xSpan = xMax - xMin || 1;

  const vis = pointsInWindow(points, xWindow || null);
  const vals = vis.map((p) => p[key]).filter(Number.isFinite);
  if (!vals.length) {
    /* 该序列本窗口无有效点（部分字段全缺省） */
    return;
  }
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

  clipPlot(ctx, box);
  ctx.strokeStyle = COLORS[key];
  ctx.lineWidth = width < 400 ? 1.5 : 1.8;
  ctx.beginPath();
  let started = false;
  points.forEach((p) => {
    const t = Date.parse(p.ts);
    if (t < xMin - xSpan * 0.02 || t > xMax + xSpan * 0.02) {
      started = false;
      return;
    }
    if (!Number.isFinite(p[key])) {
      started = false;
      return;
    }
    const x = xOf(t, xMin, xSpan, padL, plotW);
    const y = yOf(p[key], bounds, padT, plotH);
    if (!started) {
      ctx.moveTo(x, y);
      started = true;
    } else {
      ctx.lineTo(x, y);
    }
  });
  ctx.stroke();
  ctx.restore();

  ctx.fillStyle = COLORS.text;
  ctx.font = axisFont(width);
  ctx.textBaseline = 'top';
  const leftLabel = xLabelForSpan(new Date(xMin).toISOString(), xSpan);
  const rightLabel = xLabelForSpan(new Date(xMax).toISOString(), xSpan);
  if (width < 400) {
    ctx.textAlign = 'left';
    ctx.fillText(leftLabel, padL + 2, padT + plotH + 6);
    ctx.textAlign = 'right';
    ctx.fillText(rightLabel, padL + plotW - 2, padT + plotH + 6);
  } else {
    ctx.textAlign = 'center';
    ctx.fillText(leftLabel, padL + 8, padT + plotH + 6);
    ctx.fillText(rightLabel, padL + plotW - 8, padT + plotH + 6);
  }

  ctx.strokeStyle = COLORS.grid;
  ctx.strokeRect(padL, padT, plotW, plotH);

  if (cursorIndex != null && cursorIndex >= 0 && cursorIndex < points.length) {
    const p = points[cursorIndex];
    const t = Date.parse(p.ts);
    if (t >= xMin && t <= xMax) {
      const x = xOf(t, xMin, xSpan, padL, plotW);
      drawCrosshair(ctx, x, box);
      if (Number.isFinite(p[key])) {
        drawDot(ctx, x, yOf(p[key], bounds, padT, plotH), COLORS[key]);
      }
    }
  }

  return { xMin, xMax, xSpan, box, bounds, kind: 'split', key, dataMin, dataMax };
}

export function drawAll(combinedCanvas, splitCanvases, points, view, seriesSet, cursorIndex = null, xWindow = null, cssHeights = null) {
  const win = xWindow || null;
  const combinedH = cssHeights?.combined;
  const splitH = cssHeights?.split;
  if (view === 'split') {
    drawSeriesChart(splitCanvases.t, points, 'temperature', { cursorIndex, xWindow: win, cssHeight: splitH });
    drawSeriesChart(splitCanvases.h, points, 'humidity', { cursorIndex, xWindow: win, cssHeight: splitH });
    drawSeriesChart(splitCanvases.p, points, 'pressure', { cursorIndex, xWindow: win, cssHeight: splitH });
  } else {
    drawCombined(combinedCanvas, points, { series: [...seriesSet], cursorIndex, xWindow: win, cssHeight: combinedH });
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

function pointerX(canvas, evt) {
  const rect = canvas.getBoundingClientRect();
  return (evt.clientX ?? evt.touches?.[0]?.clientX ?? 0) - rect.left;
}

function activeTouches(evt) {
  const list = [];
  if (evt.touches) {
    for (let i = 0; i < evt.touches.length; i++) list.push(evt.touches[i]);
  } else if (evt.pointerId != null && evt.clientX != null) {
    list.push({ clientX: evt.clientX, clientY: evt.clientY });
  }
  return list;
}

/**
 * Bind hover / click / touch cursor inspection + time-axis zoom/pan.
 *
 * Zoom UX:
 * - Desktop: wheel over chart zooms time axis; drag pans when zoomed
 * - Mobile: pinch zooms; horizontal drag pans when zoomed
 * - Reset: explicit control only (「重置缩放」/ keyboard 0/R) — double-click does NOT reset
 */
export function bindChartCursor({
  combinedCanvas,
  splitCanvases,
  getPoints,
  getView,
  onChange,
  getXWindow,
  setXWindow,
}) {
  let index = null;
  let pinned = false;

  /** @type {Map<number, {x:number,y:number}>} */
  const pointers = new Map();
  let pinch = null;
  let pan = null;

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

  function boundsOf(pts) {
    return dataBounds(pts);
  }

  function currentWin(pts) {
    const full = boundsOf(pts);
    if (!full) return null;
    const win = typeof getXWindow === 'function' ? getXWindow() : null;
    return clampWin(win, full.min, full.max, minSpanForPoints(pts)) || { min: full.min, max: full.max };
  }

  function commitWin(next) {
    if (typeof setXWindow !== 'function') return;
    setXWindow(next);
  }

  function resetZoom() {
    commitWin(null);
  }

  function plotInfo(canvas, kind) {
    const rect = canvas.getBoundingClientRect();
    const width = Math.max(canvas.clientWidth || rect.width || 800, 200);
    const height = resolveCssHeight(canvas, kind);
    const box = plotBox(width, height, kind);
    return { rect, width, height, box };
  }

  function ratioFromClientX(canvas, kind, clientX) {
    const { rect, box } = plotInfo(canvas, kind);
    const x = clientX - rect.left;
    return Math.min(1, Math.max(0, (x - box.padL) / (box.plotW || 1)));
  }

  function indexFromEvent(canvas, evt, kind) {
    const pts = getPoints();
    if (!pts.length) return null;
    const { rect, box } = plotInfo(canvas, kind);
    const win = currentWin(pts);
    if (!win) return null;
    const clientX = evt.clientX ?? evt.touches?.[0]?.clientX ?? 0;
    const x = clientX - rect.left;
    if (x < box.padL - 12 || x > box.padL + box.plotW + 12) return null;
    const ratio = (x - box.padL) / (box.plotW || 1);
    return findNearestIndex(pts, win.min + ratio * (win.max - win.min));
  }

  function applyWheelZoom(canvas, kind, evt) {
    const pts = getPoints();
    if (!pts.length) return;
    const full = boundsOf(pts);
    if (!full) return;
    const { box } = plotInfo(canvas, kind);
    const x = pointerX(canvas, evt);
    const ratio = Math.min(1, Math.max(0, (x - box.padL) / (box.plotW || 1)));
    const win = currentWin(pts);
    if (!win) return;
    const minSpan = minSpanForPoints(pts);

    // Horizontal trackpad / shift+wheel → pan
    if (Math.abs(evt.deltaX) > Math.abs(evt.deltaY)) {
      const span = win.max - win.min;
      const dt = (evt.deltaX / (box.plotW || 1)) * span;
      commitWin(panWindow(win, full.min, full.max, dt, minSpan));
      return;
    }

    const dy = evt.deltaY || 0;
    // Discrete mouse-wheel notches (~±100) zoom harder; trackpads stay smooth.
    const factor =
      Math.abs(dy) >= 40
        ? dy > 0
          ? 1.35
          : 1 / 1.35
        : Math.exp(dy * 0.0045);
    commitWin(zoomWindow(win, full.min, full.max, factor, ratio, minSpan));
  }

  function applyPinch(pts, canvas, kind, t1, t2) {
    const full = boundsOf(pts);
    if (!full) return;
    const { box } = plotInfo(canvas, kind);
    const dist = Math.hypot(t1.clientX - t2.clientX, t1.clientY - t2.clientY) || 1;
    const midX = (t1.clientX + t2.clientX) / 2;
    const minSpan = minSpanForPoints(pts);

    if (!pinch) {
      const win = currentWin(pts);
      if (!win) return;
      pinch = {
        dist0: dist,
        win0: win,
        mid0: midX,
        canvas,
        kind,
      };
      return;
    }

    const factor = pinch.dist0 / dist;
    const rect = canvas.getBoundingClientRect();
    const x0 = pinch.mid0 - rect.left;
    const x1 = midX - rect.left;
    const span0 = pinch.win0.max - pinch.win0.min;
    const ratio0 = Math.min(1, Math.max(0, (x0 - box.padL) / (box.plotW || 1)));
    const panPx = x1 - x0;
    const panMs = (panPx / (box.plotW || 1)) * span0;

    const anchor = pinch.win0.min + span0 * ratio0;
    const nextSpan = span0 * factor;
    let min = anchor - nextSpan * ratio0 - panMs;
    let max = min + nextSpan;
    commitWin(clampWin({ min, max }, full.min, full.max, minSpan));
  }

  function bindCanvas(canvas, kind) {
    if (!canvas) return;
    canvas.style.cursor = 'crosshair';
    canvas.tabIndex = 0;
    // Allow custom pinch/horizontal pan; keep vertical page scroll via pan-y.
    canvas.style.touchAction = 'pan-y';

    canvas.addEventListener('pointermove', (evt) => {
      if (evt.pointerType === 'touch') {
        if (pointers.has(evt.pointerId)) {
          pointers.set(evt.pointerId, { x: evt.clientX, y: evt.clientY });
          if (pointers.size >= 2) {
            const [a, b] = [...pointers.values()];
            applyPinch(getPoints(), canvas, kind, a, b);
            pan = null;
            return;
          }
        }
        // Horizontal pan only when time axis is zoomed in
        if (pan && pan.zoomed && pointers.size === 1) {
          const dx = evt.clientX - pan.x0;
          if (Math.abs(dx) >= PAN_THRESHOLD_PX) {
            pan.moved = true;
            const pts = getPoints();
            const full = boundsOf(pts);
            const { box } = plotInfo(canvas, kind);
            if (full && pan.win0) {
              const dt = (-dx / (box.plotW || 1)) * (pan.win0.max - pan.win0.min);
              commitWin(panWindow(pan.win0, full.min, full.max, dt, minSpanForPoints(pts)));
            }
          }
          return;
        }
        return;
      }

      if (pinned) return;
      if (pan && pan.zoomed) {
        const dx = evt.clientX - pan.x0;
        if (Math.abs(dx) >= PAN_THRESHOLD_PX) {
          pan.moved = true;
          const pts = getPoints();
          const full = boundsOf(pts);
          const { box } = plotInfo(canvas, kind);
          if (full && pan.win0) {
            const dt = (-dx / (box.plotW || 1)) * (pan.win0.max - pan.win0.min);
            commitWin(panWindow(pan.win0, full.min, full.max, dt, minSpanForPoints(pts)));
          }
        }
        return;
      }
      const i = indexFromEvent(canvas, evt, kind);
      setIndex(i, { pin: false });
    });

    canvas.addEventListener('pointerleave', () => {
      if (pinned) return;
      if (pan) return;
      if (matchMedia('(hover: none)').matches || (typeof window !== 'undefined' && window.innerWidth <= 640)) return;
      setIndex(null, { pin: false });
    });

    canvas.addEventListener('pointerdown', (evt) => {
      canvas.setPointerCapture?.(evt.pointerId);
      pointers.set(evt.pointerId, { x: evt.clientX, y: evt.clientY });

      if (pointers.size >= 2) {
        const [a, b] = [...pointers.values()];
        pan = null;
        applyPinch(getPoints(), canvas, kind, a, b);
        return;
      }

      const pts = getPoints();
      const full = boundsOf(pts);
      const win = full ? currentWin(pts) : null;
      const zoomed = full && win && isZoomed(win, full.min, full.max);

      pan = {
        x0: evt.clientX,
        y0: evt.clientY,
        win0: win,
        moved: false,
        zoomed,
        pointerId: evt.pointerId,
      };
    });

    canvas.addEventListener('pointerup', (evt) => {
      pointers.delete(evt.pointerId);
      if (pointers.size < 2) pinch = null;

      const wasPan = pan;
      pan = null;

      if (wasPan?.moved) {
        return;
      }

      // Double-tap does NOT reset zoom — only select/pin data points.
      const i = indexFromEvent(canvas, evt, kind);
      if (i == null) {
        setIndex(null, { pin: false });
        return;
      }
      if (pinned && i === index) {
        setIndex(null, { pin: false });
      } else {
        setIndex(i, { pin: true });
      }
    });

    canvas.addEventListener('pointercancel', (evt) => {
      pointers.delete(evt.pointerId);
      if (pointers.size < 2) pinch = null;
      pan = null;
    });

    canvas.addEventListener('dblclick', (evt) => {
      // Intentionally no zoom reset on double-click — keep the zoomed view.
      evt.preventDefault();
    });

    canvas.addEventListener(
      'wheel',
      (evt) => {
        const pts = getPoints();
        if (!pts.length) return;
        evt.preventDefault();
        applyWheelZoom(canvas, kind, evt);
      },
      { passive: false }
    );

    // Block browser page-pinch while two fingers are on the chart
    canvas.addEventListener(
      'touchmove',
      (evt) => {
        if (evt.touches && evt.touches.length >= 2) {
          evt.preventDefault();
          const [a, b] = [evt.touches[0], evt.touches[1]];
          applyPinch(getPoints(), canvas, kind, a, b);
        }
      },
      { passive: false }
    );

    canvas.addEventListener(
      'touchstart',
      (evt) => {
        if (evt.touches && evt.touches.length >= 2) {
          evt.preventDefault();
          const [a, b] = [evt.touches[0], evt.touches[1]];
          pinch = null;
          applyPinch(getPoints(), canvas, kind, a, b);
        }
      },
      { passive: false }
    );

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
      } else if (evt.key === '0' || evt.key === 'r' || evt.key === 'R') {
        resetZoom();
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
    resetZoom,
  };
}

export { COLORS, SERIES_LABEL, SERIES_UNIT, CSS_HEIGHT };
