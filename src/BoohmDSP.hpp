// Boohm kick voice DSP. Plain C++ with no Rack dependency, so the same code can
// be rendered to WAV files by test/render.cpp for listening checks.
//
// The control set follows the Ohmforce Bohm kick module (HIT, VELOCITY, LENGTH,
// SUSTAIN, ATTACK, PITCH, CURVE, COLOR, TRS DECAY, TRS TONE, FX, STEREO); the
// sound engines and their maths are our own.
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

// Chamberlin state-variable filter, used for the click (transient) tone.
struct SVF {
	float lp = 0.f, bp = 0.f;
	void reset() { lp = bp = 0.f; }
	// returns {lp, bp, hp} through the out params
	void process(float x, float f, float q, float& olp, float& obp, float& ohp) {
		float hp = x - lp - q * bp;
		bp += f * hp;
		lp += f * bp;
		olp = lp; obp = bp; ohp = hp;
	}
};

// One-pole low-pass (used for crossover / DC blocking)
struct OnePole {
	float z = 0.f;
	float lp(float x, float a) { z += a * (x - z); return z; }
};

// The eight FM modulator shapes (in the spirit of the classic OPL chip waves).
inline float fmWave(int w, float ph) {
	float s = std::sin(TWO_PI * ph);
	switch (w) {
		default:
		case 0: return s;                                   // sine
		case 1: return s > 0.f ? s : 0.f;                   // half sine
		case 2: return std::fabs(s);                        // absolute sine
		case 3: { float p = ph - std::floor(ph); return (std::fmod(p, 0.5f) < 0.25f) ? std::fabs(s) : 0.f; } // quarter pulses
		case 4: { float p = ph - std::floor(ph); return p < 0.5f ? std::sin(2.f * TWO_PI * p) : 0.f; } // alternating sine
		case 5: { float p = ph - std::floor(ph); return p < 0.5f ? std::fabs(std::sin(2.f * TWO_PI * p)) : 0.f; } // camel
		case 6: return std::tanh(6.f * s);                  // square-ish
		case 7: { float p = ph - std::floor(ph); return 1.f - 2.f * p; } // saw
	}
}

// ANLG body: morph sine -> triangle -> warm saturated sine -> squarish
inline float anlgWave(float ph, float color) {
	float s = std::sin(TWO_PI * ph);
	float p = ph - std::floor(ph);
	float tri = (p < 0.25f) ? 4.f * p : (p < 0.75f ? 2.f - 4.f * p : 4.f * p - 4.f);
	float sat = std::tanh(2.6f * s) / std::tanh(2.6f);
	float sq = std::tanh(9.f * s);
	float c = clampf(color, 0.f, 1.f) * 3.f;
	if (c < 1.f) return lerpf(s, tri, c);
	if (c < 2.f) return lerpf(tri, sat, c - 1.f);
	return lerpf(sat, sq, c - 2.f) * lerpf(1.f, 0.85f, c - 2.f);
}

// FM-2 carrier: COLOR goes sine -> triangle -> square
inline float fmCarrier(float ph, float color) {
	float s = std::sin(TWO_PI * ph);
	float p = ph - std::floor(ph);
	float tri = (p < 0.25f) ? 4.f * p : (p < 0.75f ? 2.f - 4.f * p : 4.f * p - 4.f);
	float sq = std::tanh(7.f * s) * 0.85f;
	float c = clampf(color, 0.f, 1.f);
	if (c < 0.5f) return lerpf(s, tri, c * 2.f);
	return lerpf(tri, sq, (c - 0.5f) * 2.f);
}

// GRIT: a bank of eight digital single-cycle tables, scanned by COLOR.
struct GritTables {
	static const int N = 512;
	static const int T = 8;
	float t[T][N + 1];
	GritTables() {
		for (int i = 0; i <= N; i++) {
			float p = (float) i / N;
			float x = TWO_PI * p;
			float s = std::sin(x);
			t[0][i] = s;                                                    // sine
			t[1][i] = std::round(s * 4.f) / 4.f;                            // stepped (low-bit) sine
			float pd = p < 0.12f ? p / 0.12f * 0.5f : 0.5f + (p - 0.12f) / 0.88f * 0.5f;
			t[2][i] = std::sin(TWO_PI * pd);                                 // phase-distorted (bright, saw-like)
			t[3][i] = (s + std::sin(3 * x) / 3.f + std::sin(5 * x) / 5.f) * 0.9f; // odd stack
			t[4][i] = std::sin(3.f * s);                                     // folded sine
			t[5][i] = s * (0.6f + 0.4f * std::sin(7 * x));                   // formant ripple
			t[6][i] = std::tanh(4.f * (s + 0.5f * std::sin(2 * x))) * 0.8f;  // asymmetric pulse
			t[7][i] = std::sin(x + 2.2f * std::sin(5 * x)) * 0.8f;           // metallic FM table
		}
	}
	float read(float ph, float pos) const {
		float p = ph - std::floor(ph);
		float fi = p * N;
		int i = (int) fi;
		float fr = fi - i;
		float tp = clampf(pos, 0.f, 1.f) * (T - 1);
		int a = (int) tp;
		if (a >= T - 1) a = T - 2;
		float tf = tp - a;
		float va = t[a][i] + (t[a][i + 1] - t[a][i]) * fr;
		float vb = t[a + 1][i] + (t[a + 1][i + 1] - t[a + 1][i]) * fr;
		return lerpf(va, vb, tf);
	}
};

