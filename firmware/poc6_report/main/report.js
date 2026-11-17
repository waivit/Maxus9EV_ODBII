/*
 * Battery inspection report (taladkrub, hub handoff H-0135).
 * Fetches one fresh snapshot from /report, draws it on canvases and hands the
 * phone JPEGs: page 1 = summary (Design's list), page 2 = cell detail.
 * Runs on the ESP32's own WiFi with no internet: no libraries, Thai text uses
 * the phone's system fonts, the logo is drawn from its SVG paths (so the
 * canvas is never tainted and toBlob always works).
 *
 * Content rules from Design: no VIN, no good/bad colouring of values, no
 * conclusion about battery replacement, SOH and odometer carry fixed captions.
 * Page 2 explains what the numbers mean but gives no verdict; assessment bands
 * (docs/findings/16) wait for Design's approval.
 */
(function () {
  'use strict';

  /* The module chart stays off real reports until the module formula is
   * checked at the car (hypothesis H7 b/d). Demo always shows it. */
  var SHOW_MODULES = false;

  var W = 1080, PAD = 64, MAX_H = 2000;
  var INK = '#111827', MUTED = '#4b5563', LINE = '#e5e7eb', BAND = '#14181f', LIME = '#b6f23a';
  var FONT = '"Noto Sans Thai","Sukhumvit Set",Thonburi,Tahoma,sans-serif';
  var NA = 'อ่านไม่ได้';
  var MONTHS = ['ม.ค.', 'ก.พ.', 'มี.ค.', 'เม.ย.', 'พ.ค.', 'มิ.ย.',
                'ก.ค.', 'ส.ค.', 'ก.ย.', 'ต.ค.', 'พ.ย.', 'ธ.ค.'];
  /* B048 enum (MG reference, note 07). Only 03 is confirmed on our car, so
   * the raw code is always shown next to the text. */
  var BMS_STATE = {
    0: 'เสียบสายชาร์จ ไม่ได้ชาร์จ', 1: 'พัก', 3: 'กำลังใช้งาน', 6: 'กำลังชาร์จ AC',
    7: 'กำลังชาร์จ DC', 8: 'กำลังเข้าสู่โหมดพัก', 10: 'เสียบสายชาร์จ', 12: 'เริ่มชาร์จ'
  };
  var ERRORS = {
    bt: 'ยังไม่ได้เชื่อมต่อ ELM327 · รอให้จุดสถานะ BT ด้านล่างเป็นสีเขียว',
    busy: 'อุปกรณ์กำลังอ่านค่าอยู่ ลองใหม่อีกครั้ง',
    novalues: 'รถไม่ตอบ · ตรวจว่ารถอยู่ในสถานะ READY',
    nvs: 'ออกเลขรายงานไม่ได้ (หน่วยความจำอุปกรณ์) · ลองใหม่อีกครั้ง'
  };

  /* taladkrub mark (public/brand/logo/mark-lime.svg, 24x24 viewBox). */
  var LOGO_CIRCLES = [[7.1, 3.8, 1.7], [16.9, 3.8, 1.7]];
  var LOGO_PATHS = [
    'M7.4 7.3H10.7L7.8 12.4L10.6 13.3L9.4 19.6H6.4L6.1 20.6H3.6C2.2 20.6 1.8 19.4 2.2 17.7' +
    'C2.9 14.7 4.2 12.2 5.9 9C6.3 8 6.7 7.3 7.4 7.3Z M5 13.9C6.4 13.8 7.5 14.5 8.8 15.3' +
    'L8.6 16.2C7.1 15.9 5.8 15.5 4.8 15C4.3 14.6 4.5 14 5 13.9Z',
    'M13.7 7.3H16.6C17.3 7.3 17.7 8 18.1 9C19.8 12.2 21.1 14.7 21.8 17.7C22.2 19.4 21.8 20.6' +
    ' 20.4 20.6H17.9L17.6 19.6H11.7L15.3 12.5L12.5 11.8Z M19 13.9C17.6 13.8 16.5 14.5 15.2' +
    ' 15.3L15.4 16.2C16.9 15.9 18.2 15.5 19.2 15C19.7 14.6 19.5 14 19 13.9Z'
  ];

  function $(id) { return document.getElementById(id); }
  function pad(n, w) { var s = String(n); while (s.length < w) s = '0' + s; return s; }
  function commas(n) { return String(Math.round(n)).replace(/\B(?=(\d{3})+(?!\d))/g, ','); }

  /* "17 พ.ย. 2569 เวลา 14:05 น." from the phone's clock (the ESP32 has none). */
  function thaiDate(d) {
    return d.getDate() + ' ' + MONTHS[d.getMonth()] + ' ' + (d.getFullYear() + 543) +
      ' เวลา ' + pad(d.getHours(), 2) + ':' + pad(d.getMinutes(), 2) + ' น.';
  }

  /* TK-YYMM-NNNN: YYMM from the phone's clock, NNNN from the device's NVS
   * counter, which never repeats. */
  function reportNo(d, seq, demo) {
    if (demo) return 'TK-DEMO';
    return 'TK-' + pad(d.getFullYear() % 100, 2) + pad(d.getMonth() + 1, 2) + '-' + pad(seq, 4);
  }

  /* Value helpers over the /report "v" object. */
  function has(v, k) { return !!(v[k] && v[k].ok); }
  function num(v, k, dec, unit) { return has(v, k) ? v[k].v.toFixed(dec) + ' ' + unit : NA; }
  function signed(v, k, dec, unit) {
    if (!has(v, k)) return NA;
    var x = v[k].v;
    return (x > 0 ? '+' : '') + x.toFixed(dec) + ' ' + unit;
  }
  function spreadMv(v) {
    return has(v, 'cmax') && has(v, 'cmin') ? Math.round((v.cmax.v - v.cmin.v) * 1000) : null;
  }
  function idxNote(v, k, word) {
    return has(v, k) && v[k].i >= 0 ? word + 'หมายเลข ' + v[k].i + ' (ตามที่ระบบแบตเตอรี่รายงาน)' : '';
  }

  /* ---------------------------- drawing kit ---------------------------- */

  function drawLogo(g, x, y, size) {
    g.save();
    g.translate(x, y);
    g.scale(size / 24, size / 24);
    g.fillStyle = LIME;
    LOGO_CIRCLES.forEach(function (c) {
      g.beginPath(); g.arc(c[0], c[1], c[2], 0, Math.PI * 2); g.fill();
    });
    if (window.Path2D) LOGO_PATHS.forEach(function (p) { g.fill(new Path2D(p), 'evenodd'); });
    g.restore();
  }

  /* Draw text at `px`, shrinking until it fits maxW (no wrapping needed). */
  function fitText(g, text, x, y, weight, px, color, maxW) {
    g.fillStyle = color;
    do { g.font = weight + ' ' + px + 'px ' + FONT; px -= 1; }
    while (px > 18 && g.measureText(text).width > maxW);
    g.fillText(text, x, y);
  }

  function rule(g, y, h) { g.fillStyle = LINE; g.fillRect(PAD, y, W - 2 * PAD, h || 2); }

  /* Canvas with white page, header band and the report identity block.
   * Returns the y below the identity block. */
  function page(H, subtitle, meta) {
    var cv = document.createElement('canvas');
    cv.width = W; cv.height = H;
    var g = cv.getContext('2d');
    var inner = W - 2 * PAD;
    g.textBaseline = 'alphabetic';
    g.fillStyle = '#ffffff'; g.fillRect(0, 0, W, H);
    g.fillStyle = BAND; g.fillRect(0, 0, W, 230);
    drawLogo(g, PAD, 55, 120);
    fitText(g, 'ตลาดครับ', PAD + 150, 118, '700', 60, '#ffffff', inner - 150);
    fitText(g, subtitle, PAD + 150, 178, '400', 38, '#d1d5db', inner - 150);
    meta.forEach(function (m, k) {
      var y = 295 + k * 50;
      fitText(g, m[0], PAD, y, '400', 30, MUTED, 250);
      fitText(g, m[1], PAD + 270, y, k === 0 ? '700' : '400', 30, INK, inner - 270);
    });
    var y = 295 + (meta.length - 1) * 50 + 30;
    rule(g, y);
    return { cv: cv, g: g, y: y + 2 };
  }

  function watermark(g, H) {
    g.save();
    g.translate(W / 2, H / 2);
    g.rotate(-0.5);
    g.textAlign = 'center';
    g.fillStyle = 'rgba(220,38,38,0.22)';
    g.font = '700 96px ' + FONT;
    g.fillText('ตัวอย่าง', 0, -60);
    g.font = '700 64px ' + FONT;
    g.fillText('ไม่ใช่ผลตรวจจริง', 0, 40);
    g.restore();
  }

  function footer(g, H, lines) {
    rule(g, H - 46 - 38 * lines.length);
    lines.forEach(function (t, k) {
      fitText(g, t, PAD, H - 44 - 38 * (lines.length - 1 - k), '400', 24, MUTED, W - 2 * PAD);
    });
  }

  /* ------------------------------ page 1 ------------------------------ */

  function rows1(v) {
    var s = spreadMv(v);
    return [
      { t: 'สุขภาพแบตเตอรี่ (SOH)', v: num(v, 'soh', 1, '%'),
        c: 'ค่าที่ระบบแบตเตอรี่ของรถรายงาน (ยังไม่ได้เทียบกับศูนย์บริการ)' },
      { t: 'เลขไมล์', v: has(v, 'odo') ? commas(v.odo.v) + ' กม.' : NA,
        c: 'ข้อมูลประกอบ · อ่านจากชุดควบคุมมอเตอร์' },
      { t: 'แรงดันเซลล์สูงสุด', v: num(v, 'cmax', 3, 'V'), c: idxNote(v, 'cmax', 'เซลล์') },
      { t: 'แรงดันเซลล์ต่ำสุด', v: num(v, 'cmin', 3, 'V'), c: idxNote(v, 'cmin', 'เซลล์') },
      { t: 'ส่วนต่างแรงดันเซลล์ (สูงสุด − ต่ำสุด)', v: s === null ? NA : s + ' mV',
        c: 'ตัวเลขดิบ · รายละเอียดอยู่ในหน้า 2' },
      { t: 'อุณหภูมิแบตเตอรี่สูงสุด', v: num(v, 'thi', 1, '°C'), c: idxNote(v, 'thi', 'เซนเซอร์') },
      { t: 'อุณหภูมิแบตเตอรี่ต่ำสุด', v: num(v, 'tlo', 1, '°C'), c: idxNote(v, 'tlo', 'เซนเซอร์') },
      { t: 'SOC (ค่าดิบ)', v: num(v, 'soc', 1, '%'),
        c: 'ค่าดิบจากระบบแบตเตอรี่ · ตัวเลขบนหน้าปัดอาจต่างจากนี้' }
    ];
  }

  function render1(d, when, no) {
    var list = rows1(d.v), ROW = 165, TOP = 485;
    var H = TOP + list.length * ROW + 150;        /* 1955 px */
    var p = page(H, 'รายงานตรวจแบตเตอรี่จากระบบรถ', [
      ['เลขรายงาน', no + '  ·  หน้า 1/2'],
      ['วันเวลาที่ตรวจ', thaiDate(when)],
      ['รุ่นรถ', 'MAXUS 9 · โปรไฟล์ ' + d.profile],
      ['firmware', d.fw]
    ]);
    var g = p.g, inner = W - 2 * PAD;
    list.forEach(function (r, k) {
      var y0 = TOP + k * ROW;
      fitText(g, r.t, PAD, y0 + 38, '400', 30, MUTED, inner);
      fitText(g, r.v, PAD, y0 + 100, '700', 56, INK, inner);
      if (r.c) fitText(g, r.c, PAD, y0 + 140, '400', 26, MUTED, inner);
      if (k < list.length - 1) rule(g, y0 + ROW - 4, 1);
    });
    footer(g, H, ['อ่านจากพอร์ต OBD ของรถแบบอ่านอย่างเดียว ไม่เปลี่ยนค่าใดในรถ',
                  'ตัวเลขทั้งหมดเป็นค่าที่ระบบของรถรายงาน ณ เวลาที่ตรวจ']);
    if (d.demo) watermark(g, H);
    return p.cv;
  }

  /* ------------------------------ page 2 ------------------------------ */

  /* Series cell count ~ pack voltage / mean cell voltage. An estimate only:
   * the mean uses just the max and min cell. */
  function cellCount(v) {
    if (!(has(v, 'pv') && has(v, 'cmax') && has(v, 'cmin'))) return null;
    var mean = (v.cmax.v + v.cmin.v) / 2;
    return mean > 0 ? Math.round(v.pv.v / mean) : null;
  }

  function bmsText(v) {
    if (!has(v, 'bms')) return NA;
    var c = Math.round(v.bms.v);
    return (BMS_STATE[c] || 'ไม่ทราบ') + ' (รหัส ' + pad(c.toString(16).toUpperCase(), 2) + ')';
  }

  var EXPLAIN = [
    '• ส่วนต่างแรงดันบอกความสมดุลของเซลล์ในแพ็ก ไม่ได้บอกสุขภาพของเซลล์โดยตรง',
    '• ความจุที่ใช้ได้ของแพ็กถูกจำกัดด้วยเซลล์ที่อ่อนที่สุด',
    '   ส่วนต่างที่กว้างจึงอาจทำให้ใช้ความจุได้ไม่เต็ม แม้ SOH จะสูง',
    '• SOH (หน้า 1) เป็นค่าที่ระบบแบตเตอรี่คำนวณภาพรวม',
    '   ใช้ประกอบกับส่วนต่างนี้ได้ แต่แปลงหากันไม่ได้',
    '• ส่วนต่างเปลี่ยนตามสภาพขณะวัด ควรอ่านขณะรถจอดนิ่งและกระแสใกล้ 0'
  ];

  /* Key/value line: label left, value at a fixed column. */
  function kv(g, y, label, value) {
    fitText(g, label, PAD, y, '400', 28, MUTED, 400);
    fitText(g, value, PAD + 420, y, '700', 32, INK, W - 2 * PAD - 420);
  }

  function section(g, y, title) {
    fitText(g, title, PAD, y, '700', 32, INK, W - 2 * PAD);
  }

  /* Module chart: one vertical range per module from its two voltages, with
   * the pack-wide cell max/min as dashed reference lines. Returns height used. */
  function moduleChart(g, y, d) {
    var v = d.v, mods = d.mod || [];
    var CH = 160, LEFT = PAD + 110, RIGHT = W - PAD, colW = (RIGHT - LEFT) / 12;
    var vals = [];
    mods.forEach(function (m) { if (m.ok) vals.push(m.va, m.vb); });
    if (has(v, 'cmax')) vals.push(v.cmax.v);
    if (has(v, 'cmin')) vals.push(v.cmin.v);
    if (!vals.length) {
      fitText(g, 'อ่านค่าราย module ไม่ได้', PAD, y + 40, '400', 28, MUTED, W - 2 * PAD);
      return 60;
    }
    var lo = Math.min.apply(null, vals), hi = Math.max.apply(null, vals);
    var span = Math.max(hi - lo, 0.01);
    lo -= span * 0.15; hi += span * 0.15;
    function yy(x) { return y + CH - (x - lo) / (hi - lo) * CH; }

    g.fillStyle = LINE; g.fillRect(LEFT, y, 1, CH); g.fillRect(LEFT, y + CH, RIGHT - LEFT, 1);
    fitText(g, hi.toFixed(3) + ' V', PAD, y + 20, '400', 22, MUTED, 105);
    fitText(g, lo.toFixed(3) + ' V', PAD, y + CH, '400', 22, MUTED, 105);

    g.save();
    g.setLineDash([10, 8]); g.strokeStyle = MUTED; g.lineWidth = 2;
    ['cmax', 'cmin'].forEach(function (k) {
      if (!has(v, k)) return;
      g.beginPath(); g.moveTo(LEFT, yy(v[k].v)); g.lineTo(RIGHT, yy(v[k].v)); g.stroke();
    });
    g.restore();

    mods.forEach(function (m, i) {
      var cx = LEFT + colW * (i + 0.5);
      g.textAlign = 'center';
      if (m.ok) {
        var a = yy(Math.max(m.va, m.vb)), b = yy(Math.min(m.va, m.vb));
        g.fillStyle = INK;
        g.fillRect(cx - 4, a, 8, Math.max(b - a, 2));
        g.beginPath(); g.arc(cx, a, 7, 0, Math.PI * 2); g.fill();
        g.beginPath(); g.arc(cx, b, 7, 0, Math.PI * 2); g.fill();
        fitText(g, Math.round(Math.max(m.ta, m.tb)) + '°', cx, y + CH + 76, '400', 22, MUTED, colW - 6);
      } else {
        fitText(g, '—', cx, y + CH / 2, '400', 28, MUTED, colW - 6);
        fitText(g, '—', cx, y + CH + 76, '400', 22, MUTED, colW - 6);
      }
      fitText(g, 'M' + (i + 1), cx, y + CH + 40, '700', 22, INK, colW - 6);
      g.textAlign = 'left';
    });
    fitText(g, 'อุณหภูมิ', PAD, y + CH + 76, '400', 22, MUTED, 105);
    fitText(g, 'เส้นประ = แรงดันเซลล์สูงสุดและต่ำสุดของทั้งแพ็ก · ค่าราย module ยังไม่ยืนยันสูตร',
            PAD, y + CH + 122, '400', 24, MUTED, W - 2 * PAD);
    return CH + 140;
  }

  function render2(d, when, no) {
    var v = d.v, showMods = SHOW_MODULES || d.demo;
    var n = cellCount(v), s = spreadMv(v);
    var tSpread = has(v, 'thi') && has(v, 'tlo') ? (v.thi.v - v.tlo.v).toFixed(1) + ' °C' : NA;
    var at = function (k, word) { return has(v, k) && v[k].i >= 0 ? '  ·  ' + word + ' #' + v[k].i : ''; };

    /* Lay out first to find the height, then draw. */
    var SEC = 60, GAP = 24;
    var plan = [];
    plan.push({ h: SEC, f: function (g, y) { section(g, y + 48, 'สภาพขณะวัด'); } });
    [['แรงดันแพ็ก', num(v, 'pv', 1, 'V')],
     ['กระแสแพ็ก', signed(v, 'cur', 1, 'A') + (has(v, 'cur') ? '  (+ = จ่ายไฟ)' : '')],
     ['SOC (ค่าดิบ)', num(v, 'soc', 1, '%')],
     ['สถานะระบบแบตเตอรี่', bmsText(v)]].forEach(function (r) {
      plan.push({ h: 50, f: function (g, y) { kv(g, y + 40, r[0], r[1]); } });
    });
    plan.push({ h: GAP, f: function (g, y) { rule(g, y + 12, 1); } });

    plan.push({ h: SEC, f: function (g, y) { section(g, y + 48, 'เซลล์'); } });
    plan.push({ h: 50, f: function (g, y) {
      kv(g, y + 40, 'จำนวนเซลล์อนุกรม (ประมาณ)', n === null ? NA : '~' + n + ' เซลล์');
    } });
    plan.push({ h: 40, f: function (g, y) {
      fitText(g, 'คำนวณจาก แรงดันแพ็ก ÷ แรงดันเฉลี่ยของเซลล์ · ยังไม่ยืนยันจากสเปกผู้ผลิต',
              PAD, y + 28, '400', 24, MUTED, W - 2 * PAD);
    } });
    plan.push({ h: 50, f: function (g, y) { kv(g, y + 40, 'แรงดันเซลล์สูงสุด', num(v, 'cmax', 3, 'V') + at('cmax', 'เซลล์')); } });
    plan.push({ h: 50, f: function (g, y) { kv(g, y + 40, 'แรงดันเซลล์ต่ำสุด', num(v, 'cmin', 3, 'V') + at('cmin', 'เซลล์')); } });
    plan.push({ h: 50, f: function (g, y) { kv(g, y + 40, 'ส่วนต่าง (สูงสุด − ต่ำสุด)', s === null ? NA : s + ' mV'); } });
    plan.push({ h: GAP, f: function (g, y) { rule(g, y + 12, 1); } });

    plan.push({ h: SEC, f: function (g, y) { section(g, y + 48, 'อุณหภูมิแบตเตอรี่'); } });
    plan.push({ h: 50, f: function (g, y) { kv(g, y + 40, 'สูงสุด', num(v, 'thi', 1, '°C') + at('thi', 'เซนเซอร์')); } });
    plan.push({ h: 50, f: function (g, y) { kv(g, y + 40, 'ต่ำสุด', num(v, 'tlo', 1, '°C') + at('tlo', 'เซนเซอร์')); } });
    plan.push({ h: 50, f: function (g, y) { kv(g, y + 40, 'ส่วนต่าง', tSpread); } });
    plan.push({ h: GAP, f: function (g, y) { rule(g, y + 12, 1); } });

    if (showMods) {
      plan.push({ h: SEC, f: function (g, y) { section(g, y + 48, 'ราย module (12 กลุ่ม)'); } });
      plan.push({ h: 300, f: function (g, y) { moduleChart(g, y + 6, d); } });
      plan.push({ h: GAP, f: function (g, y) { rule(g, y + 12, 1); } });
    }

    plan.push({ h: SEC, f: function (g, y) { section(g, y + 48, 'อ่านค่าเหล่านี้อย่างไร'); } });
    EXPLAIN.forEach(function (t) {
      plan.push({ h: 36, f: function (g, y) { fitText(g, t, PAD, y + 28, '400', 26, INK, W - 2 * PAD); } });
    });

    var body = plan.reduce(function (a, p) { return a + p.h; }, 0);
    var H = Math.min(MAX_H, 382 + body + 110);
    var pg = page(H, 'รายละเอียดระดับเซลล์', [
      ['เลขรายงาน', no + '  ·  หน้า 2/2'],
      ['วันเวลาที่ตรวจ', thaiDate(when)]
    ]);
    var y = pg.y;
    plan.forEach(function (p) { p.f(pg.g, y); y += p.h; });
    footer(pg.g, H, ['อ่านจากพอร์ต OBD ของรถแบบอ่านอย่างเดียว · ค่าที่ระบบของรถรายงาน ณ เวลาที่ตรวจ']);
    if (d.demo) watermark(pg.g, H);
    return pg.cv;
  }

  /* ------------------------------ output ------------------------------ */

  var urls = [];
  function clearOut() {
    urls.forEach(function (u) { if (u.indexOf('blob:') === 0) URL.revokeObjectURL(u); });
    urls = [];
    $('rout').innerHTML = '';
  }

  function addImage(url, name, label) {
    urls.push(url);
    var box = document.createElement('div');
    var a = document.createElement('a');
    a.className = 'btn'; a.href = url; a.download = name + '.jpg'; a.textContent = label;
    var img = document.createElement('img');
    img.src = url; img.alt = name;
    box.appendChild(a); box.appendChild(img);
    $('rout').appendChild(box);
  }

  function toJpeg(cv, name, label, done) {
    if (cv.toBlob) {
      cv.toBlob(function (b) { addImage(URL.createObjectURL(b), name, label); done(); },
                'image/jpeg', 0.92);
    } else {
      addImage(cv.toDataURL('image/jpeg', 0.92), name, label); done();
    }
  }

  function busy(on) { $('rgo').disabled = on; $('rdemo').disabled = on; }
  function status(t) { $('rst').textContent = t; }

  function run(demo) {
    busy(true);
    clearOut();
    status(demo ? 'กำลังสร้างภาพตัวอย่าง…' : 'กำลังอ่านค่าจากรถ… (ราว 10–15 วินาที)');
    fetch('/report' + (demo ? '?demo=1' : ''), { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (!d.ok) { status(ERRORS[d.err] || 'สร้างรายงานไม่ได้'); return; }
        var when = new Date();
        var no = reportNo(when, d.seq, d.demo);
        /* Sequential so page 1 is always listed first. */
        toJpeg(render1(d, when, no), no, 'บันทึกภาพหน้า 1 (สรุป)', function () {
          toJpeg(render2(d, when, no), no + '-2', 'บันทึกภาพหน้า 2 (ระดับเซลล์)', function () {
            status((d.demo ? 'ภาพตัวอย่าง (ไม่นับเลขรายงาน) · ' : 'เลขรายงาน ' + no + ' · ') +
                   'กดปุ่มบันทึกทีละหน้า หรือกดค้างที่ภาพเพื่อบันทึก');
          });
        });
      })
      .catch(function () { status('เชื่อมต่ออุปกรณ์ไม่ได้ · ตรวจว่ามือถือยังต่อ WiFi MaxusBridge'); })
      .then(function () { busy(false); });
  }

  $('rgo').onclick = function () { run(false); };
  $('rdemo').onclick = function () { run(true); };
})();
