# epaper-image design

This document explains how epaper-image turns an image into rows of ink codes
for an e-paper panel: the colour model it works in, the algorithm of each
stage, the fixed-point arithmetic that makes the output identical on every
target, and how the per-panel tables are produced. It is written for someone
who wants to understand, review or modify the library. The API reference is
the Doxygen in `include/epaper_image.h`; this document covers what the code
behind it computes and why.

## Overview

The library is three streaming objects, all living entirely in caller-provided
memory:

- the **resampler** (`src/epi_resampler.c`) takes source rows of 8-bit sRGB
  at any size and produces panel-sized rows of 8-bit sRGB, cropped, scaled
  and placed on a background;
- the **converter** (`src/epi_converter.c`) takes panel-sized rows of 8-bit
  sRGB and produces rows of ink codes, one per byte;
- the **preview reducer** (`src/epi_preview.c`) takes rows of ink codes and
  produces an RGB preview of what the panel will show, at the same or a smaller
  size.

A caller pushes each source row into the resampler and pushes every row the
resampler pops into the converter. Every row the converter pops goes to the
reducer, to `epi_pack_row` to make the bytes the panel is sent, or to both.

All three objects never allocate, validate their configuration in init, and run
integer-only per-pixel loops; the only floating point after init is the
resampler's vertical weights, computed in deterministic double once per output
row.

Inside the converter the work is split into stages that the code's comments
label S1 to S7:

| stage | what it does | consumes | produces |
|---|---|---|---|
| S1 source | sRGB8 to linear light, CIE Y and L\*src; adaptation degree; panel XYZ and ink-relative CIELAB | one pixel | L\*src, ink-relative CIELAB |
| S2 base | separable Gaussian blur of L\*src | L\*src in a (2r+1)×(2r+1) window | base L\* |
| S3 tone and detail | tone curve on the base, local detail added back | L\*src, base | new L\* |
| S4 colour | vibrance on a\*b\*, veil on L\* | CIELAB | CIELAB |
| S5 gamut | in-gamut test in XYZ; out-of-hull pixels moved onto the hull | CIELAB | XYZ inside the ink hull |
| S6 target | XYZ to linear sRGB | XYZ | target t |
| S7 error diffusion | nearest ink, damped serpentine Floyd–Steinberg | t and the diffused error | ink index |

S1 to S6 are the **rendering intent**: a per-pixel mapping (apart from the
blur in S2) from source colour to a target colour that the panel can
reproduce on average. S7 is the **ditherer**: it chooses one ink per pixel so
that areas average to the targets. A final step maps ink indices to ink codes.

