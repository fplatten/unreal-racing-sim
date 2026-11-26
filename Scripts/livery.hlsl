// M_Livery's Custom node (pasted in by Scripts/setup_content.py): the Formula car's paint and the
// drivers' helmets, painted from the position on the car - no UVs, no textures.
//
// Inputs: P local position (cm; car frame: x forward, y across, z up, origin on the ground under the
// center of mass; swizzled below), N local normal, C1..C4 the scheme's colors, Pattern, Mode (0 body, 1 helmet,
// 2 the texture T, the Indy car's baked livery), Seed (picks the sponsors), Decals the
// lettering atlas.
// Returns the base color.
//
// Body patterns (1995-style schemes):
//   0 split    C1 nose and cockpit, C2 behind a diagonal (sidepods, engine cover), a C3 pinstripe
//   1 sweep    C1, a diagonal C2 band across the sidepods with C3 / C4 streaks ahead of it
//   2 arrow    C1 on top, C2 lower body, C3 / C2 chevrons on the nose pointing forward
//   3 blocks   C1, C2 sidepods and wings, C3 engine cover and nose tip
//   4 stripes  C1, C2 / C3 stripes nose to tail along the middle, C2 lower body
//   5 wave     C1 above a wave, C3 band, C2 below, a C4 slash on the sidepods
// Sponsor lettering (the Decals atlas, Scripts/make_decals.py) in C4, or black / white where C4
// would not show.
//
// Helmet patterns: 0 crown and band, 1 swept side stripes, 2 blades over the top, 3 swirl,
// 4 chevron, 5 halves.

#ifndef LIV_FNS
#define LIV_FNS
#define LIV_EDGE(v) saturate((v) / max(fwidth(v), 1e-4) + 0.5)
#define LIV_LUM(c) dot(c, float3(0.2126, 0.7152, 0.0722))
#define LIV_HASH(a) frac(sin(dot(a, float3(12.9898, 78.233, 37.719))) * 43758.5453)
#endif

// (the imported mesh's local frame: +Y forward, X across, Z up)
float3 p = P.yxz * 0.01;
float3 n = normalize(N.yxz);
float ay = abs(p.y);
float x = p.x, z = p.z;
int pat = (int)round(Pattern);

if (Mode > 1.5) return T;

if (Mode > 0.5)
{
	// ---------------------------------------------------------------- helmet
	float3 d = normalize(p - float3(0.34, 0.0, 0.76));
	float dy = abs(d.y);
	float3 h = C1;
	if (pat == 0)
	{
		h = lerp(h, C3, LIV_EDGE(0.08 - abs(d.z + 0.03)));
		h = lerp(h, C2, LIV_EDGE(0.02 - abs(d.z - 0.08)));
		h = lerp(h, C3, LIV_EDGE(d.z - 0.60));
		h = lerp(h, C2, LIV_EDGE(d.z - 0.70));
	}
	else if (pat == 1)
	{
		float s = d.z + 0.30 * d.x;
		h = lerp(h, C2, LIV_EDGE(0.08 - abs(s - 0.12)));
		h = lerp(h, C3, LIV_EDGE(0.06 - abs(s - 0.27)));
	}
	else if (pat == 2)
	{
		float blade = LIV_EDGE(0.08 - abs(dy - 0.24)) * LIV_EDGE(d.z + 0.05);
		h = lerp(h, C3, LIV_EDGE(0.11 - abs(dy - 0.24)) * LIV_EDGE(d.z + 0.08));
		h = lerp(h, C2, blade);
		h = lerp(h, C2, LIV_EDGE(-0.45 - d.z));
	}
	else if (pat == 3)
	{
		float a = atan2(d.y, d.x) / 6.2831853;
		float sw = frac(a * 3.0 + d.z * 1.2);
		h = lerp(h, C3, LIV_EDGE(0.20 - abs(sw - 0.5)));
		h = lerp(h, C2, LIV_EDGE(0.14 - abs(sw - 0.5)));
		h = lerp(h, C2, LIV_EDGE(d.z - 0.82));
	}
	else if (pat == 4)
	{
		float v = d.z - 0.55 * abs(d.x + 0.2) + 0.15;
		h = lerp(h, C3, LIV_EDGE(-v));
		h = lerp(h, C2, LIV_EDGE(0.11 - abs(v)));
		h = lerp(h, C2, LIV_EDGE(0.07 - dy) * LIV_EDGE(d.z - 0.2));
	}
	else
	{
		h = lerp(C2, C1, LIV_EDGE(d.z - 0.10));
		h = lerp(h, C3, LIV_EDGE(0.045 - abs(d.z - 0.10)));
		h = lerp(h, C3, LIV_EDGE(0.05 - dy) * LIV_EDGE(d.z - 0.10));
	}
	return h;
}

