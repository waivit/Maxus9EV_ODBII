/*
 * Battery inspection report (taladkrub, hub handoff H-0135).
 * Fetches one fresh snapshot from /report, draws it on a canvas and hands the
 * phone one JPEG. Top half: large-type summary for buyers. Bottom half: small-
 * type technical detail for readers who want the cell-level numbers.
 * Runs on the ESP32's own WiFi with no internet: no libraries, Thai text uses
 * the phone's system fonts, the logo is drawn from its SVG paths (so the
 * canvas is never tainted and toBlob always works).
 *
 * Content rules from Design: no VIN, no good/bad colouring of values, no
 * conclusion about battery replacement, SOH and odometer carry fixed captions,
 * temperature included. Assessment bands (docs/findings/16) wait for Design.
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

  var EXPLAIN = [
    '• ส่วนต่างแรงดันเซลล์บอกความสมดุลของแพ็ก ไม่ได้บอกสุขภาพของเซลล์โดยตรง',
    '• ความจุที่ใช้ได้ถูกจำกัดด้วยเซลล์ที่อ่อนที่สุด ส่วนต่างกว้างอาจทำให้ใช้ความจุได้ไม่เต็มแม้ SOH สูง',
    '• SOH เป็นค่าที่ระบบแบตเตอรี่คำนวณภาพรวม ใช้ประกอบกับส่วนต่างได้ แต่แปลงหากันไม่ได้'
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
  function spreadMv(v) {
    return has(v, 'cmax') && has(v, 'cmin') ? Math.round((v.cmax.v - v.cmin.v) * 1000) : null;
  }

  /* No series cell count on the report: there is no published spec, only an
   * estimate from pack V / cell V (H7 c), and the user chose to show nothing
   * rather than an estimate (2026-11-17). */

  function tempRange(v) {
    var a = has(v, 'tlo'), b = has(v, 'thi');
    if (a && b) return v.tlo.v.toFixed(1) + '–' + v.thi.v.toFixed(1) + ' °C';
    if (a || b) return (a ? v.tlo : v.thi).v.toFixed(1) + ' °C';
    return NA;
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
    while (px > 16 && g.measureText(text).width > maxW);
    g.fillText(text, x, y);
  }

  function rule(g, y, h) { g.fillStyle = LINE; g.fillRect(PAD, y, W - 2 * PAD, h || 2); }

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

  /* Compact module chart: one vertical range per module from its two
   * voltages, pack-wide cell max/min as dashed reference lines. */
  function moduleChart(g, y, d, CH) {
    var v = d.v, mods = d.mod || [];
    var LEFT = PAD + 100, RIGHT = W - PAD, colW = (RIGHT - LEFT) / 12;
    var vals = [];
    mods.forEach(function (m) { if (m.ok) vals.push(m.va, m.vb); });
    if (has(v, 'cmax')) vals.push(v.cmax.v);
    if (has(v, 'cmin')) vals.push(v.cmin.v);
    if (!vals.length) {
      fitText(g, 'ราย module: อ่านไม่ได้', PAD, y + 30, '400', 22, MUTED, W - 2 * PAD);
      return;
    }
    var lo = Math.min.apply(null, vals), hi = Math.max.apply(null, vals);
    var span = Math.max(hi - lo, 0.01);
    lo -= span * 0.15; hi += span * 0.15;
    function yy(x) { return y + CH - (x - lo) / (hi - lo) * CH; }

    g.fillStyle = LINE; g.fillRect(LEFT, y, 1, CH); g.fillRect(LEFT, y + CH, RIGHT - LEFT, 1);
    fitText(g, hi.toFixed(3) + ' V', PAD, y + 16, '400', 18, MUTED, 95);
    fitText(g, lo.toFixed(3) + ' V', PAD, y + CH, '400', 18, MUTED, 95);

    g.save();
    g.setLineDash([8, 6]); g.strokeStyle = MUTED; g.lineWidth = 1.5;
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
        g.fillRect(cx - 3, a, 6, Math.max(b - a, 2));
        g.beginPath(); g.arc(cx, a, 5, 0, Math.PI * 2); g.fill();
        g.beginPath(); g.arc(cx, b, 5, 0, Math.PI * 2); g.fill();
      } else {
        fitText(g, '—', cx, y + CH / 2, '400', 22, MUTED, colW - 6);
      }
      fitText(g, 'M' + (i + 1), cx, y + CH + 28, '700', 18, INK, colW - 6);
      g.textAlign = 'left';
    });
    fitText(g, 'ราย module · เส้นประ = เซลล์สูงสุด/ต่ำสุดของทั้งแพ็ก · ค่าราย module ยังไม่ยืนยันสูตร',
            PAD, y + CH + 62, '400', 20, MUTED, W - 2 * PAD);
  }

  /* ------------------------------ the page ------------------------------ */

  function render(d, when, no) {
    var v = d.v, showMods = SHOW_MODULES || d.demo;
    var s = spreadMv(v);
    var inner = W - 2 * PAD;

    /* Summary: large type, for buyers. */
    var summary = [
      { t: 'สุขภาพแบตเตอรี่ (SOH)', v: num(v, 'soh', 1, '%'),
        c: 'ค่าที่ระบบแบตเตอรี่ของรถรายงาน (ยังไม่ได้เทียบกับศูนย์บริการ)' },
      { t: 'เลขไมล์', v: has(v, 'odo') ? commas(v.odo.v) + ' กม.' : NA,
        c: 'ข้อมูลประกอบ · อ่านจากชุดควบคุมมอเตอร์' },
      { t: 'SOC (ค่าดิบ)', v: num(v, 'soc', 1, '%'),
        c: 'ค่าดิบจากระบบแบตเตอรี่ · ตัวเลขบนหน้าปัดอาจต่างจากนี้' },
      { t: 'ส่วนต่างแรงดันเซลล์ (สูงสุด − ต่ำสุด)', v: s === null ? NA : s + ' mV',
        c: 'ตัวเลขดิบ · รายละเอียดด้านล่าง' }
    ];
    /* Technical detail: small type. */
    var cellIdx = function (k) { return has(v, k) && v[k].i >= 0 ? '  ·  เซลล์ #' + v[k].i : ''; };
    var tech = [
      ['แรงดันเซลล์สูงสุด', num(v, 'cmax', 3, 'V') + cellIdx('cmax')],
      ['แรงดันเซลล์ต่ำสุด', num(v, 'cmin', 3, 'V') + cellIdx('cmin')],
      ['อุณหภูมิแบตเตอรี่', tempRange(v)],
      ['แรงดันแพ็ก', num(v, 'pv', 1, 'V')]
    ];

    var TOP = 490, ROW = 150, KV = 42, CH = 110;
    var yTech = TOP + summary.length * ROW;                 /* rule above tech */
    var yRows = yTech + 70;
    var yMods = yRows + tech.length * KV + 10;
    var yExpl = yMods + (showMods ? CH + 96 : 0);
    var H = Math.min(MAX_H, yExpl + 46 + EXPLAIN.length * 34 + 130);

    var cv = document.createElement('canvas');
    cv.width = W; cv.height = H;
    var g = cv.getContext('2d');
    g.textBaseline = 'alphabetic';
    g.fillStyle = '#ffffff'; g.fillRect(0, 0, W, H);

    /* Header band and identity. */
    g.fillStyle = BAND; g.fillRect(0, 0, W, 230);
    drawLogo(g, PAD, 55, 120);
    fitText(g, 'ตลาดครับ', PAD + 150, 118, '700', 60, '#ffffff', inner - 150);
    fitText(g, 'รายงานตรวจแบตเตอรี่จากระบบรถ', PAD + 150, 178, '400', 38, '#d1d5db', inner - 150);
    [['เลขรายงาน', no], ['วันเวลาที่ตรวจ', thaiDate(when)],
     ['รุ่นรถ', 'MAXUS 9 · โปรไฟล์ ' + d.profile], ['firmware', d.fw]].forEach(function (m, k) {
      var y = 295 + k * 50;
      fitText(g, m[0], PAD, y, '400', 30, MUTED, 250);
      fitText(g, m[1], PAD + 270, y, k === 0 ? '700' : '400', 30, INK, inner - 270);
    });
    rule(g, 475);

    /* Summary rows. */
    summary.forEach(function (r, k) {
      var y0 = TOP + k * ROW;
      fitText(g, r.t, PAD, y0 + 36, '400', 30, MUTED, inner);
      fitText(g, r.v, PAD, y0 + 94, '700', 54, INK, inner);
      fitText(g, r.c, PAD, y0 + 130, '400', 24, MUTED, inner);
      if (k < summary.length - 1) rule(g, y0 + ROW - 6, 1);
    });

    /* Technical detail. */
    rule(g, yTech);
    fitText(g, 'รายละเอียดทางเทคนิค', PAD, yTech + 48, '700', 28, INK, inner);
    tech.forEach(function (r, k) {
      var y = yRows + k * KV + 28;
      fitText(g, r[0], PAD, y, '400', 24, MUTED, 330);
      fitText(g, r[1], PAD + 340, y, '700', 26, INK, inner - 340);
    });
    if (showMods) moduleChart(g, yMods + 10, d, CH);

    fitText(g, 'อ่านค่าเหล่านี้อย่างไร', PAD, yExpl + 34, '700', 24, INK, inner);
    EXPLAIN.forEach(function (t, k) {
      fitText(g, t, PAD, yExpl + 46 + (k + 1) * 34 - 6, '400', 22, MUTED, inner);
    });

    /* Footer. */
    rule(g, H - 100);
    fitText(g, 'อ่านจากพอร์ต OBD ของรถแบบอ่านอย่างเดียว ไม่เปลี่ยนค่าใดในรถ',
            PAD, H - 62, '400', 22, MUTED, inner);
    fitText(g, 'ตัวเลขทั้งหมดเป็นค่าที่ระบบของรถรายงาน ณ เวลาที่ตรวจ',
            PAD, H - 28, '400', 22, MUTED, inner);

    if (d.demo) watermark(g, H);
    return cv;
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

  function toJpeg(cv, name) {
    if (cv.toBlob) {
      cv.toBlob(function (b) { show(URL.createObjectURL(b), name); }, 'image/jpeg', 0.92);
    } else {
      show(cv.toDataURL('image/jpeg', 0.92), name);
    }
  }

  function busy(on) { $('rgo').disabled = on; $('rdemo').disabled = on; }
  function status(t) { $('rst').textContent = t; }

  function run(demo) {
    busy(true);
    $('rout').innerHTML = '';
    status(demo ? 'กำลังสร้างภาพตัวอย่าง…' : 'กำลังอ่านค่าจากรถ… (ราว 10–15 วินาที)');
    fetch('/report' + (demo ? '?demo=1' : ''), { cache: 'no-store' })
      .then(function (r) { return r.json(); })
      .then(function (d) {
        if (!d.ok) { status(ERRORS[d.err] || 'สร้างรายงานไม่ได้'); return; }
        var when = new Date();
        var no = reportNo(when, d.seq, d.demo);
        toJpeg(render(d, when, no), no);
        status((d.demo ? 'ภาพตัวอย่าง (ไม่นับเลขรายงาน) · ' : 'เลขรายงาน ' + no + ' · ') +
               'กด "บันทึกภาพ" หรือกดค้างที่ภาพเพื่อบันทึก');
      })
      .catch(function () { status('เชื่อมต่ออุปกรณ์ไม่ได้ · ตรวจว่ามือถือยังต่อ WiFi MaxusBridge'); })
      .then(function () { busy(false); });
  }

  $('rgo').onclick = function () { run(false); };
  $('rdemo').onclick = function () { run(true); };
})();
