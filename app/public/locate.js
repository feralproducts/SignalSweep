// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 B0z0dcl0wn and the SignalSweep contributors
//
// locate.js -- the maths behind smoother hunting and device pins. No DOM, no
// I/O: pure functions on numbers, so `node app/selftest.js` can check them.
//
//   RssiFilter      a small Kalman filter that turns jumpy RSSI into a steady
//                   reading without lagging behind a real change
//   estimateSource  where a transmitter probably is, from RSSI readings taken
//                   at known GPS positions while you walk (log-distance model,
//                   grid search, with an honest uncertainty radius)
//   fmtDD / fmtDMS / toMGRS   the three coordinate formats a pin is shown in
//
// What the estimate can and cannot do, so nobody over-reads it: one receiver
// and signal strength alone. Bodies, walls, vehicles and how the board is held
// all move RSSI by 10 dB or more, which is a factor of 2-3 in distance. The
// estimate gets usefully tight only when you have walked AROUND the target
// (not just toward it), and its circle widens to say so when you haven't.
// "Found it -- pin here" (your own GPS, standing at the device) is always the
// more precise pin.
(function (root) {
    'use strict';

    // =========================================================================
    //  RSSI smoothing
    // =========================================================================
    // 1-D constant-level Kalman filter. q is how fast the true level is allowed
    // to wander (dB^2 per second: walking toward a device changes it a few dB a
    // second); r is the measurement noise (dB^2: BLE/Wi-Fi RSSI scatters about
    // +/-5 dB from multipath). A reading far outside the current band is
    // down-weighted rather than trusted, so one reflection spike does not throw
    // the meter, but a sustained change still comes through within ~1-2 s.
    function RssiFilter(opts) {
        opts = opts || {};
        this.q = opts.q != null ? opts.q : 6;
        this.r = opts.r != null ? opts.r : 25;
        this.x = null;
        this.p = 0;
        this.t = 0;
    }
    RssiFilter.prototype.update = function (z, tMs) {
        z = Number(z);
        if (!isFinite(z)) return this.x;
        tMs = tMs || Date.now();
        if (this.x === null) { this.x = z; this.p = this.r; this.t = tMs; return this.x; }
        const dt = Math.max(0.05, Math.min(10, (tMs - this.t) / 1000));
        this.t = tMs;
        this.p += this.q * dt;
        const innov = z - this.x;
        // Outlier guard: beyond ~3 sigma, inflate the noise for this reading.
        const s = this.p + this.r;
        const r = (innov * innov > 9 * s) ? this.r * 4 : this.r;
        const k = this.p / (this.p + r);
        this.x += k * innov;
        this.p *= (1 - k);
        return this.x;
    };
    RssiFilter.prototype.value = function () { return this.x; };

    // =========================================================================
    //  Local flat-earth frame (fine over the few hundred metres of a hunt)
    // =========================================================================
    const EARTH_R = 6371008.8;
    const D2R = Math.PI / 180;
    function toLocal(lat0, lng0, lat, lng) {
        return {
            x: (lng - lng0) * D2R * EARTH_R * Math.cos(lat0 * D2R),
            y: (lat - lat0) * D2R * EARTH_R
        };
    }
    function fromLocal(lat0, lng0, x, y) {
        return {
            lat: lat0 + (y / EARTH_R) / D2R,
            lng: lng0 + (x / (EARTH_R * Math.cos(lat0 * D2R))) / D2R
        };
    }
    function metresBetween(lat1, lng1, lat2, lng2) {
        const p = toLocal(lat1, lng1, lat2, lng2);
        return Math.hypot(p.x, p.y);
    }
    function bearingDeg(lat1, lng1, lat2, lng2) {
        const p = toLocal(lat1, lng1, lat2, lng2);
        return (Math.atan2(p.x, p.y) / D2R + 360) % 360;
    }
    function compass(deg) {
        return ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'][Math.round(deg / 45) % 8];
    }

    // =========================================================================
    //  Source estimate
    // =========================================================================
    // samples: [{lat, lng, acc (m), rssi (dBm, smoothed), t}]
    // Returns { ready:false, reason } until there is enough to say anything,
    // then { ready:true, lat, lng, r68, r95 (m), n, spread, edge, peak, ... }.
    const CELL_M = 3;            // readings within ~10 ft are one place
    const MIN_CELLS = 6;
    const MIN_SPREAD_M = 8;      // ~26 ft of walking before a guess means anything
    const ANTENNA_H_M = 1.5;     // floor on distance: avoids log(0) at the source
    const PATH_N = [2.0, 2.5, 3.0, 3.5];
    // Prior on P0, the signal strength 1 m from the transmitter. Phones,
    // trackers, cameras and drones all sit roughly -30..-70 dBm at 1 m. Without
    // this, a circle walked AROUND a device (every reading about equally
    // strong) fits a source impossibly far away just as well as one in the
    // middle, and the search picks the far edge.
    const P0_MEAN = -50, P0_MIN = -80, P0_MAX = -15;
    const P0_PRIOR = 0.36;          // = (6 dB reading noise / 10 dB prior)^2
    // Readings a few steps apart are not independent (the same wall shadows
    // them all, and the RSSI filter smooths across them), so the likelihood is
    // flattened by this factor. Calibrated in simulation so the inner circle
    // holds the true position about 2 times in 3 on a decent walk.
    const CORRELATION = 4;

    function thin(samples, lat0, lng0) {
        // Average readings into CELL_M squares so standing still in one spot
        // for a minute doesn't outvote everywhere else you walked.
        const cells = new Map();
        for (const s of samples) {
            if (!s || !isFinite(s.rssi) || !isFinite(s.lat) || !isFinite(s.lng)) continue;
            const p = toLocal(lat0, lng0, s.lat, s.lng);
            const key = Math.round(p.x / CELL_M) + ',' + Math.round(p.y / CELL_M);
            let c = cells.get(key);
            if (!c) { c = { x: 0, y: 0, rssi: 0, acc: 0, n: 0, lat: 0, lng: 0 }; cells.set(key, c); }
            c.x += p.x; c.y += p.y; c.rssi += s.rssi; c.acc += (s.acc || 10); c.n++;
            c.lat += s.lat; c.lng += s.lng;
        }
        const out = [];
        cells.forEach(function (c) {
            out.push({ x: c.x / c.n, y: c.y / c.n, rssi: c.rssi / c.n, acc: c.acc / c.n, n: c.n,
                       lat: c.lat / c.n, lng: c.lng / c.n });
        });
        return out;
    }

    function estimateSource(samples, opts) {
        opts = opts || {};
        if (!samples || samples.length < MIN_CELLS) {
            return { ready: false, reason: 'need more readings', n: samples ? samples.length : 0 };
        }
        const lat0 = samples[0].lat, lng0 = samples[0].lng;
        const cells = thin(samples, lat0, lng0);
        // Peak: the strongest place you've actually been. Robust and easy to
        // trust; shown alongside the model's estimate.
        let peak = null;
        for (const c of cells) if (!peak || c.rssi > peak.rssi) peak = c;
        const peakOut = peak ? { lat: peak.lat, lng: peak.lng, rssi: peak.rssi } : null;
        if (cells.length < MIN_CELLS) {
            return { ready: false, reason: 'walk around more', n: samples.length, cells: cells.length, peak: peakOut };
        }
        let cx = 0, cy = 0;
        for (const c of cells) { cx += c.x; cy += c.y; }
        cx /= cells.length; cy /= cells.length;
        let spread = 0;
        for (const c of cells) spread += (c.x - cx) * (c.x - cx) + (c.y - cy) * (c.y - cy);
        spread = Math.sqrt(spread / cells.length);
        if (spread < MIN_SPREAD_M) {
            return { ready: false, reason: 'walk around more', n: samples.length, cells: cells.length,
                     spread: spread, peak: peakOut };
        }

        // Search area: the walked area plus a margin -- the source is often
        // just outside where you've been, and the edge flag says when it seems
        // to be further out than that.
        let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
        for (const c of cells) {
            if (c.x < minX) minX = c.x; if (c.x > maxX) maxX = c.x;
            if (c.y < minY) minY = c.y; if (c.y > maxY) maxY = c.y;
        }
        const margin = Math.max(25, 0.6 * Math.max(maxX - minX, maxY - minY));
        minX -= margin; maxX += margin; minY -= margin; maxY += margin;

        // Weights: a reading taken with a vague GPS fix says less about where
        // the signal was strong.
        const w = cells.map(function (c) { const a = Math.max(3, c.acc); return Math.sqrt(c.n) / (1 + (a / 8) * (a / 8)); });
        const N = cells.length;
        const meanW = w.reduce(function (a, v) { return a + v; }, 0) / N;

        function solveGrid(x0, x1, y0, y1, steps) {
            const dx = (x1 - x0) / (steps - 1), dy = (y1 - y0) / (steps - 1);
            const pts = [];
            let best = null;
            const L = new Float64Array(N);
            for (let iy = 0; iy < steps; iy++) {
                const gy = y0 + iy * dy;
                for (let ix = 0; ix < steps; ix++) {
                    const gx = x0 + ix * dx;
                    for (let i = 0; i < N; i++) {
                        const ddx = cells[i].x - gx, ddy = cells[i].y - gy;
                        L[i] = 5 * Math.log10(ddx * ddx + ddy * ddy + ANTENNA_H_M * ANTENNA_H_M); // 10*log10(d)
                    }
                    // Best path-loss exponent here, with P0 in closed form.
                    let bestSse = Infinity, bestN = 0, bestP0 = 0;
                    for (const n of PATH_N) {
                        let sw = 0, swv = 0;
                        for (let i = 0; i < N; i++) { const v = cells[i].rssi + n * L[i]; sw += w[i]; swv += w[i] * v; }
                        const lam = P0_PRIOR * meanW;
                        let p0 = (swv + lam * P0_MEAN) / (sw + lam);
                        if (p0 < P0_MIN) p0 = P0_MIN; else if (p0 > P0_MAX) p0 = P0_MAX;
                        let sse = lam * (p0 - P0_MEAN) * (p0 - P0_MEAN);
                        for (let i = 0; i < N; i++) { const e = cells[i].rssi - (p0 - n * L[i]); sse += w[i] * e * e; }
                        if (sse < bestSse) { bestSse = sse; bestN = n; bestP0 = p0; }
                    }
                    const pt = { x: gx, y: gy, sse: bestSse, n: bestN, p0: bestP0, ix: ix, iy: iy };
                    pts.push(pt);
                    if (!best || bestSse < best.sse) best = pt;
                }
            }
            return { pts: pts, best: best, dx: dx, dy: dy, steps: steps };
        }

        const coarse = solveGrid(minX, maxX, minY, maxY, 61);
        const b = coarse.best;
        const fine = solveGrid(b.x - 2 * coarse.dx, b.x + 2 * coarse.dx, b.y - 2 * coarse.dy, b.y + 2 * coarse.dy, 21);
        const best = fine.best.sse <= b.sse ? fine.best : b;

        // Uncertainty: treat exp(-dSSE / 2 sigma^2) over the coarse grid as a
        // likelihood and take its spread. sigma is the residual scatter at the
        // best point, floored at 4 dB (RSSI is never better than that).
        // SSE is weight-scaled; dividing by the mean weight puts it back in
        // dB^2 so chi-square = SSE / (meanW * sigma^2).
        const sigma2 = Math.max(16, best.sse / meanW / Math.max(1, N - 3));
        let sw = 0, mx = 0, my = 0;
        const lik = coarse.pts.map(function (p) {
            const l = Math.exp(-(p.sse - best.sse) / (2 * meanW * sigma2 * CORRELATION));
            sw += l; mx += l * p.x; my += l * p.y;
            return l;
        });
        mx /= sw; my /= sw;
        let sxx = 0, syy = 0, sxy = 0;
        coarse.pts.forEach(function (p, i) {
            const l = lik[i] / sw;
            sxx += l * (p.x - mx) * (p.x - mx);
            syy += l * (p.y - my) * (p.y - my);
            sxy += l * (p.x - mx) * (p.y - my);
        });
        // Largest axis of the covariance ellipse, plus a GPS floor.
        const tr = sxx + syy, det = sxx * syy - sxy * sxy;
        const lmax = tr / 2 + Math.sqrt(Math.max(0, tr * tr / 4 - det));
        let medAcc = cells.map(function (c) { return c.acc; }).sort(function (a, c) { return a - c; })[Math.floor(cells.length / 2)];
        const sMax = Math.sqrt(Math.max(lmax, 0) + 0.5 * medAcc * medAcc);
        const r68 = Math.max(3, 1.515 * sMax);
        const r95 = Math.max(5, 2.448 * sMax);

        // On the edge of the search box: the source is probably further out
        // than you have walked; say which way.
        const edge = b.ix <= 1 || b.iy <= 1 || b.ix >= coarse.steps - 2 || b.iy >= coarse.steps - 2;
        const ll = fromLocal(lat0, lng0, best.x, best.y);
        const centroid = fromLocal(lat0, lng0, cx, cy);
        // Contrast: how much stronger the best place you walked was than the
        // worst. Under ~8 dB the readings are mostly noise and any position is
        // a weak guess -- e.g. circling at a constant distance.
        let rMin = Infinity, rMax = -Infinity;
        for (const c of cells) { if (c.rssi < rMin) rMin = c.rssi; if (c.rssi > rMax) rMax = c.rssi; }
        const contrast = rMax - rMin;
        const quality = (edge || contrast < 8) ? 'rough'
                      : (r68 <= 12 && contrast >= 12) ? 'good'
                      : (r68 <= 30) ? 'fair' : 'rough';
        return {
            ready: true,
            lat: ll.lat, lng: ll.lng,
            r68: r68, r95: r95,
            n: samples.length, cells: N, spread: spread,
            pathN: best.n, p0: best.p0, sigma: Math.sqrt(sigma2),
            edge: edge, contrast: contrast, quality: quality,
            edgeBearing: edge ? bearingDeg(centroid.lat, centroid.lng, ll.lat, ll.lng) : null,
            peak: peakOut
        };
    }

    // =========================================================================
    //  Coordinate formats
    // =========================================================================
    function fmtDD(lat, lng, places) {
        const p = places == null ? 6 : places;
        return Number(lat).toFixed(p) + ', ' + Number(lng).toFixed(p);
    }

    function dmsPart(v, pos, neg) {
        const hemi = v >= 0 ? pos : neg;
        let a = Math.abs(v);
        let d = Math.floor(a);
        let mFull = (a - d) * 60;
        let m = Math.floor(mFull);
        let s = (mFull - m) * 60;
        // Round seconds to 0.1 and carry, so 59.96" never prints as 60.0".
        s = Math.round(s * 10) / 10;
        if (s >= 60) { s -= 60; m += 1; }
        if (m >= 60) { m -= 60; d += 1; }
        return d + '°' + String(m).padStart(2, '0') + '′' + s.toFixed(1).padStart(4, '0') + '″' + hemi;
    }
    function fmtDMS(lat, lng) {
        return dmsPart(Number(lat), 'N', 'S') + ' ' + dmsPart(Number(lng), 'E', 'W');
    }

    // ---- UTM (WGS84), Kruger series (Karney 2011), sub-millimetre ----------
    const WGS_A = 6378137.0, WGS_F = 1 / 298.257223563, K0 = 0.9996;
    function toUTM(lat, lng, forceZone) {
        const n = WGS_F / (2 - WGS_F);
        const A = WGS_A / (1 + n) * (1 + n * n / 4 + n * n * n * n / 64);
        const alpha = [
            n / 2 - 2 * n * n / 3 + 5 * n * n * n / 16,
            13 * n * n / 48 - 3 * n * n * n / 5,
            61 * n * n * n / 240
        ];
        let zone = forceZone || Math.floor((lng + 180) / 6) + 1;
        if (zone > 60) zone = 60;
        if (!forceZone) {
            // Norway / Svalbard exceptions.
            if (lat >= 56 && lat < 64 && lng >= 3 && lng < 12) zone = 32;
            if (lat >= 72 && lat < 84) {
                if (lng >= 0 && lng < 9) zone = 31;
                else if (lng >= 9 && lng < 21) zone = 33;
                else if (lng >= 21 && lng < 33) zone = 35;
                else if (lng >= 33 && lng < 42) zone = 37;
            }
        }
        const lng0 = (zone - 1) * 6 - 180 + 3;
        const phi = lat * D2R, lam = (lng - lng0) * D2R;
        const e = Math.sqrt(WGS_F * (2 - WGS_F));
        const t = Math.sinh(Math.atanh(Math.sin(phi)) - e * Math.atanh(e * Math.sin(phi)));
        const xiP = Math.atan2(t, Math.cos(lam));
        const etaP = Math.atanh(Math.sin(lam) / Math.sqrt(1 + t * t));
        let xi = xiP, eta = etaP;
        for (let j = 1; j <= 3; j++) {
            xi += alpha[j - 1] * Math.sin(2 * j * xiP) * Math.cosh(2 * j * etaP);
            eta += alpha[j - 1] * Math.cos(2 * j * xiP) * Math.sinh(2 * j * etaP);
        }
        const easting = 500000 + K0 * A * eta;
        let northing = K0 * A * xi;
        if (lat < 0) northing += 10000000;
        return { zone: zone, easting: easting, northing: northing, south: lat < 0 };
    }

    const BANDS = 'CDEFGHJKLMNPQRSTUVWX';
    function latBand(lat) {
        if (lat >= 72) return 'X';           // X is 12 degrees tall (72-84)
        return BANDS[Math.floor((lat + 80) / 8)];
    }

    // MGRS to `digits` per axis (5 = 1 m). Valid -80..84 latitude; the polar
    // UPS areas are outside what anyone hunts a transmitter in.
    function toMGRS(lat, lng, digits) {
        lat = Number(lat); lng = Number(lng);
        const d = digits == null ? 5 : digits;
        if (!(lat >= -80 && lat <= 84) || !isFinite(lng)) return '';
        const u = toUTM(lat, lng);
        const set = (u.zone - 1) % 6 + 1;
        const colLetters = ['ABCDEFGH', 'JKLMNPQR', 'STUVWXYZ'][(set - 1) % 3];
        const col = Math.floor(u.easting / 100000);
        const colL = colLetters[(col - 1) % 8];
        const rowLetters = 'ABCDEFGHJKLMNPQRSTUV';
        let row = Math.floor(u.northing / 100000) % 20;
        if (set % 2 === 0) row = (row + 5) % 20;   // even sets start at F
        const rowL = rowLetters[row];
        const div = Math.pow(10, 5 - d);
        const e = Math.floor((u.easting % 100000) / div);
        const nn = Math.floor((u.northing % 100000) / div);
        const pad = function (v) { return String(v).padStart(d, '0'); };
        return u.zone + latBand(lat) + ' ' + colL + rowL + (d ? ' ' + pad(e) + ' ' + pad(nn) : '');
    }

    // Units for display: imperial, as the operator asked.
    function fmtFeet(m) {
        if (m == null || !isFinite(m)) return '?';
        const ft = m * 3.28084;
        if (ft >= 1000) return (ft / 5280).toFixed(ft >= 5280 * 10 ? 0 : 1) + ' mi';
        return Math.round(ft) + ' ft';
    }

    root.SSLocate = {
        RssiFilter: RssiFilter,
        estimateSource: estimateSource,
        toLocal: toLocal, fromLocal: fromLocal,
        metresBetween: metresBetween, bearingDeg: bearingDeg, compass: compass,
        fmtDD: fmtDD, fmtDMS: fmtDMS, toMGRS: toMGRS, toUTM: toUTM, fmtFeet: fmtFeet
    };
})(typeof window !== 'undefined' ? window : globalThis);