// -------------------------------------------------------------------- body
float sidepod = LIV_EDGE(ay - 0.31) * LIV_EDGE(0.80 - x) * LIV_EDGE(x + 1.06) * LIV_EDGE(0.56 - z);
float frontWing = LIV_EDGE(x - 1.90) * LIV_EDGE(0.335 - z) * LIV_EDGE(ay - 0.09);
float rearWing = LIV_EDGE(-1.45 - x) * LIV_EDGE(z - 0.80) * LIV_EDGE(0.482 - ay);
float endplate = LIV_EDGE(-1.40 - x) * LIV_EDGE(ay - 0.482);
float cover = LIV_EDGE(0.24 - ay) * LIV_EDGE(0.12 - x) * LIV_EDGE(x + 1.6) * LIV_EDGE(z - 0.50);
float3 col = C1;

if (pat == 0)
{
	// C1 nose and cockpit, C2 from a diagonal back (sidepods, engine cover, rear wing)
	float t = x + 1.6 * z - 1.55;
	col = lerp(C2, C1, LIV_EDGE(t));
	col = lerp(col, C3, LIV_EDGE(0.014 - abs(t)) * (1.0 - frontWing));
	col = lerp(col, C1, frontWing);
	col = lerp(col, C2, frontWing * LIV_EDGE(ay - 0.67));
	col = lerp(col, C2, rearWing);
}
else if (pat == 1)
{
	float u = x + 1.5 * z;
	col = lerp(col, C2, LIV_EDGE(0.30 - abs(u - 0.95)));
	col = lerp(col, C3, LIV_EDGE(0.025 - abs(u - 1.33)));
	col = lerp(col, C4, LIV_EDGE(0.025 - abs(u - 1.42)));
	col = lerp(col, C2, LIV_EDGE(0.025 - abs(u - 1.51)));
	col = lerp(col, C3, LIV_EDGE(x - 2.0) * (1.0 - frontWing));
	col = lerp(col, C2, cover * LIV_EDGE(z - 0.80));
	col = lerp(col, C2, frontWing);
}
else if (pat == 2)
{
	col = lerp(C2, C1, LIV_EDGE(z - 0.30 + 0.03 * x));
	float v = x - 1.3 * ay;
	col = lerp(col, C3, LIV_EDGE(0.09 - abs(v - 1.05)) * LIV_EDGE(z - 0.30));
	col = lerp(col, C2, LIV_EDGE(0.05 - abs(v - 1.30)) * LIV_EDGE(z - 0.30));
	col = lerp(col, C3, rearWing + endplate * LIV_EDGE(z - 0.70));
	col = lerp(col, C2, frontWing);
}
else if (pat == 3)
{
	col = lerp(col, C2, sidepod);
	col = lerp(col, C3, LIV_EDGE(0.015 - abs(z - 0.25)) * sidepod);
	col = lerp(col, C3, cover);
	col = lerp(col, C3, LIV_EDGE(x - 1.95) * (1.0 - frontWing));
	col = lerp(col, C2, frontWing + rearWing);
	col = lerp(col, C3, endplate * LIV_EDGE(z - 0.75));
}
else if (pat == 4)
{
	col = lerp(col, C2, LIV_EDGE(0.24 - z + 0.04 * x));
	col = lerp(col, C2, LIV_EDGE(0.045 - ay) * LIV_EDGE(z - 0.30));
	col = lerp(col, C3, LIV_EDGE(0.012 - abs(ay - 0.065)) * LIV_EDGE(z - 0.30));
	col = lerp(col, C2, rearWing);
	col = lerp(col, C3, endplate * LIV_EDGE(0.012 - abs(z - 0.62)));
}
else
{
	float w = 0.40 + 0.06 * sin(x * 3.2);
	col = lerp(C2, C1, LIV_EDGE(z - w));
	col = lerp(col, C3, LIV_EDGE(0.03 - abs(z - w + 0.035)));
	col = lerp(col, C4, sidepod * LIV_EDGE(0.06 - abs(x + 0.9 * z - 0.20)));
	col = lerp(col, C1, frontWing);
	col = lerp(col, C3, rearWing);
}