S2 needs r rows below the current one, where r is the blur radius, so output
lags input by r rows (18 on the EL133UF1); see
[Streaming and output](#streaming-and-output).

A panel is either a **colour panel** (for example the EL133UF1, a six-ink
Spectra 6 glass) or a **grey panel** (2 to 16 grey levels, for example the
GDEY075T7 in its mono and 4-grey modes). Grey panels run the same pipeline
with every chromatic stage reduced to lightness: S1 computes only L\*src,
there is no adaptation, vibrance or hull, the target is a single luminance,
and S7 works in one channel with a coverage toe ahead of it. The difference
is carried by the panel's tables (`kind` in `struct epi_panel_tables`); the
converter branches on it to call the colour or grey row functions at each
push and pop.

## Colour model

### Measured inks

A panel is described by its inks (`epi_ink_t`): the `code` sent to the panel,
a `nominal` sRGB8 colour (for nominal previews, and for a colour panel's init to
check which ink is white), and a `measured`
sRGB8 colour, which is what the panel actually shows. Everything the library
computes is driven by the measured colours: the gamut, the matching, the
diffused error and the measured preview. The EL133UF1's inks, in matching
order, are:

| ink | code | measured sRGB8 |
|---|---|---|
| black | 0 | (59, 62, 62) |
| white | 1 | (190, 200, 200) |
| yellow | 2 | (203, 189, 66) |
| red | 3 | (137, 50, 41) |
| blue | 5 | (60, 108, 177) |
| green | 6 | (87, 125, 107) |

Code 4 is unused on that glass. The order is significant: it is the index
space the ditherer works in, and matching ties go to the lowest index.

This model of the panel has known limits: on the EL133UF1 an isolated dot gives
less light than its share of the area predicts, and the colour inks' chroma is
likely under-measured. Preview colours are therefore estimates.

Measured colours become linear light through the sRGB EOTF and XYZ through
the 4-decimal IEC 61966-2-1 matrix:

```
c   = u / 255
lin = c / 12.92                     for c ≤ 0.04045
      ((c + 0.055) / 1.055)^2.4     otherwise

    [X]   [0.4124 0.3576 0.1805] [R]
    [Y] = [0.2126 0.7152 0.0722] [G]
    [Z]   [0.0193 0.1192 0.9505] [B]
```

The D65 white is taken as the matrix's row sums, (0.9505, 1, 1.0890), so
sRGB white maps exactly onto it, with Y = 1. All XYZ in
the library is relative to source white Y = 1. In the hot path both steps
are constant tables (`src/epi_srgb.c`): a 256-entry Q16 EOTF table and the
matrix in Q16.

### Paper white and the ink hull

The **white ink** W (the ink with the largest measured Y) is paper white, and
the **black ink** B (the smallest Y) is the panel's black. A dithered area
seen from a normal viewing distance is an area-weighted mix of the inks'
light, so the colours the panel can reproduce on average are exactly the
convex combinations of the inks' XYZ: their convex hull. For the EL133UF1
that hull has 8 faces. The hull is the gamut that S5 maps into.

### Ink-relative CIELAB and OKLab

Both perceptual spaces are taken relative to the white ink rather than to
D65, so the paper is the reference white of the image.

**CIELAB** uses W as the reference white (Xn, Yn, Zn) = W, so the white ink
is (100, 0, 0):

```
f(t)  = cbrt(t)                     for t > (6/29)³
        t / (3·(6/29)²) + 4/29      otherwise
L*    = 116·f(Y/Yn) − 16
a*    = 500·(f(X/Xn) − f(Y/Yn))
b*    = 200·(f(Y/Yn) − f(Z/Zn))
```

L\*src, the source lightness used for the blur and the tone curve, is the
exception: it is L\* of the source Y with white Y = 1, before any mapping to
the panel.

**OKLab** (Ottosson) is used by the ditherer. It is made ink-relative by
dividing linear sRGB by the white ink's linear sRGB w before the first
matrix, so the white ink maps to (1, 0, 0):

```
OK(x) = M2 · cbrt(M1 · (x ⊘ w))
```

with Ottosson's M1 (linear sRGB to LMS) and M2 (LMS′ to OKLab), and a signed
cube root so working values outside [0, 1] are defined. The tables fold the
division into the first matrix: `lin_to_lms` = M1·diag(1/w),
`lms_to_oklab` = M2.

### Bradford adaptation and black-point compensation

S1 maps source XYZ to panel XYZ by two candidate transforms, blended per
pixel.

The **adapted** transform is media-relative colorimetry, as in ICC: a
Bradford chromatic adaptation A from D65 to the white ink's chromaticity
(destination white W/W_y, so Y is unchanged), followed by per-component
black-point compensation that sends source black to B and adapted source
white to W:

```
A          = Bradford(D65 → W / W_y)
k          = (W − B) ⊘ (A · white_D65)          per component
to_panel(s) = diag(k) · A · s + B
```

The **unadapted** transform applies no chromatic adaptation: it scales
source XYZ uniformly to the inks' luminance range and adds the black ink:

```
bpc(s) = (W_y − B_y) · s + B
```

The blend by an adaptation degree D in [0, 1] is linear in XYZ:

```
panel = D · to_panel(s) + (1 − D) · bpc(s)
      = (D · M_adapt + (1 − D) · M_bpc) · lin + B
M_adapt = diag(k) · A · M_srgb
M_bpc   = (W_y − B_y) · M_srgb
```

Both matrices are folded with the sRGB matrix so they act on linear sRGB
directly (`lin_to_panel_adapt`, `lin_to_panel_bpc`). At D = 1 source white
lands exactly on the white ink; each quantised row of
`lin_to_panel_adapt` is adjusted so that its sum is exactly W − B, which keeps this true
in Q16 (otherwise rounding could put source white one LSB outside the
hull's white vertex). At D = 0 source white has the white ink's Y and D65's
chromaticity apart from the black ink's offset. How D is chosen per pixel is
described under S1.

### What a panel holds

`epi_panel_t` is const data: a name, the native size, the inks, the detail
blur σ (`detail_sigma`, informational: the kernel in the tables is what
runs), a list of named presets with the default first, and a pointer to the
derived tables (`src/epi_panel_tables.h`). It points at an
`epi_panel_tables_t`, which holds what both kinds read: the kind, the ink count
and the detail kernel (Q15 taps −r..r). That is the first member of the kind's
own struct, `epi_colour_tables_t` or `epi_grey_tables_t`, and the code casts to
the right one once at the top of a function, so a pixel loop pays nothing for
the split. A colour panel's struct also holds:

- W and B in XYZ (Q16);
- `lin_to_panel_adapt` and `lin_to_panel_bpc` (Q16);
- `xyz_to_lin`, the inverse of the 4-decimal sRGB matrix (Q16);
- `lin_to_lms` and `lms_to_oklab` (Q16);
- up to 32 hull faces, each a unit outward normal (Q16) and offset (Q32);
- L\*min and L\*max, the neutral interval of the hull (Q8);
- the cusp L\* at each integer hue degree, 360 entries (Q8);
- each ink's linear sRGB and ink-relative OKLab (Q16);
- n̂ = w/|w|, the white ink's direction in linear sRGB (Q16).

A grey panel's struct holds instead each level's Y relative to the lightest
(`level_y`, Q16) and the decision thresholds between levels (`level_thr`,
Q16). On a 64-bit host the colour tables are 2,112 B and the grey tables 260 B.
A panel has at most 16 inks and a kernel radius of at most 32.

Both kinds of table can be generated offline by `tools/panelgen` and committed
as C source (`src/panels/<panel>.c`), see [Panel tables](#panel-tables), or built
at runtime: colour by `epi_colour_panel_init`, see
[Colour panels from inks](#colour-panels-from-inks), and grey by
`epi_grey_panel_init`, see [Grey panels](#grey-panels).

## Rendering intent

The rendering intent (S1–S6, `src/epi_intent.c`) maps each source pixel to a
target in linear sRGB that lies inside the ink hull. It is controlled by an
`epi_intent_t` of five knobs, validated in init:

| knob | range | effect |
|---|---|---|
| `tone` | 0..1 | blend between black-point-compensated lightness (0) and a midtone curve (1) |
| `detail` | 0..1 | share of local lightness detail kept around the tone-mapped base |
| `adapt` | 0..1 | adaptation to paper white below the highlights |
| `vibrance` | 0..3 | chroma gain sparing near-neutrals and skin; 1 is off |
| `veil` | 0..3 | how far below paper white source white is placed, in L\* units; 0 is off |

Init turns the intent into fixed-point data in the converter's context: the
tone and gain tables (S3), and adapt, vibrance and veil in Q16, Q16 and Q8.

Below, ss is smoothstep:

```
ss(x, lo, hi) = 3u² − 2u³,   u = clip((x − lo) / (hi − lo), 0, 1)
```

### S1: source colour

Each pixel's sRGB8 goes through the EOTF table to linear light (Q16), and
the Y row of the sRGB matrix gives Y. L\*src is L\* of that Y with white
Y = 1, in Q8.

The adaptation degree is then

```
D = adapt + (1 − adapt) · ss(L*src, 75, 95)
```

so D equals `adapt` in shadows and midtones and rises to 1 over L\*src 75 to
95. Highlights are always fully adapted: source white lands exactly on
paper white and is reproduced by the white ink alone. Below the highlights,
`adapt` = 1 renders neutrals in the paper's own tint (media-relative), and
`adapt` = 0 keeps their D65 chromaticity, compressing only luminance.

The panel XYZ is computed as

```
panel = M_bpc·lin + D·(M_adapt·lin − M_bpc·lin) + B
```

in Q32, and converted to ink-relative CIELAB (Q8). The ratios X/Xn, Y/Yn,
Z/Zn use init-time reciprocals round(2^40/W) instead of a per-pixel division.

The colour converter stores the source row as sRGB8 and repeats this stage
when the row's output is computed (see [Streaming and output](#streaming-and-output));
both evaluations compute L\*src with the same code, so they agree exactly.

### S2: base

The base is a Gaussian blur of L\*src, separable, with σ = `detail_sigma` in
panel pixels and radius r = ⌈3σ⌉. The kernel is sampled, normalised and
rounded to Q15, with the centre tap absorbing the residual so the taps sum
to exactly 2^15:

```
w_i  = exp(−i² / (2σ²)),  i = −r..r
tap_i = round(w_i / Σw · 2^15),  tap_0 += 2^15 − Σtap
```

The horizontal pass runs on each row as it is pushed, the vertical pass when
the row's output is computed. Each pass accumulates in int32 and rounds once
(shift by 15) back to an int16 Q8 L\*. Edges are replicated: horizontally by
clamping the column index, vertically by repeating the edge row's pointer.

σ is a viewing-distance quantity: the base separates detail finer than about
σ from the broad tonal layout of the image, so it is set per panel in panel
pixels and scales with pixel pitch. The EL133UF1 uses σ = 6 (about 150 dpi)
and the GDEY075T7 σ = 5 (about 124 dpi), both close to 1 mm on the glass.
Truncating at 3σ drops 0.27% of the Gaussian's weight; it keeps r, and with
it the row lag and the ring buffers, a quarter smaller than a 4σ cut.

### S3: tone and detail

The panel's lightness range is [L\*min, L\*max] ([35.14, 100] on the
EL133UF1), so source lightness has to be compressed. The tone curve T maps
source L\* to panel L\*, and is a blend of two curves.

**T0, black-point-compensated lightness**, compresses luminance linearly
between the inks' Y:

```
Y(L)  = f⁻¹((L + 16) / 116)
T0(L) = 116 · f((Y(L) · (W_y − B_y) + B_y) / W_y) − 16
```

**M, the midtone curve**, is the identity above a knee K and a cubic Hermite
below it, from (0, L\*min) with slope 0.15 to (K, K) with slope 1:

```
M(L) = L                                                  for L ≥ K
M(L) = h00·L*min + h10·0.15·K + h01·K + h11·K             for L < K
       τ = clip(L/K, 0, 1)
       h00 = 2τ³ − 3τ² + 1    h10 = τ³ − 2τ² + τ
       h01 = −2τ³ + 3τ²       h11 = τ³ − τ²
K    = max(60, L*min / (1 − 0.3371))
```

M keeps midtones and highlights at their source lightness and spends the
compression in the shadows. The Hermite is monotone when the
Fritsch–Carlson condition (0.15/δ)² + (1/δ)² ≤ 9 holds, with
δ = (K − L\*min)/K the secant slope; that needs δ ≥ sqrt(1.0225/9) ≈ 0.33706,
and 0.3371 keeps a margin. For any black up to L\*min ≈ 39.77 the knee is
60. A lighter black (low-contrast grey glass) moves the knee up just enough
to stay monotone, and leaves the curve unchanged wherever K = 60. There is
no valid knee once K reaches 100, at L\*min ≥ 66.29 (a contrast below about
2.8:1); init rejects such a panel.

The tone curve is

```
T(L) = (1 − tone) · T0(L) + tone · M(L)
```

**Detail.** Applying T pixel by pixel would scale local contrast by T′ along
with the global range. Instead T is applied to the base, and the pixel's
deviation from the base is added back with a gain between T′ and 1:

```
T′(b) = (T(b + 0.001) − T(b − 0.001)) / 0.002
g(b)  = T′(b) + detail · (1 − T′(b))
L*    = T(base) + g(base) · (L*src − base)
```

At `detail` = 0 this is, to first order, the tone curve applied pointwise;
at `detail` = 1 local detail keeps its source amplitude around the
tone-mapped base. a\* and b\* are those of the S1 panel colour, unchanged.

Init evaluates T and g in double at 401 knots, base L\* = 0, 0.25, …, 100,
and stores them as `tone_t_q8` (Q8) and `tone_g_q16` (Q16). Per pixel the
base is clamped to [0, 100] for the lookup, both tables are interpolated
linearly between knots, and the product g·(L\*src − base) uses the
unclamped base.

T0(0) is L\* of B_y relative to W_y, 34.74 on the EL133UF1, slightly below
L\*min; the deepest shadows reach the neutral interval through the L\* clamp
in S5.

### S4: vibrance and veil

**Vibrance** is a chroma gain that spares near-neutrals and skin. In
ink-relative CIELAB, with chroma C and hue h:

```
C    = hypot(a*, b*)
h    = atan2(b*, a*)  in degrees, [0, 360)
skin = ss(h, 10, 25) · (1 − ss(h, 60, 75)) · (1 − ss(C, 30, 45))
w    = ss(C, 4, 14) · (1 − skin)
(a*, b*) ← (a*, b*) · (1 + (vibrance − 1) · w)
```

The gain fades in between C 4 and 14, so neutrals are left neutral; the
skin weight covers hues 10° to 75° (fully between 25° and 60°) at chroma
up to 30, fading out by 45, so skin tones keep their chroma while
saturated oranges are still boosted. The stage is skipped when `vibrance`
is 1 (in Q16).

A uniform saturation gain amplifies the slight warm cast of a near-white sky
into yellow dots and reddens skin, so the gain fades near neutrals and spares
skin hues. Skin tones fall at ink-relative CIELAB hues of about 22° to 56°.

**Veil** lowers the highlights:

```
L* ← L* − veil · ss(L*, 90, 100)
```

so source white lands `veil` L\* below paper white and is rendered as a
light dither rather than bare paper, while L\* below 90 is untouched. It is
skipped when `veil` is 0.

A clipped sky renders as bare paper beside sparse dots in the colour just below
the clip, so the clip's contour shows. A `veil` of about 1 hides the contour
under a light dot field, but the same dots read as spots on intentionally white
areas such as a page background. Every preset therefore leaves `veil` at 0, and
it is an option for images with clipped highlights.

### S5: gamut mapping

The CIELAB colour is converted back to XYZ with W as reference white, and
tested against every hull face:

```
inside  ⇔  n_f · xyz + d_f ≤ 0  for every face f
```

in exact integer arithmetic (Q16 normal × Q16 XYZ + Q32 offset). Pixels
inside the hull are left unchanged. A pixel outside is moved along a straight
line in (L\*, C\*) at constant hue towards an anchor on the neutral axis, by
Ottosson's adaptive-L0 rule, which picks the anchor from the pixel's position
relative to the hue's cusp (the point of maximum in-hull chroma):

```
L    = clip(L*, L*min, L*max)                  (a*, b* unchanged)
       if L ≠ L* and (L, a*, b*) is inside: done
C    = hypot(a*, b*)
h    = atan2(b*, a*)
Lc   = cusp(h)            linear between integer degrees, 359° wrapping to 0°
span = L*max − L*min
Ld   = L − Lc
k    = 2·(L*max − Lc)   if Ld > 0
       2·(Lc − L*min)   otherwise
e1   = k/2 + |Ld| + 0.05 · C · span / k
L0   = Lc + sgn(Ld) · (e1 − sqrt(e1² − 2·k·|Ld|)) / 2,   clipped to [L*min, L*max]
P(s) = (L0 + s·(L − L0),  s·a*,  s·b*),   s in [0, 1]
```

This is the normalised rule (L′ = (L − L\*min)/span, C′ = C/span, α = 0.05)
rewritten in L\* units, where only the α·C′/k′ term picks up a factor of
span. When the pixel's chroma is small compared with its lightness distance
from the cusp, the anchor stays at the pixel's own (clamped) lightness and
the move reduces chroma at constant L\*; as chroma grows the anchor moves
towards the cusp's lightness, so far out-of-gamut, saturated colours give up
lightness to keep chroma. α sets how quickly that happens. Scaling a\* and
b\* by s moves along the line at constant hue without any trigonometry.

α is 0.05, close to lightness-preserving, because higher values grey or darken
pale out-of-hull colours, such as near-white backgrounds and skies, for a small
gain in yellow chroma.

The move bisects s in Q16 for 16 steps, keeping the inside end: P(1) is
outside, P(0) = (L0, 0, 0) is a point of the hull's neutral interval, and the
result is the last inside point found (P(0) if none). The hull is convex in
XYZ but not in CIELAB, so the bisection finds a boundary crossing of the line
rather than a guaranteed outermost one; the kept point is always inside.
Each probe is a
CIELAB-to-XYZ conversion and an in-gamut test against the hull. Two shortcuts
leave the result
unchanged: a probe whose Q8 CIELAB value equals that of a bracket end reuses
that end's verdict, and the face that rejected the previous probe is tested
first.

The cusp table holds, for each integer hue, the L\* of the maximum in-hull
chroma (see [Colour panels from inks](#colour-panels-from-inks)).
`lab_to_xyz` clamps L\* to ±256 and a\*, b\* to ±512 only so the products
fit int32; no meaningful colour reaches those bounds. A colour panel has each cusp
at least 0.5 L\* inside the neutral interval, which keeps k above zero and the
anchor's arithmetic in int64 for any a\* and b\* the pipeline can produce, so the
move has no guard for a table that breaks this.

### S6: target

The in-hull XYZ is converted to linear sRGB with the inverse of the sRGB
matrix (`xyz_to_lin`), in Q16, without clamping. This target t is in the
same space as the inks' linear sRGB, so the ditherer can mix inks to it.

### Presets

A preset is a named `epi_intent_t`; `presets[0]` is the default, used when
the converter is given no intent. The EL133UF1 offers:

| preset | tone | detail | adapt | vibrance | veil |
|---|---|---|---|---|---|
| Default | 0.5 | 0.75 | 0.5 | 1 | 0 |
| Soft | 0 | 0.75 | 0.5 | 1 | 0 |
| Deep | 1 | 0.75 | 0.5 | 1 | 0 |
| Vivid | 0.5 | 0.75 | 0.5 | 1.3 | 0 |

Soft keeps the image's tonal proportions (pure black-point compensation),
which lifts shadows; Deep keeps midtones at their source lightness and
compresses the shadows; Default is halfway. Grey panels offer Default, Soft
and Deep with the same values (adapt and vibrance have no effect on grey).

Tone has the largest effect of any knob. Tone 1 loses detail in dark images, and
`detail` cannot recover it, because the blur restores texture finer than σ, not
the separation of large dark areas; so Default sits at tone 0.5. Detail masks
grain, but detail 1 over-processes skin at tone 1, so the presets use 0.75. Tone
is not derived from the image because a streaming converter has no whole-image
pass.

## Error diffusion

S7 (`src/epi_dither.c`) turns each row of targets into ink indices by error
diffusion: each pixel takes the ink nearest to its target plus the error
diffused onto it, and the remaining error is spread onto unprocessed
neighbours. Matching is done in ink-relative OKLab, where distance follows
perceived difference; the error is kept in linear sRGB, where the eye
averages the inks' light.

On Spectra 6 inks, linear-RGB matching builds a mid grey from white and the
colour inks with no black, CIELAB matching is grainier than OKLab, and
serpentine scanning cuts the worst grain. Jitter or Ostromoukhov and Zhou–Fang
coefficients add grain.

### Per pixel

For each pixel, with target t (Q16 linear sRGB):

```
(L_ok, a_ok, b_ok) = OK(t)
wc = min(1, hypot(a_ok, b_ok) / 0.04)
kl = 0.05 · (1 − ss(L_ok, 0.90, 0.97))
kc = 0.05 · wc

v  = t + err[x]                                   working value
i  = argmin_j |OK(v) − OK(ink_j)|²                ties → lowest j
e  = v − ink_lin[i]
lum = (e · n̂) · n̂
e′ = e − kl · lum − kc · (e − lum)
```

The nearest ink is the minimum squared Euclidean distance in OKLab between
the working value and each ink's precomputed OKLab. The working value and
the error are never clamped; v outside [0, 1] is defined because the cube
root is signed.

### Damping

The error is damped before it is diffused. It is split into a luminance
part lum, its projection onto n̂ (the white ink's direction in linear sRGB,
i.e. the paper's neutral axis), and a chroma part e − lum:

- the luminance part loses kl = 5%, fading to 0 as the target's OKLab L
  rises from 0.90 to 0.97, so near-white areas keep their full luminance
  error and their mean tone;
- the chroma part loses kc = 5% for chromatic targets, scaled down linearly
  below OKLab chroma 0.04 to nothing for neutral targets, so neutral areas
  stay neutral on average.

Damping trades a small bias for a shorter error memory: error that the inks
cannot pay off nearby decays instead of travelling far and smearing into
neighbouring regions. The coefficients come from the target t, not the
working value, so they are a property of the image, not of the dither
pattern.

Damping chroma on near-neutral targets pulls greys towards the ink that
dominates the mix, green on the EL133UF1, and damping luminance alone leaves
dark colours grainy. Damping luminance up to white leaves a hard edge where the
dots stop. At a 10% loss, separate saturated reds collapse onto one ink.

Most of damping's smoothness comes from dots it stops placing: the loss caps the
accumulated error, so a colour close to an ink may never receive the minority
ink, leaving a dead zone. Grain falls far more where dots are lost than where
they are kept, so keeping those dots gives up the smoothness, and dead zones are
the cost.

### Floyd–Steinberg, serpentine

Each component of e′ is split with the Floyd–Steinberg weights, each rounded
half away from zero, with the fourth tap taking the exact remainder so the
parts sum to e′:

```
ahead         round(7·e′ / 16)
behind-below  round(3·e′ / 16)
below         round(5·e′ / 16)
ahead-below   e′ − (the other three)
```

Rows alternate direction: row 0 (and every even row) runs left to right,
odd rows right to left with the kernel mirrored. Serpentine scanning avoids
the diagonal drift of a fixed scan direction.

The error memory is one row of (W + 2) Q16 triples plus a carry. Each cell
holds the current row ahead of the scan and the row below behind it. For the
pixel at x with direction d, once E[x] has been read:

```
E[x + d] += ahead
E[x − d] += behind-below
E[x]      = carry + below
carry     = ahead-below
```

The carry starts each row at zero. There is one pixel of padding on either
side: taps that fall outside the image land in it, it is never read as a
pixel, and it is zeroed at the end of each row, while the row's last carry is
discarded, so error pushed past an edge is dropped without branches.

### Exactness of the fast paths

Two shortcuts compute the same values as the plain formulas. The nearest-ink
search sums the squared differences in uint64 one component at a time and
abandons an ink as soon as the partial sum reaches the best distance so far
(valid when every component is below 2^30 in magnitude, checked per panel at
init and per pixel). And wc saturates without a square root when
a_ok² + b_ok² is large enough that the rounded chroma is at least 0.04
(2622 LSB in Q16).

## Grey panels

A grey panel has N levels (2 to 16), listed darkest first, whose measured
colours are neutral. The pipeline is the colour pipeline reduced to
lightness: the same S1 lightness, S2 blur and S3 tone code, no adaptation,
vibrance or hull, a target in Y, and a one-channel S7.

### Levels from measurements

Each level's luminance is the CIE Y of its measured sRGB8, computed exactly
in integers from the constant tables (Q16 EOTF × Q16 matrix row = Q32), and
expressed relative to the lightest level with one rounding:

```
Y_i       = Σ_c M_srgb[1][c] · lin(m_i,c)                 Q32, exact
level_y_i = (Y_i · 2^16 + Y_last / 2) div Y_last          Q16
```

so the lightest level is exactly 1 (65536). "White" below means the lightest
level, and b = level_y_0 is the darkest.

Converting colour to grey this way uses luminance. A lightness taken in the
gamma-encoded domain, such as HSL's (max + min)/2, gives pure blue and pure
yellow the same grey although their luminances differ by more than a factor
of ten; Y keeps them apart.

**Why levels depend on the waveform.** An intermediate grey on e-paper is a
partial transition of the particles, set by the waveform: the drive pulses'
sequence and length, the frame rate, the source voltage and the lookup
table (LUT) the controller or driver loads. The same code on the same glass
gives a different reflectance under another waveform, and the end levels can
differ too. A grey calibration therefore describes a glass driven by one
specific waveform; the shipped GDEY075T7 calibrations record theirs, and
another frame rate, voltage, LUT or driver needs its own measurement.

### S1 to S6 on grey

- **S1:** L\*src from source Y, by the colour code. There is no chromatic
  adaptation: a white-point tint has no meaning on grey output. The
  converter keeps L\*src (int16) from the push instead of the sRGB8 row, so
  S1 runs once per pixel.
- **S2:** the same blur.
- **S3:** tone and detail as for colour, with T0 built with W_y = 1 and
  B_y = b, and the midtone curve's L\*min = L\*(b) rounded to Q8. A light
  black on low-contrast glass is where the moving knee of the midtone curve
  applies.
- **S4:** the veil applies; vibrance and adapt are ignored.
- **S5:** L\* is clamped to [L\*min, 100].
- **S6:** the target is the Y of that L\* relative to white:
  t = f⁻¹((L\* + 16)/116), with a rounded division by 29 in Q24, f⁻¹ in Q24,
  and one rounding to Q16. L\* 100 gives exactly 65536, so source white maps to
  the lightest level with zero error.

### S7 on grey: coverage toe

Error diffusion between two levels renders a target just above a level as
sparse, isolated dots of the next level: in mono, for example, lifted
shadows become dark areas speckled with single white dots. The coverage toe
removes the sparsest coverage at both ends of every gap between levels. In
the gap y_g ≤ t < y_{g+1} the target's coverage c (its position in the gap)
is remapped and turned back into a target:

```
gap = y_{g+1} − y_g
c   = ((t − y_g) · 2^16 + ⌊gap/2⌋) div gap                 Q16, rounded
c′  = toe(c)                for c < 1/2
      1 − toe(1 − c)        for c ≥ 1/2
t′  = y_g + round(c′ · gap)

toe(c) = 0                                                 c < d
         c0·(3s² − 2s³) + (c0 − d)·(s³ − s²),             d ≤ c < c0
             s = (c − d) / (c0 − d)
         c                                                 c ≥ c0

d  = 0.03   (1966 in Q16)
c0 = 0.11   (7209 in Q16)
```

toe is 0 below d, the identity from c0, and in between the cubic Hermite
from (d, 0) with slope 0 to (c0, c0) with slope 1, so it is continuous with
a continuous slope and monotone. Coverage below 3% snaps to the level;
coverage from 11% to 89% is unchanged. Targets outside [y_0, y_last) are
unchanged. The gap is found by binary search on `level_y`; s is rounded to
Q16 and h is formed exactly in int64 and rounded once.

These values trade sparse dots against shadow tone: with no toe, dark areas are
grainy with isolated dots, and a toe twice as wide darkens them noticeably.
Where a gap is narrow in lightness, as at the top of the GDEY075T7's 4-grey
mode, the toe spans little L\* and barely changes near-white grain. Zhou–Fang
coefficients and threshold modulation do not improve on the toe, and need extra
tables and a hash.

Rounding c (not flooring it) makes t′ = t exactly wherever c′ = c, so init
stores each gap's identity band (c in [c0, 1 − c0], 78% of the gap) as two
bounds on t in the ditherer's context, and pixels in the band skip the
division and the cubic. The bounds are exact: the band is computed in
integers from the same rounding, and a test checks the fast path against
the full formula for every t.

Everything after the toe uses t′:

```
kl = 0.05 · (1 − ss(cbrt(t′), 0.90, 0.97))
v  = t′ + err[x]
i  = first level with v ≤ thr_i, else the lightest
e  = v − level_y_i
e′ = e − kl · e
```

The nearest level is chosen in cbrt(Y), which is OKLab L for a neutral, with
ties to the darker level. No per-pixel cube root is needed: the thresholds

```
thr_i = ⌊((cbrt(y_i) + cbrt(y_{i+1})) / 2)³ · 2^16⌋,   clamped to [y_i, y_{i+1} − 1]
```

are built at init, and because cbrt is monotone and the floor makes the test
exact for integer v, comparing v with them gives the same choice as
comparing cube roots. v below 0 or above 1 lands on the darkest or lightest
level with no special case. All grey error is luminance, so damping has only
the luminance term. The cube root in kl is skipped where t′ ≤ 47185 (0.72)
or t′ ≥ 60294 (0.92), where the smoothstep is exactly 0 or 1. The error is
then split and diffused exactly as for colour (same split, serpentine
order and dropped edge error), in one channel.

### epi_grey_panel_init

`epi_grey_panel_init` builds a grey panel at runtime from measured levels,
in caller storage of `epi_grey_panel_size` bytes (any alignment; the size
includes the slack), which is 447 B on a 64-bit host. The storage holds the
`epi_panel_t`, its tables and a copy of the levels; the name is kept by
pointer. It validates the config: a non-NULL name and levels, sizes ≥ 1, 2 to
16 levels, σ in (0, 10.5] (so r = ⌈3σ⌉ ≤ 32), a lightest level with Y > 0, `level_y` strictly increasing
in Q16, and a darkest level for which the midtone curve has a monotone knee
(L\*min below 66.29). Codes are free; `epi_pack_row` checks that they fit its
format. A rejection returns `EPI_ERR_INVALID_ARG` with a short English reason, for
logs and people.

It then derives `level_y`, the thresholds and the Gaussian kernel. The
kernel uses the library's own `epi_exp_d` and the same operations, in the
same order, as panelgen's kernel rule, and reproduces panelgen's taps for
every σ = k/100 in (0, 10.5]. Init uses only deterministic arithmetic, so the
same config gives the same panel, bit for bit, on every target. Every grey
panel, runtime or generated, shares one set of presets, `epi_grey_presets`:
the Default, Soft and Deep of [Presets](#presets).

## Colour panels from inks

`epi_colour_panel_init` builds a colour panel at runtime from measured inks, in
caller storage of `epi_colour_panel_size` bytes (any alignment; the size includes
the slack), which is 2,295 B on a 64-bit host. The storage holds the
`epi_panel_t`, its tables and a copy of the inks; the name is kept by pointer.
The panel's presets are `epi_colour_presets`: the four of [Presets](#presets),
EL133UF1's values, shared by every runtime colour panel.

The derivation is in `src/epi_colour_panel.c`, in double, under the rules of
[Init-time floating point](#init-time-floating-point), and `tools/panelgen` runs
the same code for a built-in panel ([Panel tables](#panel-tables)). Quantisation
is q(x, n) = sign(x)·⌊|x|·2^n + 0.5⌋ throughout.

- **Inks.** Each measured sRGB8 value goes through the EOTF to linear sRGB, then
  through the 4-decimal matrix to XYZ. x^2.4 is x²·(x²)^(1/5), with the fifth root
  by Newton from 1, which falls monotonically until rounding stops it. W and B are
  the first inks of largest and smallest Y.
- **Hull.** Every triple of inks, in lexicographic order, whose plane has all the
  inks on one side is a face, oriented so that the inside is n·x + d ≤ 0 with n a
  unit vector. A point within 1e-11 of a plane counts as on it, and faces within
  1e-9 of each other merge, so that coplanar inks make one face: a cube of 8 inks
  has 6 faces. This order is the face order of the tables. Normals round to Q16
  and offsets to Q32, then each offset is lowered until every ink, in Q16, tests
  inside in exact integer arithmetic. Without this an ink could test outside its
  own hull and be moved by S5.
- **Matrices.** The Bradford matrix A (D65 to W/W_y), the black-point scale k,
  `lin_to_panel_adapt` = diag(k)·A·M_srgb with its rows adjusted so each sums
  exactly to W − B in Q16, `lin_to_panel_bpc` = (W_y − B_y)·M_srgb,
  `xyz_to_lin` = M_srgb⁻¹, `lin_to_lms` = M1·diag(1/w) and `lms_to_oklab` = M2.
  The two constant inverses are taken by the adjugate.
- **Inks and n̂.** Each ink's linear sRGB, direct rather than from XYZ so that a
  zero channel stays exactly zero, and its ink-relative OKLab (the white ink is
  exactly (1, 0, 0)), and n̂ = w/|w|.
- **Neutral interval.** The segment of the ray Y·W/W_y inside the hull, shrunk by
  1e-9 in Y so that the end points test inside. Because the black ink need not be
  exactly neutral relative to the white ink, L\*min can sit slightly above the
  black ink's L\*. A face through the origin and W holds the ray in its plane (a
  neutral black and white can make one), where k and d in k·Y + d ≤ 0 are both
  rounding noise; a face with |k| at most 1e-11 therefore bounds nothing.
- **Cusp table.** For each integer hue 0..359, the L\* of maximum in-hull chroma,
  the first maximum winning. The in-hull chroma at a given (L\*, h) comes from a
  probe at chroma 200, bisected at constant L\* and hue for 20 steps with a
  containment tolerance of 1e-7 in XYZ. The search samples 64 L\* values over the
  neutral interval, then runs 4 refinement passes of 17 samples spanning the best
  sample's neighbours, each narrowing the step by a factor of 8 (from about 1 L\*
  to about 2.5·10^-4). That is 950,400 in-gamut tests in double, the slow part, above
  all on a target with no double-precision FPU, so the inner loop tests the face
  that rejected the last point first, as S5 does, and evaluates Y once per L\*.
- **Kernel.** The Q15 Gaussian of [S2](#s2-base) for the panel's σ, from
  `epi_gaussian_kernel_q15`, shared with grey panels.

The optional progress callback of `epi_colour_panel_config_t` is called after each
hue of the cusp search and sees no tables, so it can't affect the panel's determinism;
if it returns false, init returns `EPI_ERR_CANCELLED`.

Inks that can't make a panel return `EPI_ERR_INVALID_ARG` with a reason, which
`epi_colour_panel_init` passes on from the internal `epi_colour_tables_derive`. The
conditions, in order, keep the double arithmetic in its domains and the converter's
integer arithmetic in range:

- The inks span three dimensions and at most 32 faces.
- W's nominal Y is the largest of the inks' nominal Y. Zero or equal nominals pass.
  W is the lightest measured ink, so a pale ink measured lighter than the real white
  would otherwise take its place, and source white would render in that ink.
- W's X, Y and Z are above 512 in Q16, so that W's reciprocal fits int32; B's Y is
  below W's in Q16.
- W has no zero channel: `lin_to_lms` divides by each.
- Every cone response relative to W is in the domain of `epi_cbrt_d`: 0, or in
  [2^-16, 2).
- The ray enters the hull, and L\*min is at least 0 and has a monotone midtone knee
  (`epi_midtone_monotone`).
- The probe is outside the hull: if a bisection never moves its upper end, the
  chroma search has no answer.
- Every cusp is at least 0.5 L\* inside the neutral interval, a margin derived in
  `src/epi_panel_tables.h` from the cusp move's arithmetic ([S5](#s5-gamut-mapping)).
  The search exits at the first hue that fails. An interval under 1 L\* fails at the
  first hue.

Two ranges need no condition. L\*max can't pass 100, because W is on the ray and no
ink is above it in Y. Every entry of `lin_to_panel_adapt` fits Q16, because M_srgb
bounds X/Y and Z/Y whatever the inks (`src/epi_colour_panel.c`).

Four inks never pass. With white and black at two vertices of a tetrahedron,
either the axis has inks on one side only, so the hues on the other have no chroma
to peak, or the black ink is one of the three around the axis, so the hues towards
it peak below L\*min. The config therefore needs at least five: white, black and
three colours around the axis.

Init allocates nothing, and its stack holds the doubles of 16 inks and 32 faces.
A build that only uses const panels doesn't link this file.

## Resampling

The resampler (`src/epi_resampler.c`) resizes 8-bit sRGB by separable
convolution, one axis at a time, in the gamma-encoded domain. It crops a
source rectangle (`src_x`, `src_y`, `src_w`, `src_h`, in float source
pixels) to a dst_w × dst_h rectangle and pastes it at (`dst_x`, `dst_y`) on a
dst_width × dst_height canvas of a background colour; the placement may
overhang or miss the canvas, and only its visible part is computed. The
coefficient computation is derived from Pillow's resampling code (see
`LICENSE`).

### Filters

| filter | kernel | support (source pixels) |
|---|---|---|
| BOX | 1 on (−0.5, 0.5] | 0.5 |
| BILINEAR | 1 − \|x\| | 1 |
| BICUBIC | Keys cubic, a = −0.5 | 2 |
| LANCZOS3 | sinc(x) · sinc(x/3) | 3 |

The bicubic is evaluated as ((a + 2)·x − (a + 3))·x·x + 1 for |x| < 1 and
(((x − 5)·x + 8)·x − 4)·a for |x| < 2. Lanczos uses the library's own sine
(`epi_sin_d`, fdlibm's medium-range argument reduction and kernel
polynomials), not libm.

### Coefficients

For one axis with source extent [x0, x1) and output size n:

```
scale   = (double)(float)(x1 − x0) / n
fs      = max(1, scale)
support = filter support · fs
centre_i = x0 + (i + 0.5) · scale
window_i = [trunc(centre_i − support + 0.5), trunc(centre_i + support + 0.5)),
           clamped to [0, source size)
w_ij    = filter((j − centre_i + 0.5) / fs),   j in window_i
k_ij    = round(w_ij / Σ_j w_ij · 2^22)        half away from zero
```

The extent is formed in float first, as the config holds it. When
shrinking, the support widens by the reduction factor, so the filter
low-passes at the output's sampling rate. Weights are normalised by their
sum in double and then rounded individually to 22-bit fixed point; they are
not renormalised after rounding. Every expression is evaluated as written,
without fused multiply-adds; for BOX on an integer rectangle the last bit
of a centre can decide whether an edge pixel is in the window.

Horizontal coefficients are computed at init and stored per visible output
column (window start and length, and the weights packed end to end). Vertical
coefficients are computed per output row as it is produced, since only one
row's worth is needed at a time.

### Passes and clamping

Each pass accumulates in int32 per channel, starting from 2^21 (so the shift
rounds half up), and the result is

```
out = clamp(sum >> 22, 0, 255)          (a negative sum gives 0)
```

The first pass is clamped to 8 bits before the second pass reads it, which
bounds the ringing of BICUBIC and LANCZOS3 to the 8-bit range between the
passes as well as at the output. Sums stay below 1.4·2^30 for every filter.

### Pass order and copies

Normally the horizontal pass runs first, as each source row is pushed, and
only on the rows the vertical pass will read; the vertical pass runs as each
output row is popped. For very tall sources, when
src_height > 100·src_width and the placement height dst_h is smaller than
src_height, the vertical pass runs first and the horizontal pass second
(each with its own copy rule below, and the 8-bit clamp between them); the
ring then holds source rows rather than resampled ones. If either axis on
this path is a copy, the order makes no difference and the normal order is
used.

An axis is copied instead of convolved when it is the identity: the output
size equals the source size and the rectangle spans the whole axis from 0.
Both axes are copied when the rectangle is an integer crop of exactly the
output size (integer src_x and src_y, with src_w = dst_w and
src_h = dst_h); a crop of exactly the output size along one axis only still
convolves.

### Layout helpers

`epi_layout_cover` and `epi_layout_fit` fill the rectangles for the two
common layouts, in float. **Cover** fills the output and centre-crops: with
scale = max(dst_width/src_width, dst_height/src_height), the axis that sets
the scale takes the whole source extent and the other takes dst/scale (at
most the source extent), centred at (src − size)/2; the placement is the
whole output. **Fit** fits inside the output on the background: with
scale = min(dst_width/src_width, dst_height/src_height), the rectangle is
the whole source, dst_w = round(src_width·scale) (half away from zero, kept
in [1, dst_width]) and likewise dst_h, centred at
⌊(dst_width − dst_w)/2⌋ and ⌊(dst_height − dst_h)/2⌋.

## Fixed-point arithmetic and determinism

The output is a pure function of the input rows and the configuration, and
it is byte-identical on every supported target: host (x86-64 and ARM),
ESP32 (Xtensa and RISC-V) and WASM. A preview computed in a browser therefore
uses exactly the ink codes the device will get. This rests on three rules: the
per-pixel code is integer-only, the init-time floating point uses only
correctly rounded IEEE operations, and nothing depends on
implementation-defined behaviour.

### Number formats

Qn means n fractional bits. Every conversion from a wider value rounds half
away from zero (`epi_round_shift`, or a rounded division by a constant).

| quantity | format |
|---|---|
| linear light, XYZ, matrices | Q16 in int32; products Q32 in int64 |
| L\*, a\*, b\* | Q8 in int32 (int16 for L\*src, base and blur rows) |
| CIELAB ratio X/Xn | Q25 forward, Q24 inverse |
| CIELAB f(t) | Q19 forward, Q24 inverse |
| D, gains, smoothstep results | Q16 |
| tone table T / gain table g | Q8 / Q16, 401 knots at 1/4 L\* |
| cusp move L\*, C\*, anchor | Q16 L\* units; s in Q16 |
| cusp table | Q8 in uint16 |
| hull normals / offsets | Q16 / Q32 |
| OKLab, diffused error, working value | Q16 in int32 |
| damping coefficients | Q24 |
| grey level_y, thresholds, targets | Q16 |
| detail kernel | Q15, taps summing to exactly 2^15 |
| resampler weights | Q22 in int32 |

Q16 for OKLab and the error gives 16 fractional bits with headroom for
working values well outside [0, 1].

### The integer hot path

Everything that runs per pixel is integer code. Right shifts are applied
only to unsigned values; signed results are formed by negating an unsigned
magnitude, so no result depends on how a compiler shifts negative numbers.
Products are formed in int64 and narrowed once. The nonlinear functions are
integer primitives (`src/epi_math.c`):

- **Cube root** (CIELAB f, OKLab), Q16 in and out, signed. The input is
  normalised to 2^(3q)·u with u in [1, 8), so cbrt = 2^q·cbrt(u), and
  cbrt(u) is linearly interpolated in a 385-knot Q30 table (128 intervals
  per octave of u) and rounded once. Error is at most max(1 LSB, 2^-16
  relative); the function is odd and monotone by construction.
- **Cube** (CIELAB f⁻¹): multiplications.
- **Square root** (hypot, the cusp anchor): exact digit-by-digit integer
  square root, rounded to nearest where needed.
- **atan2** (hue, for vibrance and the cusp move): octant reduction, a Q16
  ratio and a 129-knot table of atan in degrees, result in Q8 degrees,
  within 0.01°.
- **smoothstep**: the polynomial in Q30, rounded once to Q16; exactly 0 and
  1 outside the ramp.

Divisions are by init-time reciprocals (the white ink ratios) or by
constants (the CIELAB inverse, which uses exact rational constants such as
(L + 16)/116 = (L·2^8 + 4096)·2^14/29 in Q24).

### Init-time floating point

The tables that depend on the intent or on a runtime panel are built in
double at init: the tone and gain tables, the colour tables of
[Colour panels from inks](#colour-panels-from-inks), the grey thresholds and
L\*min, the Gaussian kernel and the resampler's horizontal coefficients. The
resampler's vertical coefficients are computed the same way, once per output
row as it is popped. This code uses
only + − × ÷, integer arithmetic and exact bit manipulation, which IEEE 754
rounds identically on every conforming target. The transcendental
functions it needs are the library's own, never libm's:

- `epi_cbrt_d`, for +0 and 2^-16 ≤ x < 2 (level Y, the CIELAB ratio and a colour
  ink's cone responses): exact exponent decomposition, six Newton steps and one
  correction step whose residual is computed exactly (Dekker two-product),
  correctly rounded except possibly in near-halfway cases;
- `epi_sqrt_d`, for +0 and positive normal x (a hull face's normal and n̂): the same
  shape with four Newton steps, within 1 ulp;
- `epi_exp_d`, for finite x ≤ 0 (the Gaussian kernel's −i²/2σ², which reaches
  −0 at the centre tap and passes the −746 underflow to +0 for σ below about
  0.026): Cody–Waite reduction by ln 2 and a degree-13 Taylor polynomial,
  within 1 ulp where the result is normal;
- `epi_sin_d` (resampler, and the colour panels' hue directions, with
  cos(r) = sin(r + π/2)): fdlibm's medium-range reduction and kernels,
  within 1 ulp.

Two build conditions complete this. Contraction of a·b + c into a fused
multiply-add rounds differently, so the library builds with
`-ffp-contract=off` (MSVC: `/fp:precise`), in CMake and in the ESP-IDF
component alike. And double expressions are evaluated in double
(FLT_EVAL_METHOD 0), which holds for x86-64 SSE, ARM, Xtensa soft-float and
WASM, but not for 32-bit x87. Float inputs (the intent knobs, σ, the
resampler's rectangle) widen to double exactly.

`src/epi_fp.h`, included first by every source file that does floating
point, enforces both at compile time: a build with excess precision
(`FLT_EVAL_METHOD` other than 0, or 16 on targets with native `_Float16`) or with fused contraction is a compile error, not
silent drift. Clang and MSVC get the contraction pragma; GCC cannot take it
without a warning, so the header instead rejects a GCC target with
`__FP_FAST_FMA` unless `EPI_FP_CONTRACT_OFF` is defined (ISO modes such as
`-std=c11` do not contract and need nothing). Anyone compiling the sources
outside CMake must pass `-ffp-contract=off` and `-DEPI_FP_CONTRACT_OFF` (GCC
and Clang) or `/fp:precise` (MSVC), and must not use `-ffast-math` or an
x87 floating-point mode.

Generated panel tables are stored as integers, so they are identical everywhere
by construction; runtime ones are by these rules.

### Regression hashes

The output is frozen by hashes in the host test suite. The converter tests
hash (FNV-1a 64) the ink codes of four synthetic 157×119 images
(gradient, hue ramp, noise, a neutral page) under each EL133UF1 preset; the grey
tests do the same for three images on a 4-level and a 2-level test panel
under each grey preset; the resampler tests hash the output of each
regression case; and further fingerprints cover the maths primitives (the
double ones over their stated domains), the intent context and the tables of
three runtime colour panels. CI runs the
suite on a host matrix (Linux with GCC and Clang, macOS, Windows with MSVC)
and under Node in an Emscripten build, and
the ESP-IDF test app (`tests/esp_idf`) runs the same suite on ESP32 targets
(Xtensa and RISC-V, under QEMU), so a change that alters any byte of output
on any of them fails. A hash is updated only for an
intentional change to the output: the failing test prints the new values.

## Panel tables

`tools/panelgen` writes the constant tables as C: `src/epi_srgb.c` (the
panel-independent Q16 EOTF table and sRGB matrix), one `src/panels/<panel>.c` per
colour panel, and on request a grey glass's calibrations. `palette.py` holds the
ink tables and `panels.py` the panel definitions; `gen.py` and `grey.py` render C.

### Built-in colour panels

A built-in colour panel's tables come from the library's own derivation
([Colour panels from inks](#colour-panels-from-inks)). panelgen compiles
`tools/panelgen/colour_tables.c` with `src/epi_colour_panel.c` and the sources it
links, with the library's floating-point flags, and that program runs
`epi_colour_tables_derive` and `epi_gaussian_kernel_q15` on the panel's inks and σ.
`gen.py` renders the integers it prints, beside the panel's own presets. Generating
a colour panel therefore needs a C compiler, and gives the panel
`epi_colour_panel_init` builds from the same inks and σ: `test_colour_panel_el133uf1`
requires every table field to match, on every target the suite runs on.

Inks or a σ the derivation refuses, or presets `epi_intent_init` would refuse, fail
generation with the reason, and nothing is written.

### Inputs SHA-256 and the check mode

Each generated file starts with the command that produced it and an
**Inputs SHA-256**: the SHA-256 of a canonical JSON (sorted keys, compact)
of the generator's inputs. For a colour panel that is the panel definition, the
ink table and a format version.
For a grey glass it is the glass definition, the SHA-256 of the levels JSON's
bytes, the sRGB inputs and the grey rules. A changed hash in a diff therefore says
that an input changed, not just the output.

Generation is deterministic, so `python -m panelgen --check` regenerates in memory
and compares byte for byte with the committed files, exiting with status 1 and
naming any stale file; it writes nothing. The pytest suite also regenerates twice
into temporary trees and compares. A colour panel's tables are the same on every
platform, because the C determines them
([Init-time floating point](#init-time-floating-point)); only a grey panel's
header can differ on Windows (tools/panelgen/README.md).

### Grey calibrations

A grey glass can ship as const calibrations, so firmware need not call
`epi_grey_panel_init`. `python -m panelgen <glass> --levels LEVELS.json`
writes `src/panels/<glass>.c` with one `epi_panel_<glass>_<mode>` per mode
(for the GDEY075T7, `_mono` and `_grey4`). The levels JSON has one entry per
mode with the measured sRGB8 (`perceived`, darkest first), `nominal` and
`codes`, plus a `photocal` block whose date and waveform the generator
requires and records in the file header, since the levels are only valid
for that waveform. A JSON marked as a placeholder is refused unless
`--allow-placeholder` is given, and the result is then labelled as such.

`grey.py` computes the tables by `epi_grey_panel_init`'s rules: `level_y` in
exact integers from the same Q16 sRGB tables, and the thresholds and kernel
with line-by-line transcriptions of `epi_cbrt_d` and `epi_exp_d` in the C's
order of double operations (Python floats are IEEE doubles and CPython does
not fuse operations). The test gate compiles each generated file with the
library and requires every table field, the levels and the converter output
to equal what `epi_grey_panel_init` builds at runtime from the same levels.
Generated grey panels define no presets of their own; they reference
`epi_grey_presets`, so linking one also links `epi_grey_panel.c`.

### Adding a panel

See [Adding a panel](adding-panels.md) for the steps and the limits on inks,
levels and sigma.

## Streaming and output

### Converter rows and lag

The converter consumes and produces rows top to bottom; the image height is
in the config, so it knows when the last row arrives. `push` does the work
that depends only on the incoming row: S1's L\*src and the horizontal blur
pass. `pop` does everything for one output row: the vertical blur pass,
S1 (colour only, recomputed from the stored sRGB8) and S3–S7, then the map
from ink indices to ink codes.

Output row y needs the source row y and the horizontally blurred rows
y − r .. y + r, clamped to [0, H − 1], so it is ready once input row
min(y + r, H − 1) has been pushed. Pushing the last row therefore releases
all remaining rows, using the replicated bottom edge. The two rings are
indexed by row number: input row i goes to source slot i mod (r + 1) and blur
slot i mod (2r + 1). A push is accepted only while pushed − popped ≤ r, which
keeps every row a pending output still needs in both rings; the caller pops
until `pop` stops returning `EPI_OK` after every push. A push that breaks
this is a programming error: it asserts in debug builds and is rejected
otherwise (`push` returns `EPI_ERR_NOT_DRAINED`), without touching memory.

The colour converter stores source rows as sRGB8 rather than S1's results,
which halves the source ring at the cost of recomputing S1 (a matrix blend
and three cube roots per pixel) at pop. The grey converter needs nothing
from the source but L\*src, so it stores that (int16) and runs S1 once.

### Workspace

Everything lives in the caller's workspace, carved in this order, each block
rounded up to the alignment of `long long`, `long double` and pointers, with
that alignment minus one byte of slack so any start address works:

| block | colour | grey |
|---|---|---|
| converter state, incl. intent and ditherer contexts | about 3.5 KB (mostly the tone tables) | same |
| error row | (W + 2)·3·4 B | (W + 2)·4 B |
| target row | W·3·4 B | W·4 B |
| blurred-L\* ring | (2r + 1)·W·2 B | same |
| scratch row (L\*src at push, base at pop) | W·2 B | same |
| source ring | (r + 1)·W·3 B (sRGB8) | (r + 1)·W·2 B (L\*src) |

The size depends on the panel (through r and the kind) and the width, not
the height. On a host where that alignment is 8 bytes: the EL133UF1 (r = 18)
needs 254,695 B at W = 1600, of which the blurred-L\* ring is 118,400 B and
the source ring 91,200 B, and 191,895 B at W = 1200; the GDEY075T7 (r = 15)
needs 86,679 B at W = 800. On ESP32 this normally goes in PSRAM.

### Resampler streaming

The resampler keeps a ring of rows as tall as the largest vertical window,
each holding the horizontally resampled visible columns (on the tall path,
the span of source columns the horizontal pass needs). A pushed source row
is processed only if some visible output row reads it; rows outside the
rectangle's vertical window or below the visible placement are counted and
skipped. Output rows come out top to bottom: a background row above the
placement is ready at once, even before any push; a placement row as soon as
the last source row of its window has been pushed; rows below the placement
right after the last placement row. The push contract is the converter's:
pop until `pop` stops returning `EPI_OK` after every push.

The workspace holds the horizontal coefficients (each visible column's taps),
one row of vertical weights, the ring, a scratch array of doubles (one per
tap of the larger window) in which each filter tap is evaluated once, and on
the tall path one row of vertical results. For a 4000×3000 source
cover-fitted to 1200×1600 it is 21,295 B with BOX and 102,495 B with
LANCZOS3. It grows with the source dimensions, because shrinking widens the
windows: a 65535×65535 source needs 2,075,223 B with LANCZOS3. The resampler
rejects a source with a side over 65535 or an area over `max_src_pixels`
(default 100 megapixels), with `EPI_ERR_SOURCE_TOO_LARGE`; the workspace size
is 0 then. A long, thin source at the limit still needs a large workspace, so a
caller handling untrusted images also checks the size against its memory
budget.

The library does not automatically pick a cheaper filter or pre-shrink for
untrusted images, because that would silently change output and add a second
path.

### Output formats

Each converter row is W bytes, one ink code per pixel, mapped from the row of ink
indices. The preview and the panel's bytes both come from those codes, so there is
one route to each result and one conversion can give both.

`epi_pack_row` packs a row of codes for the panel:

| format | row bytes | content |
|---|---|---|
| `EPI_PACK_4BPP` | ⌈W/2⌉ | two codes per byte, high nibble first |
| `EPI_PACK_2BPP` | ⌈W/4⌉ | four codes per byte, most significant bits first |
| `EPI_PACK_1BPP` | ⌈W/8⌉ | eight codes per byte, most significant bit first |

The formats take codes below 16, 4 and 2 respectively (the EL133UF1, with codes up
to 6, and 16-grey panels use 4 bpp; 4-grey panels 2 bpp; mono 1 bpp).
`epi_pack_row` rejects a panel with a code too wide for the chosen format, and a
row holding one. A partial last byte is padded with the code of the lightest ink
(largest measured Y), so padding shows as paper.

The reducer makes the preview, in either of two sets of colours. The **measured**
preview shows what the panel will display, in the measured colours that drove the
dithering, so it is the honest preview of the result; the **nominal** preview uses
the inks' ideal colours, for consumers that identify inks by those colours.

### Preview reduction

The reducer (`src/epi_preview.c`) converts rows of ink codes to RGB888 rows, in
the inks' measured or nominal colours. Each side can be any size from 1 up to the
source's.

Each preview pixel is the area average of its source pixels in linear light,
which is what the eye does to fine dither at a distance. Averaging sRGB values
makes dithered areas too dark: equal black and white dots give 128, not 188.
Dithering at the preview's size would give coarser dots than the panel's.

All arithmetic is integer, so the output is identical on every target:

- **Weights.** On one axis, source pixel i covers [i·dst, (i+1)·dst) and preview
  pixel j covers [j·src, (j+1)·src). The weight is their overlap.
- **Sum.** For each preview pixel and channel, a `uint64_t` sums
  linear × x weight × y weight, where linear is the ink's sRGB8 through
  `epi_srgb_to_lin_q16` (at most 2^16). The total weight is
  src_width·src_height, below 2^32, so the sum fits.
- **Mean.** The sum divided by the total weight, rounded to nearest, halves up.
- **sRGB8.** The nearest entry in `epi_srgb_to_lin_q16`, by binary search. An
  exact midpoint takes the lower entry.

The table is strictly increasing, so one ink alone returns its exact sRGB8, and a
preview the size of the source equals the inks' colours exactly.

Since dst ≤ src, one source row finishes at most one preview row. The rest of
that source row starts the next. Pushes follow the converter's contract. A row
with a code outside the panel's inks is rejected with no change of state.

The workspace depends only on the preview's width:

| block | size |
|---|---|
| state, incl. a linear colour and validity flag per code | about 3.4 KB |
| sums | dst_width·3·8 B |
| finished row | dst_width·3 B |

A preview 300 wide needs 11,487 B where alignment is 8 bytes.
