// Renders test kicks from the Boohm DSP to WAV files and prints level checks.
//   g++ -O2 -std=c++17 -Isrc test/render.cpp -o test/render && ./test/render out_dir
#include "../src/BoohmDSP.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace boohm;

static void writeWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r, int sr) {
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) return;
	int n = (int) l.size();
	int dataBytes = n * 2 * 2;
	auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
	auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); fwrite("WAVEfmt ", 1, 8, f);
	u32(16); u16(1); u16(2); u32(sr); u32(sr * 4); u16(4); u16(16);
	fwrite("data", 1, 4, f); u32(dataBytes);
	for (int i = 0; i < n; i++) {
		int16_t a = (int16_t) (clampf(l[i] / 10.f, -1.f, 1.f) * 32767);
		int16_t b = (int16_t) (clampf(r[i] / 10.f, -1.f, 1.f) * 32767);
		fwrite(&a, 2, 1, f); fwrite(&b, 2, 1, f);
	}
	fclose(f);
}

// renders `hits` kicks, one every `gap` seconds; gateLen = how long HIT stays high
static void render(const std::string& dir, const std::string& name, Controls c, int hits = 4, float gap = 0.5f, float gateLen = 0.005f) {
	const int sr = 48000;
	Voice v;
	v.setSampleRate(sr);
	int n = (int) (sr * gap * hits + sr * 1.5f);
	std::vector<float> L(n), R(n);
	float peak = 0.f, sumsq = 0.f, sumMono = 0.f;
	bool bad = false;
	for (int i = 0; i < n; i++) {
		float tt = (float) i / sr;
		int k = (int) (tt / gap);
		float local = tt - k * gap;
		bool gate = k < hits && local < gateLen;
		if (k < hits && i == (int) (k * gap * sr)) v.hit(1.f);
		float l, r;
		v.process(c, gate, true, l, r);
		l *= 5.f; r *= 5.f;
		if (!std::isfinite(l) || !std::isfinite(r)) bad = true;
		L[i] = l; R[i] = r;
		peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
		sumsq += l * l;
		sumMono += 0.25f * (l + r) * (l + r);
	}
	// rough pitch of the tail of the first kick by zero crossings (100..200 ms)
	int zc = 0;
	for (int i = (int) (0.10f * sr); i < (int) (0.20f * sr); i++)
		if ((L[i - 1] < 0) != (L[i] < 0)) zc++;
	printf("%-22s peak %5.2f V  rms %5.2f V  mono/stereo %4.2f  tail ~%5.1f Hz %s\n", name.c_str(), peak,
	       std::sqrt(sumsq / n), std::sqrt(sumMono / std::max(sumsq, 1e-9f)), zc / 0.1f / 2.f, bad ? "NaN!" : "");
	writeWav(dir + "/" + name + ".wav", L, R, sr);
}

int main(int argc, char** argv) {
	std::string dir = argc > 1 ? argv[1] : ".";
	const char* mn[] = {"anlg", "fm2", "memb", "grit"};
	const char* fn[] = {"tube", "bass", "soft", "hard", "fold", "crush"};
	for (int m = 0; m < NUM_MODELS; m++) {
		Controls c;
		c.model = m;
		render(dir, std::string(mn[m]) + "-default", c);
		Controls d = c; d.curve = 1.f; d.color = 0.7f; d.attack = 1.f; d.length = 0.8f;
		render(dir, std::string(mn[m]) + "-909-long", d);
		Controls e = c; e.curve = 0.f; e.length = 0.15f; e.trsTone = 0.9f; e.trsDecay = 0.1f;
		render(dir, std::string(mn[m]) + "-808-short", e);
		Controls g = c; g.color = 1.f; g.attack = 0.f; g.stereo = 1.f;
		render(dir, std::string(mn[m]) + "-color1-wide", g);
	}
	for (int f = 0; f < NUM_FX; f++) {
		Controls c; c.fxType = f; c.fx = 0.7f;
		render(dir, std::string("fx-") + fn[f], c);
	}
	Controls h; h.length = 0.4f;
	render(dir, "gate-held-300ms", h, 2, 1.0f, 0.3f);
	Controls p; p.pitchOct = 1.f;
	render(dir, "pitch-c2", p);
	return 0;
}
