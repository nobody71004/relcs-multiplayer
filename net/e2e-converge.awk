# net/e2e-converge.awk — time-aligned, staleness-aware convergence check over
# relcs-netbot telemetry logs (used by e2e-bots.sh).
#
#   awk -v MODE=player  -f e2e-converge.awk a.log b.log ...
#   awk -v MODE=vehicle -f e2e-converge.awk driver.log watcher.log ...
#
# MODE=player : every bot is a target (its own "IAM x y z" samples, id from its
#               WELCOME line) and an observer ("SEES <id> x y z age=").
# MODE=vehicle: a driver bot is a target through "IAMVEH <vid> x y z", every
#               bot is an observer through "SEESVEH <vid> x y z age=".
#
# Every observation carries the age of the relayed snapshot, so the allowed
# drift is bounded by how far the target itself travelled during that stale
# window. Windows that contain a logged JUMP (respawn / correction / scripted
# relocation) are skipped as unverifiable; anything beyond the age-bounded
# drift is a genuine relay failure. Optional: -v MIN=<n> verifiable samples
# required (default 6), -v SEG=<m> largest plausible 1 Hz step (8 / 60 m).
function dist(x1,y1,z1,x2,y2,z2, dx,dy,dz) {
	dx=x1-x2; dy=y1-y2; dz=z1-z2
	return sqrt(dx*dx+dy*dy+dz*dz)
}
function interp(f, tt,   m, s2, w2, nq) {
	nq = nt[f]
	if (nq < 1) { gx = 0; gy = 0; gz = 0; return }
	if (tt <= iat[f SUBSEP 1])  { gx = iax[f SUBSEP 1];  gy = iay[f SUBSEP 1];  gz = iaz[f SUBSEP 1];  return }
	if (tt >= iat[f SUBSEP nq]) { gx = iax[f SUBSEP nq]; gy = iay[f SUBSEP nq]; gz = iaz[f SUBSEP nq]; return }
	for (m = 1; m < nq; m++) {
		if (iat[f SUBSEP m] <= tt && tt <= iat[f SUBSEP (m+1)]) {
			s2 = iat[f SUBSEP (m+1)] - iat[f SUBSEP m]
			w2 = (s2 > 0) ? (tt - iat[f SUBSEP m]) / s2 : 0
			gx = iax[f SUBSEP m] + (iax[f SUBSEP (m+1)] - iax[f SUBSEP m]) * w2
			gy = iay[f SUBSEP m] + (iay[f SUBSEP (m+1)] - iay[f SUBSEP m]) * w2
			gz = iaz[f SUBSEP m] + (iaz[f SUBSEP (m+1)] - iaz[f SUBSEP m]) * w2
			return
		}
	}
}
function stamp(s) { gsub(/[\[\]]/, "", s); return s + 0 }
BEGIN {
	if (MODE == "") MODE = "player"
	if (MIN == "") MIN = 6
	# a 1 Hz telemetry step longer than this is an unlogged discontinuity
	# (walking bots: 8 m; cars cruise at up to 40 m/s)
	if (SEG == "") SEG = (MODE == "vehicle") ? 60 : 8
}
{ files[FILENAME] = 1 }
MODE == "player" && /WELCOME id=/ { split($3, wp, "="); ids[FILENAME] = wp[2] + 0 }
MODE == "player" && $2 == "IAM" {
	nt[FILENAME] += 1; k = FILENAME SUBSEP nt[FILENAME]
	iat[k] = stamp($1); iax[k] = $3 + 0; iay[k] = $4 + 0; iaz[k] = $5 + 0
}
MODE == "vehicle" && $2 == "IAMVEH" {
	ids[FILENAME] = $3 + 0
	nt[FILENAME] += 1; k = FILENAME SUBSEP nt[FILENAME]
	iat[k] = stamp($1); iax[k] = $4 + 0; iay[k] = $5 + 0; iaz[k] = $6 + 0
}
(MODE == "player" && $2 == "SEES") || (MODE == "vehicle" && $2 == "SEESVEH") {
	key = FILENAME "|" ($3 + 0); ns[key] += 1; k = key SUBSEP ns[key]
	sst[k] = stamp($1); ssx[k] = $4 + 0; ssy[k] = $5 + 0; ssz[k] = $6 + 0; sage[k] = 0
	for (fld = 7; fld <= NF; fld++) if ($fld ~ /^age=/) { split($fld, wa, "="); sage[k] = wa[2] + 0 }
}
$2 == "JUMP" { nj[FILENAME] += 1; jt[FILENAME SUBSEP nj[FILENAME]] = stamp($1) }
END {
	worst = 0; worstratio = 0; n = 0; rej = 0; bad = 0
	for (f1 in ids) for (f2 in files) {
		if (f1 == f2) continue
		key = f2 "|" ids[f1]
		pairworst = 0; pairn = 0
		for (i = 1; i <= ns[key]; i++) {
			sk = key SUBSEP i
			# a snapshot that never had a relayed state (spawn position only)
			if (sage[sk] < 0) { rej++; continue }
			# target own position at the observation instant (time-aligned)
			interp(f1, sst[sk]); ix = gx; iy = gy; iz = gz
			d = dist(ssx[sk],ssy[sk],ssz[sk], ix, iy, iz)
			# drift budget = target path over the snapshot stale window
			tlo = sst[sk] - (sage[sk] + 0.3)
			interp(f1, tlo); cx = gx; cy = gy; cz = gz
			path = 0; jump = 0
			for (q = 1; q <= nt[f1]; q++) {
				tq = iat[f1 SUBSEP q]
				if (tq <= tlo || tq >= sst[sk]) continue
				seg = dist(cx,cy,cz, iax[f1 SUBSEP q],iay[f1 SUBSEP q],iaz[f1 SUBSEP q])
				if (seg > SEG) jump = 1
				path += seg
				cx = iax[f1 SUBSEP q]; cy = iay[f1 SUBSEP q]; cz = iaz[f1 SUBSEP q]
			}
			seg = dist(cx,cy,cz, ix,iy,iz)
			if (seg > SEG) jump = 1
			path += seg
			# samples whose stale window OR interpolation bracket overlaps a
			# logged own position jump cannot be verified at 1 Hz telemetry —
			# widen by one bracket (1.2 s) either side
			for (e = 1; e <= nj[f1]; e++) {
				tj = jt[f1 SUBSEP e]
				if (tj >= tlo - 1.2 && tj <= sst[sk] + 1.2) jump = 1
			}
			if (jump) { rej++; continue }
			tol = 2.0 + path
			n++; pairn++
			if (d > pairworst) pairworst = d
			if (d > worst) worst = d
			if (tol > 0 && d / tol > worstratio) worstratio = d / tol
			if (d > tol) {
				bad = 1
				printf "convergence VIOLATION: %s sees %s (id %d) off by %.2f m (tol %.2f, stale %.2f s)\n", f2, f1, ids[f1], d, tol, sage[sk]
			}
		}
		if (pairn > 0)
			printf "convergence: %s sees %s (id %d): %d samples, worst %.2f m\n", f2, f1, ids[f1], pairn, pairworst
	}
	if (n < MIN) { printf "FAIL: only %d verifiable %s convergence samples (need >= %d)\n", n, MODE, MIN; exit 1 }
	if (bad)     { printf "FAIL: %s convergence violated (worst %.2f m, %.0f%% of tolerance)\n", MODE, worst, worstratio * 100; exit 1 }
	printf "%s convergence OK (%d samples, %d stale-jump skipped, worst %.2f m = %.0f%% of tolerance)\n", MODE, n, rej, worst, worstratio * 100
	exit 0
}