// ---------------------------------------------------------------- sponsor lettering
// Decals: the lettering atlas (Scripts/make_decals.py), 16 rows of 8:1 cells, the name centered in
// its row: rows 0-9 long names, 10-15 short. Each panel: (u0, u1) along it, (v0, v1) up it; the
// name is as tall as the panel and centered along it.
float textMask = 0.0;
{
	float side = LIV_EDGE(abs(n.y) - 0.55);
	float up = LIV_EDGE(n.z - 0.55);
	float4 boxes[6] = {
		float4(-0.50, 0.36, 0.22, 0.36),    // sidepod flanks
		float4(-1.92, -1.50, 0.60, 0.76),   // rear wing endplates
		float4(-0.95, -0.25, 0.565, 0.665), // engine cover flanks
		float4(-0.40, 0.40, -1.92, -1.80),  // rear wing top (across the car, v along x)
		float4(1.20, 1.95, -0.045, 0.045),  // nose top (along it, v across)
		float4(2.00, 2.42, 0.08, 0.19)};    // front wing endplates
	for (int b = 0; b < 6; b++)
	{
		float u, v, gate;
		if (b == 0) { u = x; v = z; gate = side * LIV_EDGE(ay - 0.55); }
		else if (b == 1) { u = x; v = z; gate = side * endplate; }
		else if (b == 2) { u = x; v = z; gate = side * cover * LIV_EDGE(ay - 0.10); }
		else if (b == 3) { u = -p.y; v = x; gate = up * rearWing * LIV_EDGE(z - 0.93); }
		else if (b == 4) { u = x; v = p.y; gate = up * LIV_EDGE(z - 0.40); }
		else { u = x; v = z; gate = side * frontWing * LIV_EDGE(ay - 0.68); }
		float4 bx = boxes[b];
		float inBox = LIV_EDGE(u - bx.x) * LIV_EDGE(bx.y - u) * LIV_EDGE(v - bx.z) * LIV_EDGE(bx.w - v);
		float hgt = bx.w - bx.z, uc = 0.5 * (bx.x + bx.y);
		// sides: mirrored by the way the surface faces, so the name reads from where it is seen
		float du = (b == 3 || b == 4) ? u - uc : (n.y > 0.0 ? uc - u : u - uc);
		bool shortBox = (bx.y - bx.x) / hgt < 4.0;
		float pick = LIV_HASH(float3(b, Seed, 1.7));
		float row = shortBox ? 10.0 + floor(pick * 5.999) : floor(pick * 9.999);
		float2 uv = float2(0.5 + du / (hgt * 8.0), (row + 1.0 - saturate((v - bx.z) / hgt)) / 16.0);
		float letters = Decals.Sample(DecalsSampler, uv).r;
		textMask = max(textMask, letters * inBox * gate);
	}
}
float3 ink = C4;
if (abs(LIV_LUM(ink) - LIV_LUM(col)) < 0.12) ink = LIV_LUM(col) > 0.25 ? float3(0.02, 0.02, 0.025) : float3(0.85, 0.85, 0.85);
col = lerp(col, ink, textMask);
return col;
