# Backdrop Effects and Glass

This document defines the implemented backdrop-effect contract and its explicit capture boundaries.
Public usage is documented in [Frosted and Liquid Glass](../guide/glass.md).

## Native rendering architecture baseline

The repository owner relayed this guidance from the HuxerUI author, translated here from Chinese:

> HuxerUI renders natively and uses the same drawing APIs as native controls, so drawing should follow the corresponding platform's rendering hierarchy.

This is the architectural baseline for glass optimization; the acceptance criteria below are this repository's interpretation of that guidance.
Native rendering means using the platform's drawing, compositing, synchronization and resource ownership boundaries.
It does not require a native control instance for every View, establish pixel equivalence with a native control, or imply that every operation already executes on the GPU.
Prefer a supported native drawing or filter operation when it can preserve the selected material's contract, and keep custom optical processing within the existing platform renderer when that operation cannot express it.

For Linux, GTK owns the host window and scheduling, GtkSnapshot/GSK owns scene presentation, and GDK owns texture/context interoperability.
The following criteria apply to proposed Linux glass optimizations:

| Architecture criterion | Required evidence |
| --- | --- |
| Preserve native ownership | Submit through GtkSnapshot/GSK and supported GDK resources; retain GTK's renderer selection and GTK 4.14 minimum, without a parallel presentation loop or new X11, Wayland or EGL ownership. |
| Preserve the platform hierarchy | Verify current-frame prefix ordering, transparent isolation, clip coverage, alpha replacement and sharp foreground content against focused pixel fixtures; effects must remain at their original paint position. |
| Reuse native operations where compatible | Evaluate available Snapshot/GSK drawing and filtering before adding equivalent custom passes; document kernel, coverage, mapping or capability differences that require the existing optical executor. |
| Keep accelerated optical results on the GPU | Require zero optical-result readback on the admitted resident path; account separately for all input texture downloads, raster/mask uploads, GPU copies and synchronization before claiming a complete frame has no CPU round trip. |
| Preserve ordinary drawing and reuse | Keep effect-free scenes on ordinary Snapshot nodes; invalidate source/filter reuse on source, geometry, clip, scale, viewport and texture-revision changes, and schedule no continuing frames for stationary material. |
| Bound retained native resources | Verify target/cache budgets, immutable published textures, acquire/release synchronization and resize, cancellation and retirement behavior with retained-frame and lifecycle fixtures. |
| Measure the complete path | Report ordinary replay, transfers, filter work, export/interoperability and completed-window frame timing separately; CPU recording time or a filter microbenchmark alone cannot establish a frame improvement. |

