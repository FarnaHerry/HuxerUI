# Frosted and Liquid Glass

Backdrop effects process pixels already drawn behind a surface in the current HuxerUI scene.
They leave the surface's foreground text, child controls, layout, pointer targets and controlled values intact.
Include `<huxerui/effects.h>` or `<huxerui/huxerui.h>`; the same names are exported by `import huxerui;`.

Glass follows HuxerUI's native rendering architecture: effects belong at their paint position in the platform's drawing and compositing hierarchy.
On Linux, GTK/GSK owns presentation and GDK owns texture interoperability; the accelerated optical path still includes Cairo rasterization and uploads, and external texture sources may require a download.
The [native rendering baseline and acceptance criteria](../design/backdrop-effects.md#native-rendering-architecture-baseline) distinguish the implemented native integration from remaining transfer and native-filter work.

## A frosted surface

Apply `BackdropBlur` to an ordinary View and give it a translucent fill:

```cpp
return Column {
  Text("Frosted glass"),
  Text("The background is blurred; this text stays sharp."),
}.With(
    Padding(20.0F),
    Spacing(8.0F),
    BackdropBlur{18.0F},
    Background{Color::Rgb(255, 255, 255, 0.14F)},
    CornerRadius{20.0F}
);
```

Place background content earlier in a `Stack` so it is painted before the glass surface.
An opaque background on the surface hides its backdrop effect.
Blur radius uses DIPs; zero preserves background detail.
The modifier uses the View's complete bounds, including Padding, and its resolved corner radii.
It does not make the native window transparent or sample another application or the desktop.

## Liquid material

`BackdropFilter` applies static optical parameters; `LiquidGlass` also provides retained hover and press feedback:

```cpp
const BackdropEffect material{
    .blur_radius = 16.0F,
    .refraction = 6.0F,
    .edge_width = 12.0F,
    .highlight = 0.24F,
    .light_direction = {-0.5F, -1.0F},
    .saturation = 1.1F,
    .brightness = 1.0F,
};

return Button("Continue").With(
    Background{Color::Rgb(255, 255, 255, 0.12F)},
    CornerRadius{24.0F},
    LiquidGlass{material}
);
```

Refraction displaces background sampling near the rounded edge; `edge_width` controls that optical band.
Highlight strength is in `[0, 1]`, and its light direction must be finite and nonzero.
Blur radius, refraction, fusion radius, brightness and saturation must be finite and non-negative; edge width must be positive.
Invalid configuration throws `std::invalid_argument` with a HuxerUI diagnostic.
These parameters process premultiplied sRGB pixels; they do not tint or warp foreground labels.
Use `Background`, `Border` and `Shadow` for the corresponding surface decoration.

Hover and press use the existing interaction state and `MotionController`, including keyboard activation.
Cancel, disabling and unmount release the interaction; reduced motion resolves immediately.
The material does not write application State on each animation frame or create another pointer target.
Static blur/filter declarations replace earlier static declarations on the same View.
One retained `LiquidGlass` or `GlassGroup` material replaces inherited or preceding static material; adding another retained material or a static material after it is rejected.

## Optional fast blur

`BackdropEffect::blur_quality` defaults to `BackdropBlurQuality::Precise`, retaining the three-box filter and its pixel contract.
Choose `BackdropBlurQuality::Fast` explicitly when an approximate blur is acceptable:

```cpp
BackdropEffect material{
    .blur_radius = 18.0F,
    .refraction = 5.0F,
    .highlight = 0.24F,
    .blur_quality = BackdropBlurQuality::Fast,
};
```

The shared Linux and Android GPU executor uses a separable Gaussian with paired linear samples and radius-dependent 1x, 2x or 4x downsampling.
Only blur intermediates change resolution; shape coverage, refraction, lighting, clipping and foreground retain the original capture resolution.
Its sampling lattice stays aligned to device pixels as capture rectangles move, and captures include the reconstruction halo.
Fast output is not pixel-equivalent to Precise and may differ between rendering capabilities.
The portable CPU processor retains Precise processing; the shared GL executor also retains it for minifying mappings and kernels exceeding its bounded Gaussian support.
On Android hardware Canvas targets, a scene containing only Fast backdrop commands can use HWUI directly: neutral blur from API 33, and optical blur from API 35.
Precise or mixed-quality scenes and zero-blur optical scenes retain the existing renderer for the complete frame.
Unsupported capabilities or resource admission also preserve the complete original-renderer frame; applications need no backend selector.
HWUI keeps source-prefix recording, filtering and foreground composition on its own rendering path.
The existing transform restrictions and capture budgets still apply.
Measure complete frames on the intended device: cached stationary blur can leave little filtering work for Fast to save.
[Structural snapshots](testing.md#structural-captures) record `blur_quality`, so tests distinguish material policy changes even when geometry is unchanged.

## Theme materials

Background-owning component styles expose optional typed effects:

| Style | Field | Surface |
| --- | --- | --- |
| `ButtonStyle`, `ChipStyle` | `backdrop_effect` | Control background |
| `SelectStyle` | `backdrop_effect`, `popup_backdrop_effect` | Trigger and popup independently |
| `TextFieldVariantStyle` | `backdrop_effect` | Editor surface of the selected variant |
| `ComboBoxStyle` | `popup_backdrop_effect` | Suggestions popup |
| `DialogStyle`, `MenuStyle`, `TooltipStyle` | `backdrop_effect` | Presentation surface |

```cpp
ButtonStyle style = ButtonStyle::Default();
style.background = Color::Rgb(255, 255, 255, 0.12F);
style.backdrop_effect = BackdropEffect{.blur_radius = 18.0F};

ThemeDefinition definition;
definition.Set(style);
return Theme {
  std::move(definition),
  Button("Themed glass"),
};
```

Include `<utility>` for `std::move`.
Configure disabled and selected fills along with normal fills so every state has sufficient contrast.
A TextField variant's effect covers only its editor rectangle; labels and supporting or validation text stay outside that optical surface.
An explicit generic effect modifier on TextField instead covers the complete outer View and suppresses the inherited editor effect.
Text, selection, composition and revision remain owned by `TextEditingValue` and the normal input session.
For custom Popup or Dialog content, apply the generic modifiers to its surface; a scrim remains a separate layer.

Applications can provide an opaque Theme without optional effects for reduced transparency or increased contrast.
There is no new automatic native reduced-transparency detector; application policy belongs in the existing Theme and Environment.

## Fusion

`GlassGroup` draws one optical material behind a container's direct child surfaces:

```cpp
return Row {
  Button("Left").With(Background{Color::Transparent()}).Key("left"),
  Button("Right").With(Background{Color::Transparent()}).Key("right"),
}.With(
    Spacing(8.0F),
    GlassGroup{BackdropEffect{
        .blur_radius = 16.0F,
        .refraction = 6.0F,
        .highlight = 0.2F,
        .fusion_radius = 16.0F,
    }}
);
```

Remove inherited child effects when the shared material should be the only optical layer.
Child movement updates the shared rounded geometry without combining their click actions, state, focus or semantic identities.
Fusion smooths their decorative union and may paint into the gap; hit testing keeps each child's original shape.
Children must map to axis-aligned, non-reflected rounded rectangles with uniform scale in the group's coordinates.
Rotated, skewed and nonuniform child mappings are rejected.
A command accepts at most 64 shapes; custom Canvas painters can use `PaintContext::DrawBackdrop` with a copied `std::vector<BackdropShape>`.

## Source, limits and cost

The source is the current frame's already-painted prefix of the active compositing group.
The surface's own shadow, fill, text, descendants and later drawing are excluded.
Earlier glass output can be sampled by later glass; the previous finished frame is never used as a source.
Opacity below one creates a transparent isolation group, so glass inside that group samples only earlier drawing inside the same group.
Inherited clips apply when the isolated group is composed; ordinary clips and transforms do not themselves isolate the source.
This also applies during opacity-based presentation transitions.

Same-frame capture paths are implemented for Linux, Windows, macOS, iOS, Android and Web without raising their minimum platform versions.
Linux uses GPU shaders for optical processing when a usable GL context is available, with the portable CPU processor as its fallback.
Framework-owned `linux::GlTexture` sources can remain on the GPU for integral one-to-one placement with shared contexts, opaque or premultiplied input, unit image opacity and no explicit clip; other cases retain Cairo replay and may download the source.
This does not change Precise/Fast selection, blur strength, source ordering or the CPU fallback.
Android uses the same shader processing and plane cache on hardware Canvas targets at API 29 or later when GLES 3 and HardwareBuffer import are available.
Software Canvas targets, API 23–28 and unavailable GPU capabilities use the portable CPU path; the minimum Android API remains 23.
Windows, macOS, iOS and Web currently use the portable CPU processor for each glass command.
Blur supports axis-aligned nonuniform scale and uniform scale with rotation or reflection; combined rotated/sheared nonuniform blur is rejected explicitly.
Zero-blur optical processing supports invertible affine mappings.
Scenes combining backdrop effects with native `PlatformView` content are rejected.
Android GPU ExternalTextures hosted by TextureView cannot participate in backdrop scenes; readable immutable texture paths retain ordinary paint ordering.
Web sources must be origin-clean and the Canvas context must be available.

Frozen whole-scene capture rejects live backdrop effects, shared-element capture skips affected subtrees, and fragmented sources reject them.
Use ordinary retained presentation motion for glass instead of an operation requiring an immutable scene capture.
Partial scene slices cannot provide the complete source prefix and are rejected.

Each capture is limited to 16 * 1024 * 1024 physical pixels, or 64 MiB of BGRA storage; simultaneously live compositing targets are limited to 256 MiB.
The filter uses additional bounded working buffers, so those target limits are not total process-memory limits.
Exceeding a renderer capture budget reports a HuxerUI error rather than reducing optical quality silently.
Effect frames replay the scene conservatively and filter cropped regions with their sampling halo; Linux can reuse the completed image for an unchanged committed frame.
Linux and the Android GPU path group source-compatible surfaces into glass planes, sharing one capture and replacement across their commands.
A plane ends when later glass needs earlier optical output or buffered foreground drawing in its source, or when clipping or compositing groups change.
Ordinary foreground drawing retains its original order and transforms after the plane's optical processing.
This batching preserves paint order and does not fuse separate surfaces; use `GlassGroup` for geometric fusion.
Both shader paths keep the composed background and opacity groups on the GPU and publish immutable GPU images; optical planes do not read results back to the CPU.
Ordinary drawing uses Cairo on Linux, Bitmap/Canvas on Android API 29–32, and hardware RenderNode recording on Android API 33 or later.
Linux limits ordinary raster transfers and clearing to conservative drawing bounds and reuses unchanged clip coverage within a frame; unclipped surfaces need no clip mask.
Android hardware raster imports acquired GPU images after producer synchronization; clip masks still use Bitmap storage and upload only the active capture region.
Android bounds changing-source optical work by the current clip's conservative visible rectangle and retains the complete surrounding blur and refraction support.
Fractional and curved coverage continues to use the original mask; this optimization does not reduce the declared blur radius or image resolution.
At API 33 or later, ordinary segments use cropped hardware targets and compact cached textures when a bounded size pool can accommodate them; other sizes retain full viewport rendering.
Rasterization, synchronization and transfer costs remain part of each changing frame.
For the first root-group glass plane on either GPU backend, unchanged background pixels and physical blur radii can reuse the source and blur while optical feedback changes.
The cache compares actual background raster bytes; background motion, changed capture regions and different blur kernels require the corresponding work again.
Later glass planes and opacity groups currently use conservative uncached processing.
The additional source cache is bounded by 64 MiB of retained raster snapshots and 64 MiB of GPU textures; exceeding these cache budgets preserves quality through uncached execution.
Android exports each completed frame to a new HardwareBuffer-backed Bitmap retained independently by the deferred hardware Canvas.
Because Bitmap publication has no acquire-fence argument, the producer waits for its export fence before presenting; the wait and raster uploads remain part of frame cost.
A stationary effect schedules no continuing frames.
Large surfaces, higher device scale and many fused shapes increase filtering and transfer costs; measure complete frames on the intended devices before using full-screen animated glass.

The [Liquid Glass example](../../examples/liquid_glass/main.cpp) demonstrates moving background content, frosted panels, liquid buttons, controlled text editing, anchored presentation and fusion/separation.
Its [build instructions](../../examples/liquid_glass/README.md) include the Android example runner and ADB installation.
Build it with `cmake --build <build-directory> --target example_liquid_glass` in a configuration with `HUXERUI_BUILD_EXAMPLES=ON`.
See [Backdrop Effects and Glass design](../design/backdrop-effects.md) for renderer ownership and invariants.