// A tiny room (comb + all-pass reverb) for the MEMB model's ambient mic.
struct Room {
	static const int MAXD = 4096;
	float comb[4][MAXD] = {};
	float ap[2][1024] = {};
	int ci[4] = {}, ai[2] = {};
	float damp[4] = {};
	int clen[4], alen[2];
	Room() { setBase(1.f, 0); }
	void clear() {
		std::memset(comb, 0, sizeof(comb));
		std::memset(ap, 0, sizeof(ap));
		for (float& d : damp) d = 0.f;
	}
	int base[4] = {1116, 1188, 1277, 1356};
	void setBase(float sizeScale, int spread) {
		for (int k = 0; k < 4; k++) clen[k] = std::min(MAXD - 1, std::max(64, (int) ((base[k] + spread) * sizeScale)));
		alen[0] = std::min(1023, 556 + spread);
		alen[1] = std::min(1023, 441 + spread);
	}
	float process(float x, float fb) {
		float y = 0.f;
		for (int k = 0; k < 4; k++) {
			float o = comb[k][ci[k]];
			damp[k] = o * 0.6f + damp[k] * 0.4f;
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
	float t = 0.f;          // seconds since the hit
	float relT = 0.f;       // seconds since release while gate was held
	float holdLevel = 1.f;  // body envelope level when the gate was released
	bool held = false;      // the gate was still high after the first few ms
	float phase = 0.f, modPhase = 0.f, tickPhase = 0.f;
	float env = 0.f;        // body amplitude envelope (for the ENV outputs)
	float declickL = 0.f, declickR = 0.f, lastL = 0.f, lastR = 0.f;
	float hitVel = 1.f;

	Noise nL{12345}, nR{987654};
	SVF clickL, clickR;
	OnePole dcL, dcR, xoL, xoR;
	float crushHoldL = 0.f, crushHoldR = 0.f, crushCount = 0.f;
	// MEMB resonators (8 modes) and beater pulse
	static const int MODES = 8;
	float m1[MODES] = {}, m2[MODES] = {};
	float beaterT = 1.f;
	Room roomL, roomR;
	GritTables grit;

	Voice() { roomR.setBase(1.f, 23); }

	void setSampleRate(float s) { sr = s; st = 1.f / s; }

	void hit(float velocity) {
		declickL = lastL;
		declickR = lastR;
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
		for (int k = 0; k < MODES; k++) m1[k] = m2[k] = 0.f;
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
	// snappy drop (808-style) to a slower, held-then-falling sweep (909-style).
	static float sweep(float t, float curve) {
		float tau = lerpf(0.0045f, 0.040f, curve);
		float p = std::exp(-t / tau);
		float k = lerpf(1.7f, 0.55f, curve);
		return std::pow(p, k);
	}

	void process(const Controls& c, bool gateHigh, bool gateMode, float& outL, float& outR) {
		if (!gateHigh || !gateMode) release();
		if (!active) {
			env = 0.f;
			fadeDeclick(outL, outR);
			lastL = outL; lastR = outR;
			return;
		}
		t += st;

		// ---- amplitude envelope ----
		float L = lengthSec(c.length);
		float bodyEnv;
		if (gate && t > 0.015f) held = true;
		if (gate && held) {
			// gate held: sustain where we are (Bohm: a held HIT sustains the kick)
			bodyEnv = holdLevel = std::max(decayShape(0.015f / L), 0.f);
			relT = 0.f;
		}
		else if (held) {
			relT += st;
			bodyEnv = holdLevel * decayShape(relT / L);
			if (relT >= L) active = false;
		}
		else {
			bodyEnv = decayShape(t / L);
			if (t >= L) active = false;
		}
		float atk = std::min(1.f, t / 0.0004f);  // tiny fade-in, no click from the body itself
		bodyEnv *= atk;
		env = bodyEnv;

		// punch: ATTACK also pushes the first ~20 ms up a little
		float punch = 1.f + c.attack * 0.8f * std::exp(-t / 0.018f);

		// ---- pitch ----
		float f0;
		if (c.tracking) f0 = C1_HZ * std::pow(2.f, c.pitchOct);
		else f0 = C1_HZ * std::pow(2.f, clampf(c.pitchOct, -1.f, 2.5f));
		f0 = clampf(f0, 15.f, 800.f);
		float sw = sweep(t, c.curve);
		float depthOct;
		switch (c.model) {
			case FM2: depthOct = 3.0f; break;
			case MEMB: depthOct = 0.f; break;  // membrane uses its own tension glide
			case GRIT: depthOct = 4.0f; break;
			default: depthOct = 3.4f; break;
		}
		float f = f0 * std::pow(2.f, depthOct * sw);
		f = std::min(f, 0.45f * sr);

		float w = c.stereo;
		float bodyL = 0.f, bodyR = 0.f, trsL = 0.f, trsR = 0.f;

		// ---- transient (click) for the models that use one ----
		float tDec = expmap(c.trsDecay, 0.001f, 0.15f);
		float cut = expmap(c.trsTone, 180.f, 14000.f);
		float fc = clampf(2.f * std::sin(PI * std::min(cut, sr * 0.2f) / sr), 0.f, 1.2f);
		auto click = [&](Noise& n, SVF& s) {
			float e = std::exp(-t / tDec);
			float x = n.white() * e;
			// plus a short single-cycle tick at the start
			if (t < 0.0015f) x += std::sin(TWO_PI * t / 0.0015f) * 1.5f;
			float lp, bp, hp;
			s.process(x, fc, 0.9f, lp, bp, hp);
			// dark side leans low-pass, bright side leans band/high-pass
			return lerpf(lp, bp + 0.5f * hp, c.trsTone) * e;
		};

		float dph = f * st;
		if (c.model == ANLG || c.model == GRIT) {
			float off = w * (c.model == GRIT ? 0.012f : 0.022f);
			float pL = phase - off, pR = phase + off;
			if (c.model == ANLG) {
				bodyL = anlgWave(pL, c.color);
				bodyR = anlgWave(pR, c.color);
			}
			else {
				bodyL = grit.read(pL, c.color);
				bodyR = grit.read(pR, c.color);
			}
			float tl = click(nL, clickL);
			float tr = click(nR, clickR);
			float mid = 0.5f * (tl + tr), side = 0.5f * (tl - tr);
			float level = c.attack * (0.6f + 0.6f * c.color) * 0.9f;
			if (c.model == GRIT) {
				// metallic FM tick layered on the noise click
				tickPhase += lerpf(900.f, 5200.f, c.trsTone) * st;
				float tick = std::sin(TWO_PI * tickPhase + 3.f * std::sin(TWO_PI * tickPhase * 1.414f)) * std::exp(-t / (tDec * 0.6f));
				mid = mid * 0.6f + tick * 0.7f;
			}
			trsL = (mid + w * side) * level;
			trsR = (mid - w * side) * level;
			phase += dph;
		}
		else if (c.model == FM2) {
			// two-operator FM: ATTACK = depth, TRS DECAY = how fast it closes,
			// TRS TONE = modulator wave, COLOR = carrier shape
			float fmDec = expmap(c.trsDecay, 0.010f, 0.100f);
			float idx = c.attack * 6.f * std::exp(-t / fmDec);
			int wv = std::min(7, (int) (c.trsTone * 8.f));
			float mod = fmWave(wv, modPhase);
			float off = w * 0.03f;
			bodyL = fmCarrier(phase - off + idx * mod * 0.16f, c.color);
			bodyR = fmCarrier(phase + off + idx * mod * 0.16f, c.color);
			// a pinch of decorrelated air in stereo so width does something here too
			float air = std::exp(-t / 0.004f) * 0.08f * c.attack;
			trsL = nL.white() * air * w;
			trsR = nR.white() * air * w;
			phase += dph;
			modPhase += dph * c.fmRatio;
		}
		else {  // MEMB: struck membrane (modal) + beater + room mic
			static const float ratio[MODES] = {1.f, 1.594f, 2.136f, 2.296f, 2.653f, 2.918f, 3.156f, 3.501f};
			static const float gain[MODES] = {1.f, 0.55f, 0.38f, 0.30f, 0.22f, 0.18f, 0.14f, 0.10f};
			// beater: a raised-cosine push; TRS TONE = beater hardness (shorter = harder)
			float bw = expmap(1.f - c.trsTone, 0.0006f, 0.006f);
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
				float yn = 2.f * r * std::cos(wk) * m1[k] - r * r * m2[k] + std::sin(wk) * exc * g * 0.75f;
				m2[k] = m1[k];
				m1[k] = yn;
				y += yn;
			}
			y = std::tanh(y * 0.9f) * 1.1f;
			// beater click itself (ATTACK)
			float bc = 0.f;
			if (t < 0.012f) bc = (nL.white() * 0.6f) * std::exp(-t / lerpf(0.0008f, 0.003f, 1.f - c.trsTone)) * c.attack * 1.4f;
			// room mic: SUSTAIN = room level, TRS DECAY = room size and decay, STEREO = room width
			float size = lerpf(0.35f, 1.0f, c.trsDecay);
			roomL.setBase(size, 0);
			roomR.setBase(size, 23);
			float fb = lerpf(0.55f, 0.86f, c.trsDecay);
			float rin = (y + bc) * 0.35f;
			float rl = roomL.process(rin, fb);
			float rr = roomR.process(rin, fb);
			float rmid = 0.5f * (rl + rr), rside = 0.5f * (rl - rr);
			float amb = c.sustain * 1.2f;
			bodyL = y + bc + (rmid + w * rside) * amb;
			bodyR = y + bc + (rmid - w * rside) * amb;
			// the membrane rings by itself; the room tail needs time to die away
			active = true;
			float tailEnd = L + lerpf(0.3f, 1.6f, c.trsDecay);
			if (t > tailEnd) active = false;
			bodyEnv = 1.f;
			env = decayShape(t / L);
		}
		phase -= std::floor(phase);
		modPhase -= std::floor(modPhase);
		tickPhase -= std::floor(tickPhase);

		float sus = (c.model == MEMB) ? 1.f : c.sustain;
		float vel = c.velocity * hitVel;
		float sL = (bodyL * bodyEnv * sus * punch + trsL) * vel;
		float sR = (bodyR * bodyEnv * sus * punch + trsR) * vel;

		applyFx(c, sL, sR);

		fadeDeclick(sL, sR);
		outL = sL;
		outR = sR;
		lastL = outL;
		lastR = outR;
	}

	void fadeDeclick(float& l, float& r) {
		// on a retrigger, fade the old sound out over ~2 ms instead of cutting it
		float a = std::exp(-st / 0.002f);
		declickL *= a;
		declickR *= a;
		l += declickL;
		r += declickR;
	}

	float shapeOne(int type, float x, float d, OnePole& xo, float& hold) {
		float g = 1.f + d * 7.f;
		switch (type) {
			default:
			case FX_TUBE: {  // warm, slightly asymmetric
				float v = g * x;
				float y = std::tanh(v + 0.18f * d * v * v) - std::tanh(0.18f * d * 0.f);
				return y / (1.f + 0.15f * d);
			}
			case FX_BASS: {  // distort the lows only, keep the click clean
				float lo = xo.lp(x, 2.f * PI * 160.f * st);
				float hi = x - lo;
				return std::tanh(lo * g * 1.4f) / std::max(1.f, 0.5f + 0.5f * std::sqrt(g)) + hi;
			}
			case FX_SOFT: {
				float v = x * g;
				return v / (1.f + std::fabs(v)) * (1.f + 0.6f * d);
			}
			case FX_HARD: {
				float v = x * g;
				return clampf(v, -1.f, 1.f) * 0.85f;
			}
			case FX_FOLD: {
				float v = x * (1.f + d * 4.f);
				return std::sin(v * PI * 0.5f) * 0.9f;
			}
			case FX_CRUSH: {
				return x;  // handled in applyFx (shares the sample-hold clock)
			}
		}
	}

	void applyFx(const Controls& c, float& l, float& r) {
		float d = clampf(c.fx, 0.f, 1.f);
		if (d <= 0.0005f) return;
		float wet = std::min(1.f, d * 4.f);
		float yl, yr;
		if (c.fxType == FX_CRUSH) {
			float bits = lerpf(12.f, 2.5f, d);
			float q = std::pow(2.f, bits - 1.f);
			float step = lerpf(1.f, 28.f, d * d);
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
			yl = shapeOne(c.fxType, l, d, xoL, crushHoldL);
			yr = shapeOne(c.fxType, r, d, xoR, crushHoldR);
		}
		l = lerpf(l, yl, wet);
		r = lerpf(r, yr, wet);
	}
};

}  // namespace boohm
