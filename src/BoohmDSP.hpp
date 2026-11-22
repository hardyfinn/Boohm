// Boohm kick voice DSP. Plain C++ with no Rack dependency, so the same code can
// be rendered to WAV files by test/render.cpp for listening checks.
//
// The control set follows the Ohmforce Bohm kick module (HIT, VELOCITY, LENGTH,
// SUSTAIN, ATTACK, PITCH, CURVE, COLOR, TRS DECAY, TRS TONE, FX, STEREO); the
// sound engines and their maths are our own.
//
// 2.1.0: revised after a DSP review (sample-rate independent click and room,
// end-of-kick fade, mono sub, two-stage CURVE, DC-free TUBE, level-matched FX,
// band-limited GRIT tables, FM index limit, smoothed level controls).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace boohm {

static const float PI = 3.14159265358979f;
static const float TWO_PI = 6.28318530717959f;
static const float C1_HZ = 32.7032f;

enum Model { ANLG = 0, FM2, MEMB, GRIT, NUM_MODELS };
enum FxType { FX_TUBE = 0, FX_BASS, FX_SOFT, FX_HARD, FX_FOLD, FX_CRUSH, NUM_FX };

inline float clampf(float x, float a, float b) { return std::min(std::max(x, a), b); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
// 0..1 knob to an exponential range lo..hi
inline float expmap(float k, float lo, float hi) { return lo * std::pow(hi / lo, clampf(k, 0.f, 1.f)); }

// Small fast random generator (xorshift), one per noise source so the two
// stereo channels get different noise.
struct Noise {
	uint32_t s;
	explicit Noise(uint32_t seed = 22222) : s(seed) {}
	float white() {
		s ^= s << 13; s ^= s >> 17; s ^= s << 5;
		return (int32_t) s * (1.f / 2147483648.f);
	}
};

// Trapezoidal (TPT / zero-delay) state-variable filter for the click tone.
// Unlike the old Chamberlin filter it is stable right up to Nyquist, so the
// whole TRS TONE range works at any sample rate.
struct SVF {
	float ic1 = 0.f, ic2 = 0.f;
	float g = 0.1f, k = 0.9f, a1 = 1.f;
	void reset() { ic1 = ic2 = 0.f; }
	void setCutoff(float cut, float sr) {
		g = std::tan(PI * std::min(cut, 0.45f * sr) / sr);
		a1 = 1.f / (1.f + g * (g + k));
	}
	void process(float x, float& lp, float& bp, float& hp) {
		float v1 = a1 * (ic1 + g * (x - ic2));
		float v2 = ic2 + g * v1;
		ic1 = 2.f * v1 - ic1;
		ic2 = 2.f * v2 - ic2;
		lp = v2;
		bp = v1;
		hp = x - k * v1 - v2;
	}
};

// One-pole low-pass (used for crossover / DC blocking / side high-pass)
struct OnePole {
	float z = 0.f;
	float lp(float x, float a) { z += a * (x - z); return z; }
	float hp(float x, float a) { z += a * (x - z); return x - z; }
};

// 2-sample polyBLEP residual for a step at phase 0 (dt = phase increment)
inline float polyBlep(float p, float dt) {
	if (dt <= 0.f) return 0.f;
	if (p < dt) { float x = p / dt; return x + x - x * x - 1.f; }
	if (p > 1.f - dt) { float x = (p - 1.f) / dt; return x * x + x + x + 1.f; }
	return 0.f;
}

// The eight FM modulator shapes (in the spirit of the classic OPL chip waves).
// dt = modulator phase increment, used to smooth the saw's jump.
inline float fmWave(int w, float ph, float dt) {
	float p = ph - std::floor(ph);
	float s = std::sin(TWO_PI * p);
	switch (w) {
		default:
		case 0: return s;                                   // sine
		case 1: return s > 0.f ? s : 0.f;                   // half sine
		case 2: return std::fabs(s);                        // absolute sine
		case 3: return (std::fmod(p, 0.5f) < 0.25f) ? std::fabs(s) : 0.f; // quarter pulses
		case 4: return p < 0.5f ? std::sin(2.f * TWO_PI * p) : 0.f;       // alternating sine
		case 5: return p < 0.5f ? std::fabs(std::sin(2.f * TWO_PI * p)) : 0.f; // camel
		case 6: return std::tanh(3.f * s) / std::tanh(3.f);  // soft square
		case 7: return 1.f - 2.f * p + polyBlep(p, dt);       // band-limited saw (falling)
	}
}

static const float TANH26 = 0.98903f;  // tanh(2.6)

// ANLG body: morph sine -> triangle -> warm saturated sine -> squarish
inline float anlgWave(float ph, float color) {
	float s = std::sin(TWO_PI * ph);
	float p = ph - std::floor(ph);
	float tri = (p < 0.25f) ? 4.f * p : (p < 0.75f ? 2.f - 4.f * p : 4.f * p - 4.f);
	float c = clampf(color, 0.f, 1.f) * 3.f;
	if (c < 1.f) return lerpf(s, tri, c);
	float sat = std::tanh(2.6f * s) / TANH26;
	if (c < 2.f) return lerpf(tri, sat, c - 1.f);
	float sq = std::tanh(9.f * s);
	return lerpf(sat, sq, c - 2.f) * lerpf(1.f, 0.85f, c - 2.f);
}

// FM-2 carrier: COLOR goes sine -> triangle -> square
inline float fmCarrier(float ph, float color) {
	float s = std::sin(TWO_PI * ph);
	float p = ph - std::floor(ph);
	float tri = (p < 0.25f) ? 4.f * p : (p < 0.75f ? 2.f - 4.f * p : 4.f * p - 4.f);
	float c = clampf(color, 0.f, 1.f);
	if (c < 0.5f) return lerpf(s, tri, c * 2.f);
	float sq = std::tanh(7.f * s) * 0.85f;
	return lerpf(tri, sq, (c - 0.5f) * 2.f);
}

// GRIT: a bank of eight digital single-cycle tables, scanned by COLOR.
// Each table is stored at four band-limited levels (at most 170, 64, 16 and 4
// harmonics) so the bright tables don't alias during the pitch sweep.
struct GritTables {
	static const int N = 512;
	static const int T = 8;
	static const int LV = 4;
	float t[LV][T][N + 1];
	float maxH[LV] = {170.f, 64.f, 16.f, 4.f};
	GritTables() {
		static float raw[N];
		static float sn[N], cs[N];
		for (int i = 0; i < N; i++) {
			sn[i] = std::sin(TWO_PI * i / N);
			cs[i] = std::cos(TWO_PI * i / N);
		}
		for (int tb = 0; tb < T; tb++) {
			for (int i = 0; i < N; i++) {
				float p = (float) i / N;
				float x = TWO_PI * p;
				float s = std::sin(x);
				float v;
				switch (tb) {
					default:
					case 0: v = s; break;                                                  // sine
					case 1: v = std::round(s * 4.f) / 4.f; break;                          // stepped (low-bit) sine
					case 2: {                                                              // phase-distorted (bright, saw-like)
						float pd = p < 0.12f ? p / 0.12f * 0.5f : 0.5f + (p - 0.12f) / 0.88f * 0.5f;
						v = std::sin(TWO_PI * pd);
					} break;
					case 3: v = (s + std::sin(3 * x) / 3.f + std::sin(5 * x) / 5.f) * 0.9f; break; // odd stack
					case 4: v = std::sin(3.f * s); break;                                   // folded sine
					case 5: v = s * (0.6f + 0.4f * std::sin(7 * x)); break;                 // formant ripple
					case 6: v = std::tanh(4.f * (s + 0.5f * std::sin(2 * x))) * 0.8f; break; // asymmetric pulse
					case 7: v = std::sin(x + 2.2f * std::sin(5 * x)) * 0.8f; break;         // metallic FM table
				}
				raw[i] = v;
			}
			// harmonic analysis (plain DFT, done once at start-up)
			static float re[N / 2], im[N / 2];
			for (int h = 1; h < N / 2; h++) {
				float a = 0.f, b = 0.f;
				for (int i = 0; i < N; i++) {
					int idx = (h * i) % N;
					a += raw[i] * cs[idx];
					b += raw[i] * sn[idx];
				}
				re[h] = a * 2.f / N;
				im[h] = b * 2.f / N;
			}
			float dc = 0.f;
			for (int i = 0; i < N; i++) dc += raw[i];
			dc /= N;
			for (int lv = 0; lv < LV; lv++) {
				int H = std::min(N / 2 - 1, (int) maxH[lv]);
				for (int i = 0; i < N; i++) {
					float v = dc;
					for (int h = 1; h <= H; h++) {
						int idx = (h * i) % N;
						v += re[h] * cs[idx] + im[h] * sn[idx];
					}
					t[lv][tb][i] = v;
				}
				t[lv][tb][N] = t[lv][tb][0];
			}
		}
	}
	float readLevel(int lv, float ph, float pos) const {
		float p = ph - std::floor(ph);
		float fi = p * N;
		int i = (int) fi;
		float fr = fi - i;
		float tp = clampf(pos, 0.f, 1.f) * (T - 1);
		int a = (int) tp;
		if (a >= T - 1) a = T - 2;
		float tf = tp - a;
		const float* A = t[lv][a];
		const float* B = t[lv][a + 1];
		float va = A[i] + (A[i + 1] - A[i]) * fr;
		float vb = B[i] + (B[i + 1] - B[i]) * fr;
		return lerpf(va, vb, tf);
	}
	// pick (and crossfade) the brightest level whose top harmonic stays below 0.45 * sr.
	// lvA/lvB/mix are worked out once per sample by choose().
	void choose(float f, float sr, int& lvA, int& lvB, float& mix) const {
		float ideal = 0.45f * sr / std::max(f, 1.f);
		if (ideal >= maxH[0]) { lvA = lvB = 0; mix = 0.f; return; }
		for (int i = 1; i < LV; i++) {
			if (maxH[i] <= ideal) {
				lvA = i;           // safe level
				lvB = i - 1;       // brighter level
				mix = std::log(ideal / maxH[i]) / std::log(maxH[i - 1] / maxH[i]);
				return;
			}
		}
		lvA = lvB = LV - 1; mix = 0.f;
	}
	float read(float ph, float pos, int lvA, int lvB, float mix) const {
		float a = readLevel(lvA, ph, pos);
		if (lvA == lvB || mix <= 0.f) return a;
		return lerpf(a, readLevel(lvB, ph, pos), mix);
	}
};

// A tiny room (comb + all-pass reverb) for the MEMB model's ambient mic.
// Delay lengths and damping are scaled for the sample rate, so the room
// sounds the same at 44.1, 48, 96 or 192 kHz. Size is latched per hit.
struct Room {
	static const int MAXD = 16384;
	static const int MAXA = 4096;
	float comb[4][MAXD];
	float ap[2][MAXA];
	int ci[4] = {}, ai[2] = {};
	float damp[4] = {};
	int clen[4], alen[2];
	float dampA = 0.6f;
	int base[4] = {1116, 1188, 1277, 1356};
	Room() { clear(); setSize(1.f, 0, 44100.f); }
	void clear() {
		std::memset(comb, 0, sizeof(comb));
		std::memset(ap, 0, sizeof(ap));
		for (int k = 0; k < 4; k++) { damp[k] = 0.f; ci[k] = 0; }
		ai[0] = ai[1] = 0;
	}
	void setSize(float sizeScale, int spread, float sr) {
		float s = sr / 44100.f;
		for (int k = 0; k < 4; k++) {
			clen[k] = std::min(MAXD - 1, std::max(64, (int) ((base[k] + spread) * sizeScale * s)));
			if (ci[k] >= clen[k]) ci[k] = 0;
		}
		alen[0] = std::min(MAXA - 1, (int) ((556 + spread) * s));
		alen[1] = std::min(MAXA - 1, (int) ((441 + spread) * s));
		for (int k = 0; k < 2; k++) if (ai[k] >= alen[k]) ai[k] = 0;
		dampA = 1.f - std::exp(-TWO_PI * 7000.f / sr);
	}
	float process(float x, float fb) {
		float y = 0.f;
		for (int k = 0; k < 4; k++) {
			float o = comb[k][ci[k]];
			damp[k] += dampA * (o - damp[k]);
			comb[k][ci[k]] = x + damp[k] * fb;
			if (++ci[k] >= clen[k]) ci[k] = 0;
			y += o;
		}
		y *= 0.25f;
		for (int k = 0; k < 2; k++) {
			float b = ap[k][ai[k]];
			float o = -y + b;
			ap[k][ai[k]] = y + b * 0.5f;
			if (++ai[k] >= alen[k]) ai[k] = 0;
			y = o;
		}
		return y;
	}
};

// Everything the voice reads each sample, already mixed knob + CV, 0..1 unless stated.
struct Controls {
	int model = ANLG;
	int fxType = FX_TUBE;
	float pitchOct = 0.25f;  // octaves above C1 (0 = C1, 1 = C2), may go beyond with CV
	bool tracking = false;   // pitchOct is a plain 1V/oct value (already includes knob)
	float curve = 0.35f;     // 0 = 808-style fast sweep, 1 = 909-style
	float length = 0.5f;
	float attack = 0.5f;
	float sustain = 0.8f;    // body level 0..1
	float color = 0.0f;
	float trsDecay = 0.3f;
	float trsTone = 0.5f;
	float velocity = 1.0f;
	float fx = 0.0f;
	float stereo = 0.3f;
	float fmRatio = 2.0f;
};

struct Voice {
	float sr = 48000.f, st = 1.f / 48000.f;
	// state
	bool active = false;
	bool gate = false;
	int model = ANLG;       // latched at each hit
	float t = 0.f;          // seconds since the hit
	float relT = 0.f;       // seconds since release while gate was held
	float holdLevel = 1.f;  // body envelope level while the gate is held
	bool held = false;      // the gate was still high after the first few ms
	float phase = 0.f, modPhase = 0.f, tickPhase = 0.f;
	float env = 0.f;        // body amplitude envelope (for the ENV outputs)
	float declickL = 0.f, declickR = 0.f, lastL = 0.f, lastR = 0.f;
	float hitVel = 1.f;
	float roomSize = 1.f;   // latched at each hit

	// values worked out only when their control changes (saves pow/tan per sample)
	float cLength = -1.f, cTrsDecay = -1.f, cTrsTone = -1.f, cPitch = -99.f, cSr = 0.f;
	float L_ = 0.1f, tDec_ = 0.01f, fmDec_ = 0.02f, cut_ = 1000.f, f0_ = 40.f, bw_ = 0.002f;
	void prepare(const Controls& c) {
		bool srChanged = (cSr != sr);
		if (c.length != cLength) { cLength = c.length; L_ = lengthSec(c.length); }
		if (c.trsDecay != cTrsDecay) {
			cTrsDecay = c.trsDecay;
			tDec_ = expmap(c.trsDecay, 0.001f, 0.15f);
			fmDec_ = expmap(c.trsDecay, 0.010f, 0.100f);
		}
		if (c.trsTone != cTrsTone || srChanged) {
			cTrsTone = c.trsTone;
			cut_ = expmap(c.trsTone, 180.f, 16000.f);
			clickL.setCutoff(cut_, sr);
			clickR.g = clickL.g; clickR.a1 = clickL.a1;
			bw_ = expmap(1.f - c.trsTone, 0.0006f, 0.006f);
		}
		float pitch = c.tracking ? c.pitchOct : clampf(c.pitchOct, -1.f, 2.5f);
		if (pitch != cPitch) { cPitch = pitch; f0_ = clampf(C1_HZ * std::exp2(pitch), 15.f, 800.f); }
		cSr = sr;
	}

	// per-sample smoothed level controls (no zipper from the control-rate update)
	float sSus = 0.8f, sVel = 1.f, sFx = 0.f, sStereo = 0.3f;
	bool smoothInit = false;

	Noise nL{12345}, nR{987654};
	SVF clickL, clickR;
	OnePole dcL, dcR, xoL, xoR, sideHp, nbL, nbR;
	float crushHoldL = 0.f, crushHoldR = 0.f, crushCount = 0.f;
	// MEMB resonators (8 modes, rotation form) and beater pulse
	static const int MODES = 8;
	float mRe[MODES] = {}, mIm[MODES] = {};
	float beaterT = 1.f;
	Room roomL, roomR;
	GritTables grit;

	Voice() { setSampleRate(48000.f); }

	void setSampleRate(float s) {
		sr = s;
		st = 1.f / s;
		roomL.setSize(roomSize, 0, sr);
		roomR.setSize(roomSize, 23, sr);
	}

	void hit(float velocity, const Controls& c) {
		declickL = lastL;
		declickR = lastR;
		bool modelChanged = (c.model != model);
		if (!active || modelChanged) {
			roomL.clear();
			roomR.clear();
		}
		model = c.model;
		active = true;
		gate = true;
		held = false;
		t = 0.f;
		relT = 0.f;
		phase = 0.25f;  // start at the sine peak for a solid, consistent thump
		modPhase = 0.f;
		tickPhase = 0.f;
		beaterT = 0.f;
		hitVel = velocity;
		clickL.reset(); clickR.reset();
		for (int k = 0; k < MODES; k++) mRe[k] = mIm[k] = 0.f;
		roomSize = lerpf(0.35f, 1.0f, c.trsDecay);
		roomL.setSize(roomSize, 0, sr);
		roomR.setSize(roomSize, 23, sr);
	}
	void release() {
		if (gate) {
			gate = false;
			relT = 0.f;
		}
	}

	// body length in seconds, 40 ms .. 2.5 s
	static float lengthSec(float k) { return expmap(k, 0.04f, 2.5f); }

	// shape of the body decay, 1 at start, 0 at end (u = time / length)
	static float decayShape(float u) {
		if (u >= 1.f) return 0.f;
		float u4 = u * u; u4 *= u4;
		return std::exp(-3.f * u) * (1.f - u4);
	}

	// Pitch sweep: 1 at the hit, falling to 0. CURVE morphs from a fast,
	// snappy drop (808-style, n < 1) to a held-then-falling sweep (909-style,
	// n > 1: it stays high for a moment, then drops).
	static float sweep(float t, float curve) {
		float tau = lerpf(0.003f, 0.030f, curve);
		float n = lerpf(0.8f, 1.8f, curve);
		return std::exp(-std::pow(t / tau, n));
	}

	void smooth(const Controls& c) {
		if (!smoothInit) {
			sSus = c.sustain; sVel = c.velocity; sFx = c.fx; sStereo = c.stereo;
			smoothInit = true;
			return;
		}
		float a = 1.f - std::exp(-st / 0.002f);
		sSus += a * (c.sustain - sSus);
		sVel += a * (c.velocity - sVel);
		sFx += a * (c.fx - sFx);
		sStereo += a * (c.stereo - sStereo);
	}

	void process(const Controls& c, bool gateHigh, bool gateMode, float& outL, float& outR) {
		smooth(c);
		if (!gateHigh || !gateMode) release();
		if (!active) {
			env = 0.f;
			outL = outR = 0.f;
			fadeDeclick(outL, outR);
			outL = dcL.hp(outL, TWO_PI * 8.f * st);
			outR = dcR.hp(outR, TWO_PI * 8.f * st);
			lastL = outL; lastR = outR;
			return;
		}
		t += st;
		prepare(c);

		// ---- amplitude envelope ----
		float L = L_;
		float bodyEnv;
		float remaining;  // seconds until the voice ends (for the end fade)
		if (gate && t > 0.015f) held = true;
		if (gate && held) {
			// gate held: sustain (Bohm: a held HIT sustains the kick)
			bodyEnv = holdLevel = std::max(decayShape(0.015f / L), 0.7f);
			relT = 0.f;
			remaining = 1e9f;
		}
		else if (held) {
			relT += st;
			bodyEnv = holdLevel * decayShape(relT / L);
			remaining = L - relT;
		}
		else {
			bodyEnv = decayShape(t / L);
			remaining = L - t;
		}
		float atk = std::min(1.f, t / 0.0004f);  // tiny fade-in, no click from the body itself
		bodyEnv *= atk;
		env = bodyEnv;

		// punch: ATTACK also pushes the first ~20 ms up a little
		float punch = 1.f + c.attack * 0.8f * std::exp(-t / 0.018f);

		// ---- pitch ----
		float f0 = f0_;
		float sw = sweep(t, c.curve);
		float depthOct;
		switch (model) {
			case FM2: depthOct = 3.0f; break;
			case MEMB: depthOct = 0.f; break;  // membrane uses its own tension glide
			case GRIT: depthOct = 4.0f; break;
			default: depthOct = 3.4f; break;
		}
		depthOct *= lerpf(1.f, 0.6f, c.curve);
		float f = std::min(f0 * std::exp2(depthOct * sw), 0.45f * sr);

		float w = sStereo;
		float bodyL = 0.f, bodyR = 0.f, trsL = 0.f, trsR = 0.f;
		float sideA = TWO_PI * 180.f * st;  // side signal high-pass: keeps the sub mono

		// ---- transient (click) for the models that use one ----
		float tDec = tDec_;
		float noiseScale = std::sqrt(sr / 48000.f);
		float clickEnv = std::exp(-t / tDec);
		float nbA = 1.f - std::exp(-TWO_PI * 12000.f * st);  // noise band limit, same at every sample rate
		auto click = [&](Noise& n, SVF& s, OnePole& nb) {
			float x = nb.lp(n.white() * noiseScale * 1.6f, nbA) * clickEnv;
			// plus a short single-cycle tick at the start
			if (t < 0.0015f) x += std::sin(TWO_PI * t / 0.0015f);
			float lp, bp, hp;
			s.process(x, lp, bp, hp);
			// dark side leans low-pass, bright side leans band/high-pass
			return lerpf(lp, 0.45f * (bp + 0.5f * hp), c.trsTone);
		};

		float dph = f * st;
		if (model == ANLG || model == GRIT) {
			float off = w * (model == GRIT ? 0.024f : 0.044f);
			float M, S;
			if (model == ANLG) {
				M = anlgWave(phase, c.color);
				S = (off > 0.f) ? 0.5f * (anlgWave(phase + off, c.color) - anlgWave(phase - off, c.color)) : 0.f;
			}
			else {
				int la, lb; float mx;
				grit.choose(f, sr, la, lb, mx);
				M = grit.read(phase, c.color, la, lb, mx);
				S = (off > 0.f) ? 0.5f * (grit.read(phase + off, c.color, la, lb, mx) - grit.read(phase - off, c.color, la, lb, mx)) : 0.f;
			}
			S = sideHp.hp(S, sideA);
			bodyL = M + S;
			bodyR = M - S;
			// the click is over once its envelope has died away: skip the work
			bool clickOn = clickEnv > 1e-5f || t < 0.0015f;
			float tl = clickOn ? click(nL, clickL, nbL) : 0.f;
			float tr = clickOn ? click(nR, clickR, nbR) : 0.f;
			float mid = 0.5f * (tl + tr), side = 0.5f * (tl - tr);
			if (model == GRIT && clickOn) {
				// metallic FM tick layered on the noise click
				tickPhase += lerpf(900.f, 5200.f, c.trsTone) * st;
				float tick = std::sin(TWO_PI * tickPhase + 3.f * std::sin(TWO_PI * tickPhase * 1.414f)) * std::exp(-t / (tDec * 0.6f));
				mid = mid * 0.6f + tick * 0.5f;
			}
			float level = c.attack * (0.6f + 0.4f * c.color);
			// keep the click alone under about 5 V, however it's set
			if (clickOn) {
				trsL = std::tanh((mid + w * side) * level * 2.6f) * 0.85f;
				trsR = std::tanh((mid - w * side) * level * 2.6f) * 0.85f;
			}
			phase += dph;
		}
		else if (model == FM2) {
			// two-operator FM: ATTACK = depth, TRS DECAY = how fast it closes,
			// TRS TONE = modulator wave, COLOR = carrier shape
			float fmDec = fmDec_;
			float idx = c.attack * 6.f * std::exp(-t / fmDec);
			// keep the FM sidebands below Nyquist (Carson's rule)
			float idxMax = std::max(0.f, (0.45f * sr - f) / (f * c.fmRatio) - 1.f);
			idx = std::min(idx, idxMax);
			int wv = std::min(7, (int) (c.trsTone * 8.f));
			float mod = fmWave(wv, modPhase, dph * c.fmRatio);
			float pm = idx * mod * 0.16f;
			float off = w * 0.06f;
			float M = fmCarrier(phase + pm, c.color);
			float S = 0.5f * (fmCarrier(phase + off + pm, c.color) - fmCarrier(phase - off + pm, c.color));
			S = sideHp.hp(S, sideA);
			bodyL = M + S;
			bodyR = M - S;
			// a pinch of decorrelated air in stereo so width does something here too
			float air = std::exp(-t / 0.004f) * 0.08f * c.attack * noiseScale;
			trsL = nL.white() * air * w;
			trsR = nR.white() * air * w;
			phase += dph;
			modPhase += dph * c.fmRatio;
		}
		else {  // MEMB: struck membrane (modal) + beater + room mic
			static const float ratio[MODES] = {1.f, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f};
			static const float gain[MODES] = {1.f, 0.55f, 0.38f, 0.30f, 0.22f, 0.18f, 0.14f, 0.10f};
			// beater: a raised-cosine push; TRS TONE = beater hardness (shorter = harder)
			float bw = bw_;
			float exc = 0.f;
			if (beaterT < bw) {
				exc = (1.f - std::cos(TWO_PI * beaterT / bw)) / bw * st;  // area = 1
				beaterT += st;
			}
			// head tension glide: CURVE sets how much the head bends down after the hit
			float glide = 1.f + c.curve * 0.9f * std::exp(-t / 0.03f);
			// LENGTH sets head damping
			float tauFund = L * 0.25f;
			float shell = c.color;  // COLOR: how much of the shell overtones ring
			float y = 0.f;
			for (int k = 0; k < MODES; k++) {
				float fk = f0 * glide * ratio[k];
				if (fk > 0.45f * sr) continue;
				float tau = tauFund / (1.f + k * (2.2f - 1.8f * shell));
				float r = std::exp(-st / tau);
				float wk = TWO_PI * fk * st;
				float g = gain[k] * (k == 0 ? 1.f : (0.25f + 1.2f * shell));
				// rotation form: stays well-behaved while the frequency glides
				float cw = std::cos(wk) * r, sw2 = std::sin(wk) * r;
				float re = cw * mRe[k] - sw2 * mIm[k] + exc * g * 0.75f;
				float im = sw2 * mRe[k] + cw * mIm[k];
				mRe[k] = re;
				mIm[k] = im;
				y += im;
			}
			y = std::tanh(y * 0.9f) * 1.1f * 1.6f;
			// beater click itself (ATTACK)
			float bc = 0.f;
			if (t < 0.012f) bc = (nL.white() * 0.6f * noiseScale) * std::exp(-t / lerpf(0.0008f, 0.003f, 1.f - c.trsTone)) * c.attack * 1.4f;
			// room mic: SUSTAIN = room level, TRS DECAY = room size (latched per hit) and decay, STEREO = room width
			float fb = lerpf(0.55f, 0.86f, c.trsDecay);
			float rin = (y + bc) * 0.35f;
			float rl = roomL.process(rin, fb);
			float rr = roomR.process(rin, fb);
			float rmid = 0.5f * (rl + rr), rside = 0.5f * (rl - rr);
			float amb = sSus * 1.2f;
			bodyL = y + bc + (rmid + w * rside) * amb;
			bodyR = y + bc + (rmid - w * rside) * amb;
			// the membrane rings by itself; the room tail needs time to die away
			float tailEnd = L + lerpf(0.3f, 1.6f, c.trsDecay);
			remaining = tailEnd - t;
			bodyEnv = 1.f;
			env = decayShape(t / L);
		}
		phase -= std::floor(phase);
		modPhase -= std::floor(modPhase);
		tickPhase -= std::floor(tickPhase);

		// fade the very end out (5 ms; 30 ms for the MEMB room tail) instead of cutting it
		float fadeLen = (model == MEMB) ? 0.030f : 0.005f;
		float endFade = clampf(remaining / fadeLen, 0.f, 1.f);

		float sus = (model == MEMB) ? 1.f : sSus;
		float vel = sVel * hitVel;
		float sL = (bodyL * bodyEnv * sus * punch + trsL) * vel * endFade;
		float sR = (bodyR * bodyEnv * sus * punch + trsR) * vel * endFade;

		applyFx(c, sL, sR);

		fadeDeclick(sL, sR);
		sL = dcL.hp(sL, TWO_PI * 8.f * st);
		sR = dcR.hp(sR, TWO_PI * 8.f * st);
		outL = sL;
		outR = sR;
		lastL = outL;
		lastR = outR;
		if (remaining <= 0.f) {
			active = false;
			declickL = lastL;
			declickR = lastR;
		}
	}

	void fadeDeclick(float& l, float& r) {
		// on a retrigger, fade the old sound out over ~2 ms instead of cutting it
		float a = std::exp(-st / 0.002f);
		declickL *= a;
		declickR *= a;
		if (std::fabs(declickL) < 1e-9f) declickL = 0.f;  // no denormals
		if (std::fabs(declickR) < 1e-9f) declickR = 0.f;
		l += declickL;
		r += declickR;
	}

	float shapeOne(int type, float x, float d, OnePole& xo) {
		float g = 1.f + d * 7.f;
		switch (type) {
			default:
			case FX_TUBE: {  // warm, slightly asymmetric (biased tanh, stays monotonic)
				float b = 0.35f * d;
				float y = std::tanh(g * x + b) - std::tanh(b);
				return y / (1.f + 0.15f * d) / (1.f + 0.47f * d);
			}
			case FX_BASS: {  // distort the lows only, keep the click clean
				float lo = xo.lp(x, 2.f * PI * 160.f * st);
				float hi = x - lo;
				float y = std::tanh(lo * g * 1.4f) / std::max(1.f, 0.5f + 0.5f * std::sqrt(g)) + hi;
				return y / (1.f + 0.25f * d);
			}
			case FX_SOFT: {
				float v = x * g;
				return v / (1.f + std::fabs(v)) * (1.f + 0.6f * d) / (1.f + 0.75f * d);
			}
			case FX_HARD: {
				float v = x * g;
				return clampf(v, -1.f, 1.f) * 0.85f / (1.f + 0.56f * d);
			}
			case FX_FOLD: {
				float v = x * (1.f + d * 4.f);
				return std::sin(v * PI * 0.5f) * 0.9f / (1.f + 0.45f * d);
			}
			case FX_CRUSH: {
				return x;  // handled in applyFx (shares the sample-hold clock)
			}
		}
	}

	void applyFx(const Controls& c, float& l, float& r) {
		float d = clampf(sFx, 0.f, 1.f);
		if (d <= 0.0005f) return;
		float wet = std::min(1.f, d * 4.f);
		float yl, yr;
		if (c.fxType == FX_CRUSH) {
			float bits = lerpf(12.f, 2.5f, d);
			float q = std::exp2(bits - 1.f);
			float step = lerpf(1.f, 28.f, d * d) * (sr / 48000.f);
			crushCount += 1.f;
			if (crushCount >= step) {
				crushCount -= step;
				crushHoldL = std::round(l * q) / q;
				crushHoldR = std::round(r * q) / q;
			}
			yl = crushHoldL;
			yr = crushHoldR;
		}
		else {
			yl = shapeOne(c.fxType, l, d, xoL);
			yr = shapeOne(c.fxType, r, d, xoR);
		}
		l = lerpf(l, yl, wet);
		r = lerpf(r, yr, wet);
	}
};

}  // namespace boohm
