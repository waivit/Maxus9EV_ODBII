/*
 * Battery inspection report (taladkrub, hub handoff H-0135).
 * Fetches one fresh snapshot from /report, draws it on a canvas and hands the
 * phone a JPEG. Runs on the ESP32's own WiFi with no internet: no libraries,
 * Thai text uses the phone's system fonts, the logo is drawn from its SVG paths
 * (so the canvas is never tainted and toBlob always works).
 *
 * Content rules from Design: no VIN, no good/bad colouring of values, no
 * conclusion about battery replacement, SOH and odometer carry fixed captions.
 */
(function () {
  'use strict';

  var W = 1080, PAD = 64, ROW = 165, TOP = 485;
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

  /* Rows of the report, in Design's order. */
  function rows(v) {
    function ok(k) { return !!(v[k] && v[k].ok); }
    function num(k, dec, unit) { return ok(k) ? v[k].v.toFixed(dec) + ' ' + unit : NA; }
    function idx(k, word) {
      return ok(k) && v[k].i >= 0 ? word + 'หมายเลข ' + v[k].i + ' (ตามที่ระบบแบตเตอรี่รายงาน)' : '';
    }
    var spread = ok('cmax') && ok('cmin')
      ? Math.round((v.cmax.v - v.cmin.v) * 1000) + ' mV' : NA;
    return [
      { t: 'สุขภาพแบตเตอรี่ (SOH)', v: num('soh', 1, '%'),
        c: 'ค่าที่ระบบแบตเตอรี่ของรถรายงาน (ยังไม่ได้เทียบกับศูนย์บริการ)' },
      { t: 'เลขไมล์', v: ok('odo') ? commas(v.odo.v) + ' กม.' : NA,
        c: 'ข้อมูลประกอบ · อ่านจากชุดควบคุมมอเตอร์' },
      { t: 'แรงดันเซลล์สูงสุด', v: num('cmax', 3, 'V'), c: idx('cmax', 'เซลล์') },
      { t: 'แรงดันเซลล์ต่ำสุด', v: num('cmin', 3, 'V'), c: idx('cmin', 'เซลล์') },
      { t: 'ส่วนต่างแรงดันเซลล์ (สูงสุด − ต่ำสุด)', v: spread, c: 'ตัวเลขดิบ' },
      { t: 'อุณหภูมิแบตเตอรี่สูงสุด', v: num('thi', 1, '°C'), c: idx('thi', 'เซนเซอร์') },
      { t: 'อุณหภูมิแบตเตอรี่ต่ำสุด', v: num('tlo', 1, '°C'), c: idx('tlo', 'เซนเซอร์') },
      { t: 'SOC (ค่าดิบ)', v: num('soc', 1, '%'),
        c: 'ค่าดิบจากระบบแบตเตอรี่ · ตัวเลขบนหน้าปัดอาจต่างจากนี้' }
    ];
  }

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

  function render(d, when, no) {
    var list = rows(d.v);
    var H = TOP + list.length * ROW + 150;      /* 1955 px: under Design's 2000 */
    var cv = document.createElement('canvas');
    cv.width = W; cv.height = H;
    var g = cv.getContext('2d');
    var inner = W - 2 * PAD;
    g.textBaseline = 'alphabetic';

    g.fillStyle = '#ffffff'; g.fillRect(0, 0, W, H);

    /* Header band: logo, brand, title. */
    g.fillStyle = BAND; g.fillRect(0, 0, W, 230);
    drawLogo(g, PAD, 55, 120);
    fitText(g, 'ตลาดครับ', PAD + 150, 118, '700', 60, '#ffffff', inner - 150);
    fitText(g, 'รายงานตรวจแบตเตอรี่จากระบบรถ', PAD + 150, 178, '400', 38, '#d1d5db', inner - 150);

    /* Report identity. */
    var meta = [
      ['เลขรายงาน', no],
      ['วันเวลาที่ตรวจ', thaiDate(when)],
      ['รุ่นรถ', 'MAXUS 9 · โปรไฟล์ ' + d.profile],
      ['firmware', d.fw]
    ];
    meta.forEach(function (m, k) {
      var y = 295 + k * 50;
      fitText(g, m[0], PAD, y, '400', 30, MUTED, 250);
      fitText(g, m[1], PAD + 270, y, k === 0 ? '700' : '400', 30, INK, inner - 270);
    });
    g.fillStyle = LINE; g.fillRect(PAD, 475, inner, 2);

    /* Values: label, big number, caption. One ink colour for every value. */
    list.forEach(function (r, k) {
      var y0 = TOP + k * ROW;
      fitText(g, r.t, PAD, y0 + 38, '400', 30, MUTED, inner);
      fitText(g, r.v, PAD, y0 + 100, '700', 56, INK, inner);
      if (r.c) fitText(g, r.c, PAD, y0 + 140, '400', 26, MUTED, inner);
      if (k < list.length - 1) { g.fillStyle = LINE; g.fillRect(PAD, y0 + ROW - 4, inner, 1); }
    });

    /* Footer: how the values were obtained. */
    g.fillStyle = LINE; g.fillRect(PAD, H - 130, inner, 2);
    fitText(g, 'อ่านจากพอร์ต OBD ของรถแบบอ่านอย่างเดียว ไม่เปลี่ยนค่าใดในรถ',
            PAD, H - 82, '400', 24, MUTED, inner);
    fitText(g, 'ตัวเลขทั้งหมดเป็นค่าที่ระบบของรถรายงาน ณ เวลาที่ตรวจ',
            PAD, H - 44, '400', 24, MUTED, inner);

    if (d.demo) {
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
    return cv;
  }

  var lastUrl = null;
  function show(url, name) {
    if (lastUrl && lastUrl.indexOf('blob:') === 0) URL.revokeObjectURL(lastUrl);
    lastUrl = url;
    var img = $('rimg'), dl = $('rdl');
    img.src = url; img.style.display = 'block';
    dl.href = url; dl.download = name + '.jpg'; dl.style.display = 'inline-block';
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
    status(demo ? 'กำลังสร้างภาพตัวอย่าง…' : 'กำลังอ่านค่าจากรถ… (ราว 5–10 วินาที)');
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
