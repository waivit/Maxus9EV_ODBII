/*
 * Battery report: "รายงานผลการอ่านค่าแบตเตอรี่" (taladkrub).
 * Wording rule (Design, 2026-11-19): the report confirms the values were read,
 * it certifies nothing about the car. No "รับรอง" / "ผ่านการตรวจ" / "มาตรฐาน"
 * in the title or next to the seal; the seal marks the issuer only.
 * Fetches one fresh snapshot from /report, draws an A4-proportioned document
 * on a canvas and hands the phone one JPEG. The same image is the on-screen
 * preview and the export.
 * Runs on the ESP32's own WiFi with no internet: no libraries, Thai text uses
 * the phone's system fonts. The seal is a separate vector asset (seal.svg,
 * same origin, so the canvas stays exportable); its Thai text is drawn here
 * because an SVG drawn as an image cannot use the page's fonts.
 *
 * Content rules (Design H-0135/H-0138 + the certificate brief): no VIN, no
 * pass/fail grading or colour bands, no capacity conclusion from SOH, no
 * signature, no QR until a real verification page exists, every value bound
 * to the device's JSON, and "อ่านค่าไม่ได้" / "ไม่มีข้อมูล" instead of a 0.
 */
(function () {
  'use strict';

  var W = 1414, H = 2000;                       /* A4 proportion, long side 2000 */
  var L = 70, R = W - 70;                       /* content edges */
  var GRAPHITE = '#1f2329', GREEN = '#1f5a32', LIME = '#b6f23a', MUTED = '#5b6470';
  var LINE = '#d9dee3', RED = '#b42318', SOFT = '#f1f3f5';
  var TINT = 'rgba(182,242,58,0.14)';
  var WATERMARK_ALPHA = 0.10;                   /* brief: start 8-12 %, tune by reading */
  var FONT = '"Noto Sans Thai","Sukhumvit Set",Thonburi,Tahoma,sans-serif';
  var NA = 'อ่านค่าไม่ได้', NODATA = 'ไม่มีข้อมูล';
  var CARS = { maxus9: 'MAXUS 9' };
  var MONTHS = ['ม.ค.', 'ก.พ.', 'มี.ค.', 'เม.ย.', 'พ.ค.', 'มิ.ย.',
                'ก.ค.', 'ส.ค.', 'ก.ย.', 'ต.ค.', 'พ.ย.', 'ธ.ค.'];
  var ERRORS = {
    bt: 'ยังไม่ได้เชื่อมต่อ ELM327 · รอให้จุดสถานะ BT ด้านล่างเป็นสีเขียว',
    busy: 'อุปกรณ์กำลังอ่านค่าอยู่ ลองใหม่อีกครั้ง',
    novalues: 'รถไม่ตอบ · ตรวจว่ารถอยู่ในสถานะ READY',
    time: 'เวลาในมือถือไม่น่าเชื่อ จึงออกเลขรายงานไม่ได้ · ตั้งวันเวลาเป็นอัตโนมัติแล้วลองใหม่',
    sameminute: 'เพิ่งออกรายงานในนาทีนี้ · รอสักครู่แล้วลองใหม่'
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

  /* Thai date and time (UTC+7) from the same epoch the device used for the
   * report number, so the two always agree whatever the phone's timezone is. */
  function thaiParts(t) {
    var d = new Date((t + 7 * 3600) * 1000);
    return {
      date: d.getUTCDate() + ' ' + MONTHS[d.getUTCMonth()] + ' ' + (d.getUTCFullYear() + 543),
      time: pad(d.getUTCHours(), 2) + ':' + pad(d.getUTCMinutes(), 2) + ' น.'
    };
  }

  /* ---------------------------- value binding ---------------------------- */
  /* A key absent from the JSON = this profile does not read it ("ไม่มีข้อมูล");
   * present but not ok = the read failed this time ("อ่านค่าไม่ได้"). */
  function state(v, k) { return !v[k] ? 'none' : (v[k].ok ? 'ok' : 'fail'); }
  function shown(v, k, fmt) {
    var s = state(v, k);
    return s === 'ok' ? fmt(v[k]) : (s === 'fail' ? NA : NODATA);
  }
  function fixed(dec, unit) { return function (x) { return x.v.toFixed(dec) + ' ' + unit; }; }
  function cellFmt(x) { return x.v.toFixed(3) + ' V' + (x.i >= 0 ? '  ·  เซลล์ #' + x.i : ''); }

  function spreadText(v) {
    var a = state(v, 'cmax'), b = state(v, 'cmin');
    if (a === 'ok' && b === 'ok') return Math.round((v.cmax.v - v.cmin.v) * 1000) + ' mV';
    return (a === 'none' || b === 'none') ? NODATA : NA;
  }

  function tempText(v) {
    var a = state(v, 'tlo'), b = state(v, 'thi');
    if (a === 'ok' && b === 'ok') return v.tlo.v.toFixed(1) + '–' + v.thi.v.toFixed(1) + ' °C';
    if (a === 'ok' || b === 'ok') return (a === 'ok' ? v.tlo : v.thi).v.toFixed(1) + ' °C';
    return (a === 'none' && b === 'none') ? NODATA : NA;
  }

  /* SOH wording per state. Never a grade, never "100 - SOH". */
  function sohCopy(v) {
    var s = state(v, 'soh');
    if (s === 'ok') {
      var p = v.soh.v.toFixed(1);
      return { big: p + '%', pct: v.soh.v, caption: 'ค่าประเมินที่ระบบรถรายงาน ณ เวลาตรวจ',
               easy: 'รถรายงานค่าสุขภาพแบตเตอรี่ ' + p + '% ณ เวลาตรวจ',
               easy2: 'เป็นค่าจากระบบรถ ไม่ใช่ผลทดสอบความจุจริง' };
    }
    if (s === 'fail') {
      return { big: NA, pct: null, caption: 'ครั้งนี้อ่านค่าจากระบบรถไม่ได้',
               easy: 'ครั้งนี้อ่านค่าสุขภาพแบตเตอรี่จากระบบรถไม่ได้',
               easy2: 'ค่าอื่นในใบนี้เป็นค่าที่อ่านได้จริง ณ เวลาตรวจ' };
    }
    return { big: NODATA, pct: null, caption: 'ยังอ่านค่า SOH ของรุ่นนี้ไม่ได้',
             easy: 'ยังอ่านค่าสุขภาพแบตเตอรี่ของรถรุ่นนี้ไม่ได้',
             easy2: 'ค่าอื่นในใบนี้เป็นค่าที่อ่านได้จริง ณ เวลาตรวจ' };
  }

  /* ---------------------------- drawing kit ---------------------------- */

  function font(g, weight, px) { g.font = weight + ' ' + px + 'px ' + FONT; }

  /* Text width that is right for Thai on every phone. On some phones canvas
   * measureText() (and with it textAlign) under-reports Thai drawn from a
   * fallback system font: centred lines start at the centre and nothing
   * shrinks to fit (seen on a real phone, 2026-11-19). The DOM lays Thai out
   * correctly, so widths come from a hidden span in the same font, and every
   * alignment is done by hand. A Latin reference, which canvas measures
   * correctly, cancels any accessibility text scaling the DOM applies. */
  var meter = null, scaleCache = {};
  function domWidth(fontCss, s) {
    if (!meter) {
      meter = document.createElement('span');
      meter.style.cssText = 'position:absolute;left:-9999px;top:0;white-space:pre;visibility:hidden;' +
                            '-webkit-text-size-adjust:none;text-size-adjust:none';
      document.body.appendChild(meter);
    }
    meter.style.font = fontCss;
    meter.textContent = s;
    return meter.getBoundingClientRect().width;
  }
  function textWidth(g, s) {
    var f = g.font;
    if (!(f in scaleCache)) {
      var ref = 'Battery Report 0123456789', d = domWidth(f, ref), c = g.measureText(ref).width;
      scaleCache[f] = d > 0 && c > 0 ? c / d : 0;
    }
    var k = scaleCache[f], w = k ? domWidth(f, s) * k : 0;
    return w > 0 ? w : g.measureText(s).width;     /* no DOM: fall back to canvas */
  }

  /* Draw text, shrinking until it fits maxW. align: 'left' | 'center' | 'right'. */
  function text(g, s, x, y, weight, px, color, maxW, align) {
    var w;
    do { font(g, weight, px); w = textWidth(g, s); px -= 1; } while (px > 14 && maxW && w > maxW);
    g.fillStyle = color;
    g.textAlign = 'left';
    g.fillText(s, x - (align === 'center' ? w / 2 : align === 'right' ? w : 0), y);
  }

  /* Letter-spaced Latin line (canvas letterSpacing is not on every phone). */
  function spaced(g, s, cx, y, px, color, gap) {
    font(g, '600', px);
    g.fillStyle = color;
    var w = 0, i;
    for (i = 0; i < s.length; i++) w += g.measureText(s[i]).width + (i ? gap : 0);
    var x = cx - w / 2;
    for (i = 0; i < s.length; i++) { g.fillText(s[i], x, y); x += g.measureText(s[i]).width + gap; }
  }

  function rule(g, x1, x2, y, h, color) { g.fillStyle = color || LINE; g.fillRect(x1, y, x2 - x1, h || 1); }

  function rrect(g, x, y, w, h, r) {
    g.beginPath();
    g.moveTo(x + r, y); g.arcTo(x + w, y, x + w, y + h, r); g.arcTo(x + w, y + h, x, y + h, r);
    g.arcTo(x, y + h, x, y, r); g.arcTo(x, y, x + w, y, r); g.closePath();
  }

  function drawLogo(g, x, y, size, color) {
    g.save();
    g.translate(x, y);
    g.scale(size / 24, size / 24);
    g.fillStyle = color;
    LOGO_CIRCLES.forEach(function (c) { g.beginPath(); g.arc(c[0], c[1], c[2], 0, Math.PI * 2); g.fill(); });
    if (window.Path2D) LOGO_PATHS.forEach(function (p) { g.fill(new Path2D(p), 'evenodd'); });
    g.restore();
  }

  /* Thai grapheme clusters: a base character plus its combining marks, so an
   * arc never separates a vowel or tone mark from its consonant. */
  function clusters(s) { return s.match(/[^ัิ-ฺ็-๎][ัิ-ฺ็-๎]*/g) || []; }

  /* Text along the top of a circle, reading left to right. */
  function arcText(g, s, cx, cy, r, px, color) {
    font(g, '700', px);
    g.fillStyle = color;
    g.textAlign = 'left';
    g.textBaseline = 'middle';
    var parts = clusters(s), widths = parts.map(function (c) { return textWidth(g, c); });
    var total = widths.reduce(function (a, b) { return a + b; }, 0) * 1.08 / r;
    var a = -Math.PI / 2 - total / 2;
    parts.forEach(function (c, i) {
      var step = widths[i] * 1.08 / r;
      var m = a + step / 2;
      g.save();
      g.translate(cx + r * Math.cos(m), cy + r * Math.sin(m));
      g.rotate(m + Math.PI / 2);
      g.fillText(c, -widths[i] / 2, 0);
      g.restore();
      a += step;
    });
    g.textAlign = 'left';
    g.textBaseline = 'alphabetic';
  }

  /* The seal asset is 400x440 user units; the arc sits above the mark
   * (centre 200,236) and the ribbon line at y 386 (see seal.svg). */
  function drawSeal(g, img, x, y, w, alpha, top, ribbon) {
    if (!img) return;
    var s = w / 400;
    g.save();
    g.globalAlpha = alpha;
    g.drawImage(img, x, y, w, 440 * s);
    arcText(g, top, x + 200 * s, y + 236 * s, 88 * s, 30 * s, RED);
    g.textBaseline = 'middle';
    text(g, ribbon, x + 200 * s, y + 386 * s, '700', 26 * s, RED, 280 * s, 'center');
    g.textBaseline = 'alphabetic';
    g.restore();
  }

  function frame(g) {
    g.strokeStyle = GREEN;
    g.lineWidth = 3; g.strokeRect(22, 22, W - 44, H - 44);
    g.lineWidth = 1; g.strokeRect(34, 34, W - 68, H - 68);
    [[34, 34], [W - 34, 34], [34, H - 34], [W - 34, H - 34]].forEach(function (c) {
      g.save();
      g.translate(c[0], c[1]);
      g.rotate(Math.PI / 4);
      g.fillStyle = '#ffffff'; g.fillRect(-11, -11, 22, 22);
      g.strokeStyle = GREEN; g.lineWidth = 1.5; g.strokeRect(-11, -11, 22, 22);
      g.fillStyle = LIME; g.fillRect(-5, -5, 10, 10);
      g.restore();
    });
  }

  /* Dark-green section tab with a lime slash, then a hairline to the edge. */
  function section(g, y, title) {
    font(g, '700', 34);
    var tw = textWidth(g, title), x2 = L + tw + 70;
    g.fillStyle = GREEN;
    g.beginPath(); g.moveTo(L, y); g.lineTo(x2 + 20, y); g.lineTo(x2, y + 56); g.lineTo(L, y + 56); g.closePath(); g.fill();
    g.fillStyle = LIME;
    g.beginPath(); g.moveTo(x2 + 28, y); g.lineTo(x2 + 44, y); g.lineTo(x2 + 24, y + 56); g.lineTo(x2 + 8, y + 56); g.closePath(); g.fill();
    text(g, title, L + 26, y + 40, '700', 34, '#ffffff');
    rule(g, x2 + 30, R, y + 54, 2, GREEN);
  }

  /* ------------------------------ the page ------------------------------ */

  function render(d, when, no, seal) {
    var v = d.v, soh = sohCopy(v), tp = thaiParts(when);
    var cv = document.createElement('canvas');
    cv.width = W; cv.height = H;
    var g = cv.getContext('2d');
    g.textBaseline = 'alphabetic';
    g.fillStyle = '#ffffff'; g.fillRect(0, 0, W, H);

    /* Watermark first, so every line of text sits on top of it. */
    drawSeal(g, seal, W / 2 - 360, 440, 720, WATERMARK_ALPHA, 'ตลาดครับ', 'ผู้ออกรายงาน');
    frame(g);

    if (d.demo) {                                /* preview only, never on a real report */
      rrect(g, W / 2 - 200, 4, 400, 56, 28); g.fillStyle = LIME; g.fill();
      text(g, 'ตัวอย่างงานออกแบบ', W / 2, 43, '700', 30, GRAPHITE, 360, 'center');
    }

    /* Header */
    drawLogo(g, 330, 92, 112, '#8ccf1f');
    text(g, 'ตลาดครับ', 468, 156, '700', 76, GRAPHITE, 380);
    text(g, 'Taladkrub', 470, 206, '700', 42, GRAPHITE, 380);
    rule(g, 880, 882, 104, 112, GREEN);
    text(g, 'ออกโดยตลาดครับ', 914, 170, '400', 32, GRAPHITE, 420);

    text(g, 'รายงานผลการอ่านค่าแบตเตอรี่', W / 2, 314, '700', 68, GRAPHITE, R - L, 'center');
    spaced(g, 'BATTERY READING REPORT', W / 2, 368, 28, MUTED, 9);
    rule(g, L, R, 398, 3, GREEN);

    /* 1. Reference (no plate / VIN: the device has neither, by design) */
    var refL = [['เลขรายงาน', no], ['รถ', CARS[d.profile] || d.profile]];
    var refR = [['วันที่ตรวจ', tp.date], ['เวลาตรวจ', tp.time]];
    [refL, refR].forEach(function (col, c) {
      var x = c ? 760 : L + 10, vx = c ? 950 : L + 230, xe = c ? R : 680;
      col.forEach(function (r, k) {
        var y = 456 + k * 58;
        text(g, r[0], x, y, '400', 30, GRAPHITE, vx - x - 16);
        text(g, r[1], vx, y, '700', 32, GRAPHITE, xe - vx);
        rule(g, x - 10, xe, y + 20, 1, LINE);
      });
    });
    rule(g, 712, 714, 420, 120, LINE);

    /* 2. Key results */
    section(g, 568, 'ผลสำคัญที่เจ้าของรถควรรู้');
    var cy = 646, ch = 290;
    g.strokeStyle = LINE; g.lineWidth = 2;
    rrect(g, L, cy, 790, ch, 18); g.stroke();
    rrect(g, 890, cy, R - 890, ch, 18); g.stroke();

    text(g, 'สุขภาพแบตเตอรี่ (SOH)', L + 40, cy + 58, '700', 36, GRAPHITE, 710);
    text(g, soh.big, L + 34, cy + 190, '700', soh.pct === null ? 92 : 150, GREEN, 720);
    text(g, soh.caption, L + 40, cy + 228, '400', 28, GRAPHITE, 710);
    if (soh.pct !== null) {                      /* numeric scale only, no colour bands */
      var bx = L + 40, bw = 700, by = cy + 244, pct = Math.max(0, Math.min(100, soh.pct));
      rrect(g, bx, by, bw, 20, 10); g.fillStyle = '#e6e9ec'; g.fill();
      if (pct > 0) { rrect(g, bx, by, Math.max(20, bw * pct / 100), 20, 10); g.fillStyle = LIME; g.fill(); }
      text(g, '0%', bx, by + 44, '400', 22, MUTED);
      text(g, '100%', bx + bw, by + 44, '400', 22, MUTED, 0, 'right');
    }

    var ox = (890 + R) / 2;
    text(g, 'เลขไมล์ที่อ่านได้จากรถ', ox, cy + 70, '700', 34, GRAPHITE, R - 930, 'center');
    var odo = shown(v, 'odo', function (x) { return commas(x.v) + ' กม.'; });
    text(g, odo, ox, cy + 190, '700', state(v, 'odo') === 'ok' ? 96 : 70, GREEN, R - 930, 'center');
    text(g, 'ข้อมูลประกอบ · อ่านจากชุดควบคุมมอเตอร์', ox, cy + 246, '400', 24, MUTED, R - 930, 'center');

    /* 3. Plain-language box */
    var ey = 960;
    rrect(g, L, ey, R - L, 130, 18); g.fillStyle = TINT; g.fill();
    /* Label on two lines so it stays large: "ความหมายของผล" (user, 2026-11-19). */
    text(g, 'ความหมาย', L + 40, ey + 58, '700', 42, GREEN, 250);
    text(g, 'ของผล', L + 40, ey + 106, '700', 42, GREEN, 250);
    rule(g, 330, 332, ey + 22, 86, GREEN);
    text(g, soh.easy, 370, ey + 58, '700', 36, GRAPHITE, R - 400);
    text(g, soh.easy2, 370, ey + 102, '400', 28, MUTED, R - 400);

    var ny = 1130;
    font(g, '400', 26);
    var nt = 'ไม่มีเกณฑ์ผ่าน–ไม่ผ่านในรายงานนี้', nw = textWidth(g, nt);
    text(g, nt, W / 2, ny, '400', 26, MUTED, 0, 'center');
    rule(g, L + 120, W / 2 - nw / 2 - 24, ny - 9, 1, LINE);
    rule(g, W / 2 + nw / 2 + 24, R - 120, ny - 9, 1, LINE);

    /* 4. Supporting data, as read */
    section(g, 1152, 'ข้อมูลประกอบ ณ เวลาตรวจ');
    var rows = [
      ['ระดับประจุ (SOC ค่าดิบ)', shown(v, 'soc', fixed(1, '%'))],
      ['ส่วนต่างแรงดันเซลล์ (สูงสุด − ต่ำสุด)', spreadText(v)],
      ['แรงดันเซลล์สูงสุด', shown(v, 'cmax', cellFmt)],
      ['แรงดันเซลล์ต่ำสุด', shown(v, 'cmin', cellFmt)],
      ['อุณหภูมิแบตเตอรี่', tempText(v)],
      ['แรงดันแพ็ก', shown(v, 'pv', fixed(1, 'V'))]
    ];
    var ty = 1226, RH = 44;
    rows.forEach(function (r, k) {
      var y = ty + k * RH;
      text(g, r[0], L + 30, y + 31, '400', 28, GRAPHITE, 600);
      text(g, r[1], 760, y + 31, '700', 30, GRAPHITE, R - 780);
      rule(g, L, R, y + RH, 1, LINE);
    });
    rule(g, 730, 731, ty, rows.length * RH, LINE);

    /* 5. Method */
    var my = ty + rows.length * RH + 18;
    rrect(g, L, my, R - L, 56, 12); g.fillStyle = SOFT; g.fill();
    text(g, 'วิธีอ่านค่า:', L + 26, my + 38, '700', 26, GREEN, 160);
    text(g, 'OBD แบบอ่านอย่างเดียว · โปรไฟล์ ' + d.profile + ' · Firmware ' + d.fw,
         L + 180, my + 38, '400', 26, GRAPHITE, R - L - 200);

    /* 6. Issuer: electronic, no person, no signature, no QR */
    var iy = my + 80;
    rule(g, L, R, iy, 2, GRAPHITE);
    drawSeal(g, seal, L + 30, iy + 8, 192, 1, 'ตลาดครับ', 'เอกสารอิเล็กทรอนิกส์');
    rule(g, 300, 302, iy + 30, 186, LINE);
    var ix = (302 + R) / 2;
    text(g, 'ออกโดยระบบอิเล็กทรอนิกส์ของตลาดครับ', ix, iy + 64, '700', 36, GRAPHITE, R - 340, 'center');
    text(g, 'เลขรายงาน ' + no, ix, iy + 108, '700', 32, GRAPHITE, R - 340, 'center');
    text(g, 'ตรวจสอบเลขรายงานได้กับทีมตลาดครับ · taladkrub.com', ix, iy + 150, '400', 26, GRAPHITE,
         R - 340, 'center');
    rule(g, ix - 330, ix + 330, iy + 170, 1, LINE);
    text(g, 'เอกสารนี้ออกโดยระบบ ไม่ต้องลงลายมือชื่อ', ix, iy + 206, '400', 26, MUTED, R - 340, 'center');

    /* Scope and footer */
    /* Two larger lines: this limit must stay readable on a phone screen.
     * Wording from Design (hub, 2026-11-19), placed next to the issuer seal. */
    var sy = iy + 226;
    rrect(g, L, sy, R - L, 92, 12); g.fillStyle = '#eceff2'; g.fill();
    var s1 = 'ตลาดครับยืนยันว่าได้อ่านค่าเหล่านี้จากระบบแบตเตอรี่ของรถที่นำมาตรวจ ณ วันเวลาที่ระบุในรายงานนี้';
    var s2 = 'รายงานนี้ไม่ใช่การรับประกันสภาพแบตเตอรี่ สภาพรถ หรือเลขไมล์';
    var spx = 32;                                /* one size for both lines: fit the longer one */
    do { font(g, '700', spx); } while (textWidth(g, s1) > R - L - 40 && --spx > 14);
    text(g, s1, W / 2, sy + 40, '700', spx, GRAPHITE, R - L - 40, 'center');
    text(g, s2, W / 2, sy + 78, '700', spx, GRAPHITE, R - L - 40, 'center');
    text(g, 'taladkrub.com', L + 10, H - 54, '700', 28, GREEN, 300);
    if (d.demo) text(g, 'ตัวอย่าง · ไม่ใช่รายงานฉบับจริง', R - 10, H - 54, '400', 26, GRAPHITE, 600, 'right');
    rule(g, 330, d.demo ? 780 : R - 10, H - 64, 1, GREEN);

    if (d.demo) {                                /* a demo must never pass for a real one */
      g.save();
      g.translate(W / 2, H / 2);
      g.rotate(-0.5);
      text(g, 'ตัวอย่าง', 0, -70, '700', 120, 'rgba(180,35,24,0.14)', 0, 'center');
      text(g, 'ไม่ใช่ผลตรวจจริง', 0, 50, '700', 80, 'rgba(180,35,24,0.14)', 0, 'center');
      g.restore();
    }
    return cv;
  }

  /* The seal, loaded from the device (same origin) BEFORE the car is read:
   * a report without its seal is not issued. Two tries, the second one
   * bypassing any cached failure; a success is kept for later reports. */
  var sealImg = null;
  function loadSealOnce(url) {
    return new Promise(function (resolve) {
      var img = new Image();
      img.onload = function () { resolve(img.naturalWidth ? img : null); };
      img.onerror = function () { resolve(null); };
      img.src = url;
    });
  }
  function loadSeal() {
    if (sealImg) return Promise.resolve(sealImg);
    return loadSealOnce('seal.svg').then(function (img) {
      return img || loadSealOnce('seal.svg?retry=' + Date.now());
    }).then(function (img) { sealImg = img; return img; });
  }

  /* ------------------------------ output ------------------------------ */

  var lastUrl = null;
  function show(url, name) {
    if (lastUrl && lastUrl.indexOf('blob:') === 0) URL.revokeObjectURL(lastUrl);
    lastUrl = url;
    var out = $('rout');
    out.innerHTML = '';
    var a = document.createElement('a');
    a.className = 'btn'; a.href = url; a.download = name + '.jpg'; a.textContent = 'บันทึกภาพ';
    var img = document.createElement('img');
    img.src = url; img.alt = name;
    out.appendChild(a); out.appendChild(img);
  }

  /* The website accepts at most 2 MB per image (H-0138). A report is ~300 KB
   * at 0.92, so this only matters if the layout ever grows: step the quality
   * down until it fits, and refuse rather than hand over an oversize file. */
  var MAX_BYTES = 1900000;                     /* margin under 2 MB */
  var QUALITIES = [0.92, 0.8, 0.7, 0.6, 0.5];

  function toJpeg(cv, name, done) {
    var i = 0;
    function attempt() {
      var q = QUALITIES[i];
      function check(size, deliver) {
        if (size <= MAX_BYTES) { deliver(); done(true); return; }
        if (i < QUALITIES.length - 1) { i++; attempt(); return; }
        done(false);
      }
      if (cv.toBlob) {
        cv.toBlob(function (b) {
          check(b.size, function () { show(URL.createObjectURL(b), name); });
        }, 'image/jpeg', q);
      } else {
        var url = cv.toDataURL('image/jpeg', q);
        check(Math.ceil(url.length * 0.75), function () { show(url, name); });
      }
    }
    attempt();
  }

  function busy(on) { $('rgo').disabled = on; $('rdemo').disabled = on; }
  function status(t) { $('rst').textContent = t; }

  function run(demo) {
    busy(true);
    $('rout').innerHTML = '';
    status(demo ? 'กำลังสร้างภาพตัวอย่าง…' : 'กำลังอ่านค่าจากรถ… (ราว 10–15 วินาที)');
    /* The phone's clock (UTC epoch seconds) is the device's only time source. */
    var now = Math.floor(Date.now() / 1000);
    var seal = null;
    /* The seal first: if it cannot load, stop before the car is read, so no
     * report number is spent on an image without its seal. */
    loadSeal()
      .then(function (img) {
        if (!img) { status('โหลดตราจากอุปกรณ์ไม่ได้ · ลองกดใหม่อีกครั้ง'); return null; }
        seal = img;
        return fetch('/report?t=' + now + (demo ? '&demo=1' : ''), { cache: 'no-store' })
          .then(function (r) { return r.json(); });
      })
      .then(function (d) {
        if (!d) return;
        if (!d.ok) { status(ERRORS[d.err] || 'สร้างรายงานไม่ได้'); return; }
        var when = d.t > 0 ? d.t : now;
        var no = d.no;
        toJpeg(render(d, when, no, seal), no, function (ok) {
          status(!ok ? 'ภาพใหญ่เกิน 2 MB · สร้างภาพไม่ได้' :
                 (d.demo ? 'ภาพตัวอย่าง (ไม่นับเลขรายงาน) · ' : 'เลขรายงาน ' + no + ' · ') +
                 'กด "บันทึกภาพ" หรือกดค้างที่ภาพเพื่อบันทึก');
        });
      })
      .catch(function () { status('เชื่อมต่ออุปกรณ์ไม่ได้ · ตรวจว่ามือถือยังต่อ WiFi MaxusBridge'); })
      .then(function () { busy(false); });
  }

  $('rgo').onclick = function () { run(false); };
  $('rdemo').onclick = function () { run(true); };

  /* ------------------------- DTC read (H9) ------------------------- */
  /* Shown on the phone only, not on the report image (Design has not
   * decided). Codes only: no descriptions yet. */

  var OBD_MODE = { '03': 'รหัสที่บันทึกไว้', '07': 'รหัสที่รอยืนยัน', '0A': 'รหัสถาวร' };
  /* UDS status byte (ISO 14229): the bits a reader cares about first. */
  var STATUS_BITS = [[3, 'ยืนยันแล้ว'], [2, 'รอยืนยัน'], [0, 'เกิดในการทดสอบล่าสุด'],
                     [7, 'สั่งไฟเตือน']];

  function esc(t) {
    return String(t).replace(/[&<>"]/g, function (c) {
      return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
    });
  }
  function hex2(n) { return pad(Number(n).toString(16).toUpperCase(), 2); }
  function statusText(h) {
    var s = parseInt(h, 16), out = [];
    STATUS_BITS.forEach(function (b) { if (s & (1 << b[0])) out.push(b[1]); });
    return (out.length ? out.join(' · ') : 'เคยเกิด') + ' (สถานะ ' + h + ')';
  }

  function renderDtc(d) {
    var html = '<h4>OBD มาตรฐาน (ทุก ECU ที่ตอบ)</h4>';
    (d.obd || []).forEach(function (m) {
      var head = OBD_MODE[m.mode] + ' (' + m.mode + ')';
      if (!m.ecus.length) {
        html += '<div class=row>' + esc(head) + ': <span class=mut>ไม่มี ECU ตอบ</span></div>';
        return;
      }
      m.ecus.forEach(function (e) {
        var body;
        if (e.dtcs) body = e.dtcs.length ?
          e.dtcs.map(function (c) { return '<span class=code>' + esc(c) + '</span>'; }).join(' ') :
          '<span class=mut>ไม่มีรหัส</span>';
        else if (e.nrc !== undefined) body = '<span class=mut>ไม่รองรับ (NRC 0x' + hex2(e.nrc) + ')</span>';
        else body = '<span class=mut>คำตอบอ่านไม่ออก</span>';
        html += '<div class=row>' + esc(head) + ' · ECU ' + esc(e.id) + ': ' + body + '</div>';
      });
    });
    html += '<h4>ราย ECU (UDS)</h4>';
    (d.uds || []).forEach(function (u) {
      var head = esc(u.name) + ' (' + esc(u.ecu) + ')';
      var body;
      if (!u.answered) body = '<span class=mut>ไม่ตอบ</span>';
      else if (u.dtcs) body = u.dtcs.length ? u.dtcs.map(function (c) {
          return '<div><span class=code>' + esc(c.code) + '</span> <span class=mut>' +
                 esc(statusText(c.status)) + '</span></div>';
        }).join('') : '<span class=mut>ไม่มีรหัส</span>';
      else if (u.nrc !== undefined) body = '<span class=mut>ไม่รองรับ (NRC 0x' + hex2(u.nrc) + ')</span>';
      else body = '<span class=mut>คำตอบอ่านไม่ออก</span>';
      html += '<div class=row>' + head + ': ' + body + '</div>';
    });
    $('dout').innerHTML = html;
  }

  var dtcUrl = null;
  function offerDtcJson(d) {
    var when = new Date();
    d.read_at = when.toISOString();
    if (dtcUrl) URL.revokeObjectURL(dtcUrl);
    dtcUrl = URL.createObjectURL(new Blob([JSON.stringify(d, null, 1)], { type: 'application/json' }));
    var a = $('dsave');
    a.href = dtcUrl;
    a.download = 'dtc-' + when.getFullYear() + pad(when.getMonth() + 1, 2) + pad(when.getDate(), 2) +
                 '-' + pad(when.getHours(), 2) + pad(when.getMinutes(), 2) + '.json';
    a.style.display = 'inline-block';
  }

  $('dgo').onclick = function () {
    $('dgo').disabled = true;
    $('dout').innerHTML = '';
    $('dsave').style.display = 'none';
    $('dst').textContent = 'กำลังอ่าน DTC… (ราว 10–20 วินาที)';
    fetch('/dtc', { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (!d.ok) { $('dst').textContent = ERRORS[d.err] || 'อ่าน DTC ไม่ได้'; return; }
        renderDtc(d);
        offerDtcJson(d);
        $('dst').textContent = 'พบ ' + d.total + ' รหัส · ข้อมูลนี้ไม่อยู่ในภาพรายงาน';
      })
      .catch(function () { $('dst').textContent = 'เชื่อมต่ออุปกรณ์ไม่ได้ · ตรวจว่ามือถือยังต่อ WiFi MaxusBridge'; })
      .then(function () { $('dgo').disabled = false; });
  };
})();