GTK's [Snapshot blur operation](https://docs.gtk.org/gtk4/method.Snapshot.push_blur.html) filters content recorded inside that node; it does not automatically select the earlier scene prefix as a backdrop.
Reusing it still requires correct prefix construction, replacement coverage and explicit qualification of the selected quality contract; it cannot silently replace Precise's three-box pixel contract.
Custom optics must not depend on [GskGLShader](https://docs.gtk.org/gsk4/class.GLShader.html), deprecated since GTK 4.16 and unsupported by the rendering infrastructure introduced in 4.14.

### Current Linux implementation against the baseline

`SnapshotScenePainter` records effect-free transforms, opacity, clipping, Cairo batches and external texture nodes into the native Snapshot hierarchy.
Backdrop scenes instead use `SnapshotBackdropScene` and `BackdropScenePainter`: Cairo rasterizes ordinary segments and clip masks, the private GDK GL context maintains the current prefix and optical groups on the GPU, and an immutable `GdkGLTextureBuilder` texture returns the result to GSK.
This satisfies the native ownership and presentation boundary, with resident optical processing, source-compatible planes and bounded reuse; it is still a hybrid path with CPU rasterization and uploads rather than a complete native-node filter graph.
The GPU publication fence and independent release context preserve outstanding frames, but do not prove that every GSK renderer or driver imports that texture without additional copies or waits.

`BackdropScenePainter::Command(const DrawExternalTextureCommand&)` currently calls `gdk_texture_download()` before Cairo replay, so a GPU-backed external source can introduce a GPU-to-CPU transfer followed by a raster upload.
`LinuxBackdropStats::readback_bytes` comes from the optical executor and does not include that external-source download; zero in that counter must not be described as zero readback for every complete glass scene.
Resident fixtures cover optical-result readback, source ordering, masks, cropped raster uploads and immutable texture lifetime; their capture downloads are test observations, not evidence of a production readback.

Future optimization should first qualify direct native/GPU external-texture composition, then reduce Cairo raster and mask transfers by reusing compatible Snapshot/GSK operations or retained resources.
Keep neutral native-blur experiments distinct from general refraction/fusion, preserve the CPU fallback and existing Precise/Fast gates, and measure complete frames before changing backend admission.

## Ownership

Backdrop effects reuse View declarations, modifiers, PaintSequence and RenderScene.
Static configuration belongs to a property modifier; retained optical interaction belongs to NodeExtension and uses MotionController.
UiWindow owns declaration reconciliation, effect bounds and frame damage.
Platform renderers own captured pixels, filter resources and caches.
There is no additional host, registry, shader property bag or component-specific Runtime dispatch.

Brush and VisualFill remain sources of filling colors and images.
BackdropEffect describes processing of previously drawn pixels independently from Background, Border, Shadow and CornerRadius.
Foreground text, editing state, layout, pointer routes and accessibility are not modified by an optical surface.

## Source and drawing order

The source is the already-painted prefix of the active compositing isolation group in the current frame.
A group created for opacity begins with a transparent source; effects inside it do not sample outside that group.
Ordinary transforms and clips do not create a source isolation group.
An opacity group uses a full viewport-sized transparent target; inherited clips are deferred until its final composition, while clips declared inside the group remain active during replay.
The source never includes the effect's own shadow, fill, border, indication, descendants, foreground or drawing that follows its position.
Earlier optical output participates in the source of later effects; this ordering is acyclic.
Reading the previous completed frame is forbidden because it includes old effects and foreground and creates feedback.

The backdrop phase precedes the owner's shadow and ordinary background.
NodeExtension can contribute to this phase through a generic callback; compatible retained extensions preserve declaration order.
Each effect is replayed as a platform-neutral immutable DrawBackdropCommand at its exact paint position.
Transparent filling colors are configured separately so opaque defaults do not hide the processed background.

## Glass planes

Linux renderer replay collects source-compatible glass commands into a plane with one shared capture and one replacement.
A later member can join only when its sampling bounds exclude the pending plane's earlier optical output and any deferred raster work, including conservative capture margins.
Otherwise the renderer finishes the pending plane first, then replays deferred drawing so the later effect can sample its complete original prefix.
Ordinary raster commands between members are buffered with their original transforms and replayed in their original order after the plane completes.
This allows separate sibling glass controls and their foregrounds to share a plane when their source dependencies permit it.
Transform changes are carried in each member and buffered command; clip changes, compositing group boundaries and the end of replay finish a pending plane.
Commands from different opacity isolation groups never share a plane.
These boundaries preserve the source at each command's original paint position rather than sampling the previous frame or moving foreground into the source.

The collector bounds each plane to 16 commands and 256 shapes, and the renderer buffers at most 4096 raster commands; reaching a budget finishes the plane without changing the public per-command shape limit.
Plane batching shares capture work; it does not combine semantic identity or enable geometric fusion between separate commands.
Fusion remains an explicit property of the shapes in one DrawBackdropCommand.

## Android Fast HWUI processing

Android automatically attempts HWUI recording on hardware Canvas targets when every backdrop command in the complete scene selects `BackdropBlurQuality::Fast`.
Admission scans node content, foreground and descendants before recording; any Precise command preserves the existing renderer for the complete scene, including mixed-quality scenes.
Neutral blur requires API 33; optical blur requires API 35 shader clipping and admits up to 64 shapes per command with refraction, edge lighting, brightness, saturation and ordered smooth-union fusion.
Zero-blur optical commands retain the existing GL path for the complete scene; native zero-blur remains outside automatic admission pending complete-scene qualification.
The public default remains Precise; applications select material quality through the existing effect field and need no platform backend selector.
Unsupported capabilities, mappings or bounded resource admission use the existing renderer for the complete scene.
Software Canvas targets and older Android releases retain the existing implementation.

The example runner defaults `-PhuxeruiNativeBlur=auto` to the SDK policy and accepts private `off`, `matched` and `nominal` modes for diagnostics.
Neither comparison mode overrides scene-wide quality or zero-blur admission.
The process-local internal property is not a supported application API.

HWUI records ordinary segments and their current-frame prefix into retained RenderNodes, applies `RenderEffect.createBlurEffect` to a cropped prefix node, and replaces pixels through the original shape and clip program before recording foreground content.
Opacity groups begin with a transparent prefix and apply inherited clipping at composition.
JNI's matrix and save stack uses a one-pixel software proxy while every drawing operation targets a hardware recording; there is no CPU backdrop capture or ImageReader/native-GL handoff in this path.
The candidate admits at most 64 MiB of nominal RGBA filter-layer extent per frame, 256 MiB of simultaneously live isolation-target extent, 512 recording nodes and 64 saved clip/transform operations per surface.
These bounds precede node/filter creation or recording-state growth; the software proxy pool retains at most four one-pixel surfaces.
The filter capture intersects the output with conservative current-clip visibility before adding its complete sampling halo, while replacement retains the original shape and clip stack.
Unused recording slots retire their effects and display lists, aborted candidates retire their complete graph, and an inactive candidate releases previously retained nodes when the existing renderer resumes.
HWUI still owns additional filter intermediates, in-flight frames and its global texture cache; the nominal admission budgets are not a total process or GPU memory limit.

A native attempt publishes its root only after the complete scene records successfully.
Private admission and mapping failures discard the unpublished graph and replay the original scene through the existing renderer with native selection disabled for that attempt.
The destination Canvas remains untouched until successful root publication, so fallback cannot combine half a native frame with a second rendering of its background.
Only the candidate's private unavailable result requests this retry; unrelated Java exceptions and shared renderer errors retain their original exception semantics.
The renderer exposes the committed path and fallback reason through internal diagnostics; these are not public SDK configuration or APIs.
An unchanged rejected scene can reuse the existing completed-frame cache under its original root, revision, configuration, dimensions and device mapping checks; rejection never validates another scene or mutable source.
The private native mode participates in the raster configuration identity, so switching Gaussian comparison modes cannot reuse an admission decision or frame from another mode.
Android consumer rules retain the View and text-layout JNI bridge classes, including callbacks only referenced from C++; Java native-method retention alone does not preserve reverse scheduling, painting or text-metric lookups under R8.
Device-axis blur support swaps correctly under quarter turns with nonuniform scaling, and equivalent reflected mappings retain the same filter dimensions.

The HWUI backend uses per-command filters rather than the shared executor's plane collector or complete source/kernel cache policy.
Fast permits platform Gaussian differences; Precise continues to retain its existing box-filter pixel contract.
Additional physical GPU vendors remain a validation requirement before claiming broad device coverage.

Android instrumentation also contains a separate AGSL capability probe that chains RuntimeShader sampling after RenderEffect blur and uses shader alpha as replacement coverage through `Canvas.clipShader`.
The probe is guarded at API 35, where shader clipping is available, and exercises a two-circle smooth union, edge-normal displacement, directional lighting, partial-alpha replacement and sharp later foreground.
It remains a test-only fusion feasibility probe.

The optical blur path uses one bounded retained shader slot per admitted command, with the same normalized asymmetric rounded-rectangle distance, ordered union, finite-difference normal, logical edge width, light direction and premultiplied sRGB color equations as the shared executor.
Shader capacities grow geometrically from one to 64 shapes and reuse their uniform arrays; the active shape count excludes unused trailing capacity, and empty shapes do not participate in the union.
Array access stays inside constant-bound AGSL loops, passing shape values to distance helpers rather than using a helper argument as an array index.
The private zero-blur diagnostic uses up to four fixed accesses per iteration, each independently guarding the active count and empty shapes while preserving ordered fusion.
This avoids the scalar-loop normal discrepancy reproduced on a software GL driver without the first-use compilation growth of fully unrolling 32 or 64 shapes.
Positive blur retains its scalar shader source; changing between positive and zero blur replaces the retained program while preserving its capacity.
Multiple shapes share one prefix, blur and replacement within their command; independent commands retain their original paint boundaries.
An inverse affine mapping takes device pixels to local geometry; displaced points map back to the cropped device source, with pixel-center clamping at capture edges.
Fusion expansion, the complete refraction reach and Gaussian halo are included before filter admission; shader coverage replaces captured pixels with SRC under the original clip program, preserving partial alpha and later foreground.
The shared GL path retains invertible affine support for zero-blur optics; admitted HWUI blur retains the existing axis-aligned or uniform orthogonal mapping restriction.
Optical shader instances reuse recording slots, and unused or aborted slots release their shader references with the corresponding node graph.
The optical filter output is device-aligned; sampling begins at exact device-pixel centers to prevent interpolation noise from creating a spurious normal at a shared hard-union edge.
Multi-shape optical shaders skip refraction, normal estimation and color sampling beyond a conservative two-pixel local-space distance outside the glass, where the replacement clip has zero coverage.
Single-shape shader capacity retains its original program without this early-exit branch because device comparisons did not show a benefit there.
The retained fringe covers antialiasing and pixel-center alignment under inverse affine mappings; sampling halos and blur processing remain unchanged.
Zero-blur materials with neutral refraction, lighting and color are identity operations, including fusion-only declarations, and bypass optical recording.
Standalone byte fixtures compare zero-blur optics against the shared CPU processor on identical premultiplied input pixels and the same Android-computed inverse mapping coefficients, with the existing max-3/255 and mean-0.5 tolerance.
Ordinary HWUI Bitmap rasterization has a separate identity baseline; identity-material processing preserves those raster pixels exactly.
That standalone diagnostic remains the gate for any future native zero-blur admission; it does not establish Gaussian/box blur equivalence.
The standard SDK-path smoke instead verifies that zero-blur optical scenes retain complete GL pixels, including affine mappings and backend changes.
The Liquid Glass example's L3 workload adds optical parameters to the existing six-surface L2 geometry, while L4 uses two rounded shapes in each command with a 32 DIP fusion radius.
Both support fixed and scrolling source comparisons without changing the material during measurement.

`nominal` passes the transformed public radius directly to Android and therefore does not match the existing blur strength.
`auto` and `matched` approximate the existing three integer box kernels by matching per-axis variance: for physical support `s`, each box has radius `r = ceil(s / 3)`, the three-box variance is `r * (r + 1)`, and Android's current radius-to-sigma conversion is inverted to obtain the RenderEffect radius.
The conversion is an experiment calibrated against Android's implementation, not an Android public API guarantee.
Gaussian and box kernels, edge handling and rounding remain different; Fast has its own image comparisons and does not relax the Precise GPU fixture tolerances.
Performance comparisons must include main-window GPU completion or presentation evidence because hardware recording time alone excludes deferred HWUI filtering.

## Optical values and geometry

Distances use DIPs and are mapped to capture pixels at the renderer boundary.
BackdropShape describes one rounded rectangle; several shapes in one command form one optical surface.
A nonzero fusion radius smooths the union of those surfaces.
Fusion is decorative and does not merge child state, action targets, keyboard focus or semantic identity.

The portable baseline processes premultiplied sRGB BGRA8 pixels.
The blur uses three separable box passes with a finite total support derived from blur_radius.
Sampling beyond capture boundaries extends the nearest captured pixel; captures include the surrounding support needed by the effect.
Refraction displaces bilinear source sampling according to the surface distance gradient, edge width and declared displacement.
CPU, GLSL and AGSL normals use the same central-difference step and suppress gradients below a coordinate-relative floating-point noise floor before normalization.
The floor is the greater of 1e-5 and eight float epsilons times the largest of one, the absolute local coordinates and the absolute surface distance; this keeps medial-axis rounding noise from producing full-strength refraction or lighting while retaining resolved gradients.
The GL executor evaluates geometry at exact pixel centers rather than interpolated vertex coordinates, matching the CPU raster and device-aligned AGSL effect.
Directional lighting, brightness and saturation are applied to the optical layer, not to foreground content.
Processing must preserve premultiplied alpha to avoid dark fringes on transparent edges.
The processor returns the complete updated capture, with coverage interpolation inside the optical surface and unchanged pixels outside it.
The renderer replaces that region under the current clip; source-over against the old prefix would apply captured alpha twice.
Axis-aligned nonuniform blur uses independent physical radii; uniform rotation and reflection preserve the isotropic blur metric.
Rotated or sheared nonuniform mappings with positive blur are rejected until a directional processor can preserve the declared DIP metric; zero-blur refraction retains invertible affine support.

All configuration and geometry must be finite; radii and strengths have documented ranges.
Invalid caller input throws std::invalid_argument with a HuxerUI diagnostic.
Broken recording, capture or framework invariants throw std::logic_error.
Buffers and arithmetic are checked before allocation or indexing.

## Optional fast blur execution

`BackdropBlurQuality::Precise` remains the default and preserves the integer three-box processor.
`Fast` permits an approximate Gaussian on the shared GL executor without changing source-prefix ordering, replacement coverage or foreground rasterization.
The portable processor and unsupported GPU configurations retain Precise processing.
Positive-blur mappings that minify either axis and Gaussian supports above 128 low-resolution samples per side use that fallback.
This is an explicit quality choice rather than an automatic reduction of the default material.
A neutral zero-blur Fast material retains the copied source directly, avoiding floating-point resampling of identity pixels.

The executor matches the three-box variance with `sigma = sqrt(r * (r + 1))` for each physical box radius `r`, truncates at three sigma, normalizes the weights and combines adjacent samples through linear filtering.
The smaller axis chooses a common divisor: 1 at sigma up to 4, 2 up to 8, and 4 above 8.
A four-sample area reduction precedes the 4x case; the 2x case uses one bilinear sample.
Two normalized RGBA8 blur targets operate at that resolution, and a full-resolution normalized result feeds the existing optics shader.
Uniform arrays bound each Gaussian to 65 paired entries, and programs are created only when a Fast command first executes.
Filter workspace allocation includes its divisor and reduced dimensions; source and result cache budgets remain unchanged.

Downsampling begins on a lattice anchored in device space, independently of each capture origin.
Fast sampling bounds include a 12 DIP margin plus the renderer's device margin for the Gaussian tail, downsampling footprint and reconstruction footprint; Precise retains its existing margin.
The clipped-capture path uses the same sampling-outset function so visible clipping cannot discard that halo.
The Fast executor requires non-minifying mappings, keeping that logical margin conservative in physical pixels.
Source-compatible commands still share actual GPU-filtered textures within their plane, but both grouping and retained kernel lookup distinguish Fast from Precise.
A configuration change cannot reuse the other quality's filtered pixels.
Fast has separate Gaussian, alpha, exterior-pixel and capture-phase tests; it does not weaken the Precise CPU/GPU pixel gates.

## Reconciliation, animation and invalidation

Static effect changes invalidate their owner's relevant paint record without changing measurement.
Retained material interaction reads shared hover, press and enabled state and uses the existing frame clock.
Paint-visible parameter changes call InvalidatePaint; FrameResult only requests continuing or delayed frame delivery.
Reduced motion resolves to the target through MotionController.
No optical animation performs per-frame State writes or duplicates component input state machines.

A backdrop depends on source pixels beyond its output region, including blur support and refractive displacement.
Background paint, transforms, scrolling and ExternalTexture publication must damage dependent output even when the glass declaration is unchanged.
Initial effect frames may conservatively replay the complete effect surface; narrower dependencies and renderer caches must preserve that correctness.
The committed damage snapshot stores transformed optical output and sampling bounds and propagates source damage through dependent surfaces until no additional output is added.
Source damage inside an opacity group remains available beyond inherited final clips, since it can feed a visible optical surface before final composition.
This dependency calculation is conservative across ordering and groups; it may add damage but cannot drop required optical output.
Clean node PaintSequences are reused when only their source changes.
No effect frame is requested solely because a stationary glass surface exists.

## Native content and scene capture

The initial contract rejects scenes combining backdrop effects and PlatformView placement.
Independent native view islands cannot be captured by replaying one HuxerUI drawing slice.
There is no silent approximation, stale native snapshot or implicit desktop capture.
ExternalTexture is ordinary paint where the renderer can obtain the published immutable pixels; unsupported host texture paths are rejected explicitly.

FrozenScene and shared-transition subtree capture cannot merely copy a backdrop command, because doing so would read a new live prefix instead of immutable source pixels.
Frozen whole-scene capture rejects live backdrop effects and shared-element capture excludes affected subtrees.
Fragmented sources reject backdrop effects; a renderer slice accepts an effect scene only when it contains the complete raster-command range.

## Platforms and validation

Minimum operating-system and GTK versions remain unchanged.
Every renderer explicitly handles every new PaintCommand variant.
The portable processor is the reference for pixel invariants; renderer-specific acceleration must preserve the source, isolation, clipping and alpha contract.
Linux remains presented through GTK Snapshot/GSK; the implementation must not force a legacy renderer or depend on deprecated GskGLShader.
Effect-free Linux frames retain ordinary Cairo and texture Snapshot nodes.
When a usable private GdkGLContext and supported frame dimensions are available, Linux keeps the composed drawing prefix and transparent opacity targets on the GPU and processes glass planes with GLSL blur and optical shaders.
Ordinary drawing segments and clip coverage masks are rasterized with Cairo and uploaded into those targets at their paint boundaries.
Linux bounds each raster upload, GPU composition and CPU clear to the segment's conservative integral device rectangle.
The source cache compares the compact BGRA rows together with that rectangle, so identical bytes at a different origin cannot reuse an old source.
Positioned text runs conservatively retain the active clip extent because their layout bounds need not contain all glyph ink.
Unclipped planes and group composition omit clip masks; clipped planes reuse one mask until the device-space clip changes within the frame.
Transforms alone do not invalidate a Cairo clip, while entering or leaving an opacity isolation group changes the active clip domain.
GPU mask uploads grow resident coverage through disjoint strips, transferring each covered pixel at most once for that clip until full-frame group composition; changed clips invalidate that coverage explicitly.
Each plane copies its cropped source within the GPU, applies its optical commands and replaces the active target without reading optical pixels back to the CPU.
Masked replacement interpolates the independent original capture and optical output directly into the active group target at the capture's device origin, without a separate clipped-result texture or copy.
The transfer disables blending, samples mask coverage in frame coordinates and honors any integral replacement scissor; an empty scissor writes nothing.
Neither sampled capture aliases the active destination, so the direct transfer preserves transparent prefix pixels and isolation without framebuffer feedback.
The plane can require multiple blur passes and optical draws, while sharing its source capture and final replacement.
Final publication copies the root target into an independent immutable texture through GdkGLTextureBuilder, translating the working BGRA channels to RGBA on the GPU and preserving the row orientation expected by GDK.
Published textures retain their own release context and GPU fence, so later frames can reuse working targets and renderer retirement does not invalidate an outstanding texture.
Without a usable GPU frame path, Linux replays into a viewport-bounded Cairo image, uses the portable CPU processor with the same plane and source boundaries, and publishes an immutable GdkMemoryTexture.
Shader compilation and GPU resource failures report HuxerUI errors; they do not silently change optical quality.
The Linux completed-frame cache is keyed by root identity, committed frame revision, capture bounds and device scale and retires when the scene no longer needs it or the renderer is discarded.
Direct renderer callers must advance RenderFrame::revision whenever referenced paint or presentation changes.

The first root-group glass plane can also reuse an immutable source capture and physical blur kernels across changed frames.
Eligibility requires no earlier optical plane or opacity-group operation in that frame; later planes and isolated groups conservatively capture and filter their current prefix again.
Linux compares ordered compact BGRA raster segments byte for byte; Android hardware raster compares complete immutable paint commands, device transforms, active clips and configuration before assigning reusable texture identities.
Both source keys include frame dimensions and capture rectangles; pointer identity, hashes and frame revision alone cannot establish source equivalence.
Capture rectangles expand to 64-physical-pixel tiles within the frame so small optical displacements can retain the same sampling support.
After three consecutive source-input matches, a moving capture window with one positive blur radius, one blur quality and one linear mapping may retain a full-frame source and kernel when both fit the existing 64 MiB optical-cache budget.
This admission avoids recapturing a fixed background as glass crosses capture tiles; stationary small surfaces and changing inputs retain cropped processing.
Source-input changes and viewport changes reset admission immediately, and full-frame reuse still requires exact source equality and the current physical blur radius.
Unchanged source pixels, physical blur radii and blur quality reuse the cached source and blur result while refraction, lighting, color adjustment and shape geometry are recomputed.
Changed source bytes invalidate the source and every kernel immediately; a new physical blur radius or blur quality computes a distinct kernel from the unchanged source.
Invalidation retains same-size source and kernel texture storage within the existing optical-cache budget, but invalid kernel contents cannot satisfy a cache lookup.
A recomputed kernel may overwrite an invalid same-size slot, including a slot with a different old radius or origin; valid kernels remain available for exact source/radius/geometry matches.
Capture-size changes release the optical cache before allocating its replacement, and viewport changes and renderer retirement release retained storage.
A blur kernel spanning the complete source texture samples that source directly; a smaller kernel still copies into its own cropped texture to preserve edge extension.
Blur intermediates use RGBA8UI storage, so later box passes read the exact rounded bytes without repeatedly normalizing and reconstructing them.
The sixth pass writes a normalized RGBA8 working target directly, retaining the existing hardware bilinear optical sampling without another conversion pass.
The cache adopts that completed normalized target by exchanging texture ownership with an invalid same-size slot or a newly allocated working replacement, avoiding a result-copy pass.
Cached and working texture ownership remains disjoint, and replacements retain immutable GL storage when image-store writes require it.
These paths preserve the six box passes and the existing cache budget without retaining additional working targets.
Retained raster snapshots are bounded by 64 MiB, and the immutable GPU source plus cached blur textures share a separate 64 MiB budget with bounded eviction.
Android hardware raster textures have a separate 64 MiB cache; exact paint programs are bounded by 16 segments, 256 operations per segment and 4 MiB of retained payload.
Immutable raster texture entries retain their device rectangle and compact dimensions; the cache also caps the number of entries at 64.
Android admits a raster texture only after its exact paint program repeats in the next frame; newly changing programs still establish source identity but avoid speculative texture allocation and copying.
Ordinary segments after optical planes may also reuse their own immutable raster textures; changed earlier source pixels do not invalidate an independently unchanged segment.
Cache eviction protects the immediately preceding frame's working set, so a large uncached segment cannot repeatedly displace all reusable entries.
An entry that cannot fit those budgets simply executes uncached at the same optical quality.
Physical blur kernels normally process the union of their members' sampling bounds, including the complete filter and refraction halo; the admitted stable-source path amortizes one full-frame kernel across moving surfaces.
GLES 3.1 and desktop GL 4.3 can use independent 128-pixel sliding-window compute tiles for large kernels; lower capabilities and smaller kernels retain the fragment shader.
Both paths preserve the portable processor's byte rounding after each box pass.
For physical box divisors up to 255, the compute path uses a 16-bit reciprocal with an exact one-unit quotient correction; larger divisors retain extended multiplication.
The rounded byte numerator is below 65536 in the compact path, so its product fits an ordinary 32-bit unsigned integer without changing any output byte.

Windows renders effect scenes into a Direct2D target with cropped CPU readback and source-copy output.
AppKit and UIKit replay effect scenes into Core Graphics bitmap targets with cropped replacement.
Android keeps density-neutral Bitmap/Canvas captures available at API 23; GPU TextureView-backed ExternalTextures are rejected in these scenes.
At API 29 or later, hardware Canvas targets with GLES 3 and HardwareBuffer import use the shared GL plane executor, GLSL bodies and first-root-plane source/kernel cache.
A renderer-owned EGL context retains the prefix and opacity groups without optical readback.
The Android context stays current for one active frame and restores the caller's previous binding at frame release, cancellation or renderer destruction; nested segment scopes do not rebind an already-current context.
At API 33 or later, ordinary segments record into RenderNodes and HardwareRenderer submits them to an ImageReader; the producer fence signals before native EGL imports the acquired image.
The native sampling fence retires before returning that image to the producer.
Android accumulates conservative raster bounds independently of cache eligibility, including deferred foreground replay.
At API 33 or later, a cropped RenderNode root translates that recording into a smaller ImageReader target, and native composition places the compact texture at the original device rectangle.
Positioned text runs conservatively use the full viewport to preserve ink outside layout bounds.
Capture dimensions use 64-pixel size classes; a per-View pool retains the full viewport reader and at most three compact readers with a combined nominal RGBA image extent of 64 MiB.
This budget describes one image per reader, not total producer storage: each reader permits up to four acquired images and platform BufferQueue storage remains additional.
Readers with acquired images are never evicted during replay; a size that cannot fit the pool uses the full viewport reader at the same quality.
Viewport changes, CPU fallback and window retirement release all readers after native sampling retires.
HardwareRenderer does not additionally wait for presentation; image delivery and the producer acquire fence still precede native sampling, and the sampling fence still precedes image release.
Before HWUI acquires a producer buffer, the retained native GL context explicitly flushes pending accesses and retired-image releases; mask uploads or context changes cannot be relied on to submit that work implicitly.
Each raster submits its current content once; `SYNC_FRAME_DROPPED` defers that synchronized content to a later vsync rather than reporting a lost image.
Image acquisition keeps the same bounded delivery wait for immediate and deferred frames, without duplicate submissions or returning a stale raster.
At API 29–32, locked Bitmap rows and a reusable RGBA-to-BGRA staging buffer retain the GPU optical path without depending on unavailable public Image fences.
The mirror tracks clip and transform save/restore operations; inherited clips are deferred until isolated-group composition.
Unpublished Bitmap/Canvas raster and clip targets reuse a per-View pool bounded by four surfaces and 64 MiB; viewport changes, CPU fallback and window retirement release it.
Clip masks upload directly from locked Bitmap storage, restricted to the active plane's capture rectangle; unchanged coverage already resident in that rectangle is reused.
Each pooled clip Bitmap retains the complete ordered rectangular clip program and each clip's device transform, so identical programs can reuse its coverage across frames without repainting or changing its generation.
The key stores at most 64 rectangular clips, including fractional edges, rounded corners and affine mappings; paths and larger stacks conservatively rebuild coverage.
The key belongs to that Bitmap's fixed dimensions and isolation surface, and inherited-group composition uses the parent surface's clip program.
GPU residency still requires matching Bitmap identity, generation and uploaded coverage; equality of a clip key alone never validates another GPU texture.
Native read-only pixel access records the post-unlock generation only after successful execution, since pixel access itself can advance generation; exceptions retire the residency identity.
Android planes whose complete active clip stack consists of axis-aligned, zero-radius rectangles with integral device-pixel edges replace their output through a GPU scissor without generating or uploading a coverage mask.
The scissor applies only to final replacement, preserving source pixels outside the clip that supply the blur and refraction halo.
Fractional edges, rounded rectangles, paths and non-axis-aligned mappings retain coverage masks; opacity-isolation group composition retains its existing inherited-mask path.
Android also derives conservative visible bounds from the transformed clip stack to bound optical work, including fractional rectangles and curved paths.
The executor intersects sampling with those bounds expanded by the complete transformed blur and refraction support plus pixel margins; the original scissor or coverage mask still controls replacement.
Stable full-frame source admission retains its existing extent and exact-input checks, while changing sources can avoid processing clipped-away regions.
An unclipped plane needs no mask upload.
Android can reuse a completed immutable Bitmap when the committed root, revision, dimensions, device mapping and configuration match; scenes containing images or external textures conservatively bypass this cache.
The renderer exports into a fresh RGBA HardwareBuffer, waits at most one second for its export fence, and publishes a hardware Bitmap with an independent deferred-Canvas lifetime.
HardwareBuffer functions resolve dynamically and the JNI publication signature uses Object, so API 23 loaders do not need newer native symbols or Java signature classes.
Unsupported capabilities and software Canvas targets use the existing CPU processor.
Web uses cropped Canvas2D readback and copy composition, rejecting unavailable contexts and unreadable origin-tainted sources.
Windows, AppKit, UIKit and Web currently process each glass command with the portable CPU processor; they do not yet use shader planes.
Every backend uses explicit transparent opacity targets.

Each capture is bounded by 16 * 1024 * 1024 physical pixels and each BGRA target by 64 MiB.
Simultaneously live compositing targets are bounded by 256 MiB; Linux and Android GPU replay count CPU and GPU targets together, while filter temporaries, clip masks, exported textures and texture/image caches remain additional storage.
Finite transformed capture bounds and checked arithmetic precede allocation and integer conversion.
Sampling includes the blur/refraction halo; filter work is cropped rather than processing the whole viewport for each surface.

Tests cover source order, zero blur, transparent edges, blur support, affine transforms, isolated opacity, overlap, source-only changes, texture revisions, retained animation, cancellation, disabling, keyed movement, unmount, capture exclusions and resource retirement.
Plane tests cover source compatibility, intervening raster work, command and shape budgets, and equivalence with immediate per-command replay.
Linux GPU tests explicitly exercise desktop GL and available GLES shader flavors, source-cache hits and invalidation, direct path clipping, nested opacity, negative capture origins and immutable texture retirement.
Visual comparison uses specified tolerances rather than assuming platform text or filter pixels are identical.
The direct Linux and Android GL fixtures for Precise compare complete premultiplied BGRA8 captures with the portable processor: maximum absolute channel error is at most 3 byte levels, and mean absolute channel error over all capture channels is at most 0.5.
Precise identity and uniform-color fixtures require exact equality, and pixels outside conservative optical output bounds must remain unchanged.
Fast retains exact identity and exterior pixels, checks crop-origin stability within one byte level, and accepts at most one byte level of normalized-filter rounding on uniform colors.
The independent full-resolution Gaussian oracle uses maximum 1 and mean 0.5 byte-level limits; reduced-resolution Fast output is not compared against the Precise box kernel with that gate.
These are fixture acceptance limits, not permission to reduce optical quality or a guarantee that arbitrary platform text rasterization matches.
Performance measurements include scene replay, capture transfers, filtering and final composition; batching and shader execution alone do not establish a speedup on a particular device.
The opt-in Linux `LinuxResidentBackdropReportsScrollingMultiPlaneTiming` fixture scrolls eight overlapping glass surfaces at 1600 by 1200 and 2400 by 1800 physical pixels with fixed or changing background pixels.
Its warmed frame timing includes a blocking texture download to complete GPU work, so it reports replay, filtering, publication and validation readback rather than on-screen presentation latency.
Android source UI instrumentation additionally checks hardware Bitmap publication, complete renderer pixels, clip and opacity isolation, retained exports, aborted-frame recovery, viewport resizing, completed-frame reuse and plane counters.
Full-renderer comparison measures optical error beyond an independently rendered identity-material baseline, separating HWUI and software raster antialiasing differences; direct shader fixtures retain the unadjusted tolerance above.
The independent API 35 zero-blur AGSL gate runs with `nativeOutputMode=optics`, including all original max-3/255 and mean-0.5/255 checks even on drivers where the diagnostic fails.
The API 35 native output fixture accepts the instrumentation argument `nativeOutputMode=record` to save deterministic premultiplied RGBA references, followed by `nativeOutputMode=compare` in a rebuilt APK on the same device and configuration with the same fixtures and retained test-app data.
Its 288 cases cover both Gaussian comparison modes, positive and zero blur, changing partial-alpha source pixels, fractional and orthogonal mappings, fusion, sparse multi-shape inputs, clipping, viewport edges and opacity isolation; maximum and mean channel limits remain 3/255 and 0.5/255.
This focused mode reports its own result and does not replace the complete windowless or renderer suites; compare an unchanged build against its recorded bytes to check reproducibility before attributing a difference to a patch.
The separate `nativeOutputMode=benchmark` diagnostic renders zero-blur optical groups with 1, 2, 4, 16 and 64 shapes at 960 by 720 pixels, with four warmup and twenty measured frames per group.
Its per-frame wall times include replay, submission, GPU synchronization and pixel readback, and its first measured pixels are retained for cross-build comparison; these are standalone diagnostic timings rather than isolated shader time or on-screen frame latency.
The Android scrolling harness additionally reports process-owned `drawLayer` extents and their sum normalized by UI recording spans when HWUI exposes those trace names.
These are repeated layer-draw extents, not measured GPU fragment counts, unique texture allocations or an isolated filter-kernel timing.
Warmed workloads cover 800 by 600 and 1264 by 2780 physical pixels; application scrolling still requires separate device traces because a one-plane fixture does not represent a multi-plane page.
Native validation is reported separately for each available host; source review or a successful Linux build is not evidence of another platform's execution.
