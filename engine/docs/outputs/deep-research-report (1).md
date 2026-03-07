# Shape Representation and Mesh Extraction for Destructible Stone SDFs

## Problem framing and what “authoritative SDF” implies

A signed distance field (SDF) represents geometry as a scalar field whose sign classifies inside/outside and whose magnitude approximates distance to the surface; its zero level set is the surface. citeturn3search5 If you treat the SDF as *authoritative*, then meshes become a derived cache that can be regenerated (or partially updated) whenever the field changes.

Dual Contouring (DC) is explicitly designed for extracting meshes from signed grids using Hermite data (edge intersections + normals) and a quadratic error function (QEF) to place vertices, which is why it is a natural partner to an authoritative SDF. citeturn0search0turn4search19 The key architectural consequence is that your “shape correctness” and most gameplay/physics correctness live in the SDF domain, whereas rendering correctness depends on how faithfully and efficiently you *cache* that SDF into meshes (or render it directly).

Your decision surface is therefore: (a) how the narrow-band SDF is stored and queried (precision, bandwidth, locality, update cost), and (b) which DC family member to ship as the deterministic baseline versus which ones to keep as optional quality/fallback modes. 

## SDF storage format under sparse volumetric structures

### Narrow-band and truncation are aligned with how sparse volumes are usually made practical

High-resolution sparse level sets are commonly stored as *narrow-band* signed distance fields because most queries of interest (surface extraction, contact, shading normals) happen near the zero level set; the VDB paper explicitly motivates VDB around high-resolution sparse level sets / narrow-band SDFs. NeuralVDB, which is explicitly positioned as improving VDB-like sparse volumes, also describes narrow-band level sets as truncated SDFs around the surface.

Practically, a “±3 voxel” band is a defensible default for mesh extraction because isosurface methods only need values around cells that straddle the surface, and those cells are necessarily within a small number of voxels of the zero set if the field is reasonably distance-like. This is consistent with tooling ecosystems that treat “band dilation” as a first-class operation (e.g., OpenVDB’s `dilateSdf`).

The deeper trade-off is that truncation is not just a memory optimization: it changes numerical behavior for gradient estimation and for any algorithm that assumes ∥∇φ∥≈1, which matters if you intend to rely on sphere tracing / ray marching for any path (even as a fallback). citeturn0search2turn2search3turn2search7

### Sparse structure choice should be driven by update patterns, not just memory

Because you’re storing stones (many small objects, potentially edited/destroyed locally), the “best” sparse structure depends on whether you optimize for:

- **GPU-friendly read-only traversal** (fast ray queries / sampling), or
- **High-frequency sparse edits** (fast local writes, reallocations, and streaming).

Representative points on this spectrum, with evidence they are used for sparse implicit geometry:

- **VDB / NanoVDB-style hierarchical grids**: OpenVDB is explicitly designed for sparse volumetric data sampled at high spatial frequency and provides a mature tool suite; NanoVDB exists specifically to accelerate OpenVDB-like data on GPUs. citeturn0search11turn0search23turn0search39  
- **Spatial hashing of voxel blocks (“voxel hashing”)**: a well-known approach to allocate and update implicit surface data densely only where needed, with the explicit goal of real-time access and updates without requiring a full regular grid.   
- **Sparse voxel octrees (SVO/ESVO)**: established for efficient traversal and streaming of voxel bricks, including explicit discussion of CPU↔GPU streaming and multi-resolution brick sampling. citeturn5search11turn5search8

For destructible stones, if edits are *frequent and localized*, block hashing tends to be conceptually aligned (allocate bricks where the surface is, update only touched bricks) in the same way it was used for real-time implicit reconstruction workloads. citeturn5search2turn5search6 If edits are *infrequent* but read bandwidth is king (e.g., you constantly sample for ray queries), a GPU-optimized hierarchical layout (NanoVDB-like) becomes more attractive. citeturn0search23turn0search19

### Int8 quantized SDF is plausible, but you should call out exactly where it breaks

Your “int8 quantized SDF” idea is not speculative: large engines have shipped an explicit “8-bit mesh distance field” option, noting it halves memory but introduces artifacts for large or thin meshes. citeturn9search1turn9search0 That is directly relevant to stones because fracture can generate thin slivers and long, thin features.

Where int8 quantization tends to hurt you most is not shading (since you can shade from a separate normal channel), but:

- **Edge intersection placement**: DC needs approximate zero crossing positions along grid edges; quantization error moves intersection points and can amplify vertex placement noise. citeturn0search0turn0search20  
- **Finite-difference gradients**: if you compute ∇φ from neighboring SDF samples, quantization introduces staircasing and jitter, which can destabilize contact normals, friction directions, and any solver that expects smooth gradients. citeturn3search2turn3search12turn3search5

Given you already stated “normals are stored in a separate channel,” the key critical note is: normals-as-a-channel solve *rendering normal fidelity*, but they do not automatically solve *DC/QEF stability* unless those normals are coherent with the SDF zero set and not overly noisy. Classic DC and follow-up work explicitly discuss numerical issues around QEF solving (e.g., rank deficiency / instability), which gets worse when normals/intersections are noisy. citeturn0search20turn4search31

A practical compromise that stays consistent with your plan is:

- keep **int8 (or 8–16 bit) truncated distances** for memory and bandwidth,
- keep a **compact normal channel** specifically for Hermite constraints and shading,
- and reserve **higher precision distance (or filtered distance)** only inside “active interaction zones” (recent fracture region, collision hot spots) where stability matters most. This mirrors the broader idea that SDFs are used because they provide efficient distance/gradient queries for collision and optimization tasks. citeturn3search2turn3search0turn3search9

### Gradient storage vs on-demand: your conclusion is directionally right, but add one missing constraint

Computing gradients on demand via central differences is a standard engineering choice, but it depends on having valid neighbor samples. In a strict narrow-band representation, “neighbors” may be unallocated/missing unless you ensure a one-voxel halo around the band (or a defined fallback value) for finite differences. OpenVDB’s explicit support for band dilation is relevant here because it lets you cheaply grow the band to satisfy stencil needs without recomputing the whole field. citeturn0search31turn0search11

Your recommendation—on-demand gradients, plus caching in high-frequency collision regions—matches how SDF collision literature treats gradients as fundamental but not necessarily something you store everywhere. SDF-based collision detection is motivated by query efficiency and access to distance/gradient-derived information. citeturn3search12turn3search9turn3search5

## Surface extraction choice: Dual Contouring vs Marching Cubes vs direct SDF rendering

Marching Cubes (MC) is the classic isosurface extractor: it linearly interpolates a cell’s edges and emits triangles based on a case table. citeturn0search9turn0search25 It is widely used, but its standard form has well-known ambiguity/topology issues, and substantial literature exists just to fix or guarantee topology. citeturn0search29turn4search6turn4search16

Dual Contouring differs in the way that matters for stones: it uses Hermite data (intersection points and normals) and places one vertex per cell by minimizing a QEF, which is explicitly aimed at sharp-feature preservation without having to explicitly detect features. citeturn4search19turn0search12 For cleavage planes and crystalline edges, that is a structural advantage over MC’s “triangle soup approximating a trilinear interpolant,” especially when you care about crisp planar breaks rather than smooth organic forms. citeturn0search0turn4search5

Direct SDF ray marching (sphere tracing) is viable in principle: it advances along the ray using the distance bound so it won’t step through surfaces. citeturn0search2 But sphere tracing is not a free lunch: it can take many steps near surfaces and becomes challenging to integrate cleanly with rasterization-first pipelines. citeturn2search3turn0search26 If you are path tracing as a primary renderer, there is also evidence that “direct SDF” can be integrated into a path tracing context—but the fast approach in a modern GPU setting tends to build acceleration (BVH) around non-empty voxels and then solve ray–trilinear-SDF intersections carefully, which is substantially more complex than tracing triangles. citeturn2search3turn2search7

Your “mesh extraction as the default” recommendation is therefore defensible specifically because modern GPU-driven pipelines (meshlets/mesh shaders, visibility-buffer-like approaches) are built around triangle/mesh inputs and their associated batching/culling workflows. citeturn2search1turn2search13turn2search17 The important nuance is that “GPU budget in 2030+” does not automatically make direct SDF ray marching cheap; divergence and memory access patterns remain the hard part, and the path-tracing SDF-grid work explicitly emphasizes acceleration structures and specialized intersection routines rather than naive marching. citeturn2search3turn2search7

image_group{"layout":"carousel","aspect_ratio":"16:9","query":["dual contouring sharp feature isosurface example","marching cubes isosurface mesh example medical visualization","sphere tracing signed distance field ray marching diagram","manifold dual contouring adaptive octree crack free result"],"num_per_query":1}

## Dual Contouring variants: what to ship now versus what to treat as an optional module

### The “Classic DC versus everything else” framing is slightly off; the real baseline should be manifold-capable DC

Classic DC (Ju et al.) is established and directly targets sharp features using Hermite data and QEF minimization. citeturn4search19turn0search0 The two real risks you called out—self-intersection/non-manifold outcomes and ill-conditioned QEFs—are real enough that there is a mature follow-up track focused specifically on making DC manifold and stable under adaptivity/simplification. citeturn4search5turn0search20turn4search9

In other words, “Classic DC” is a good conceptual base, but if you’re extracting from **adaptive** sparse structures (octrees, multi-resolution bricks), you should treat **Manifold Dual Contouring (MDC)** as the practical baseline because it explicitly targets crack-free surfaces on adaptive octrees and adds a manifold guarantee. citeturn4search5turn4search12 If you do not adopt MDC-like constraints, you will spend that complexity budget later debugging LOD seams, non-manifold collision meshes, and rare fracture edge cases.

### Neural and learning-based variants are promising, but their cost is more “product risk” than “GFLOPs”

Neural Dual Contouring (NDC) replaces hand-crafted Hermite/QEF computations with network predictions while keeping DC’s “one vertex per cell / quads from edge crossings” structure, and it reports improvements versus several learned/traditional baselines in its evaluation context. citeturn1search0turn1search8 Self-Supervised Dual Contouring (SDC) builds on the NDC line and proposes self-supervised losses so it does not require supervised mesh ground truth. citeturn1search1turn1search5

From a game/engine systems perspective, the hard critique is: even if inference time is acceptable, learned meshing introduces new failure modes (generalization gaps, nondeterminism across GPU/driver changes, QA surface area, asset-dependent tuning) that are *orthogonal* to raw compute budget. The papers’ contributions are real, but they are research-stage systems aimed at reconstruction quality, not at deterministic runtime meshing under adversarial fracture cases. citeturn1search0turn1search1

### Occupancy-based DC is a legitimately good fit for your “incomplete SDF right after fracture” slot

Occupancy-Based Dual Contouring (ODC) is explicitly designed to work from occupancy functions (not distance magnitudes) and is learning-free; it is positioned as GPU-parallel and modifies how edge/face/cell points are computed so it does not rely on distance values. citeturn1search2turn1search38 Importantly for your use case, ODC is described as being based on Manifold Dual Contouring, which aligns it with “crack-free / manifold under adaptivity” thinking. citeturn1search2turn4search5

So your instinct—ODC as a “temporary mesh when only sign/occupancy is reliable”—is one of the few research variants that maps cleanly to a real production need (fast fallback meshing during transient inconsistency). citeturn1search2turn3search0

### FlexiCubes belongs in the ML optimization pipeline bucket, not the core runtime mesher

FlexiCubes is motivated by gradient-based mesh optimization and differentiability; it introduces extra parameters to make isosurface extraction amenable to downstream optimization and uses a dual-grid extraction scheme (Dual Marching Cubes lineage) as part of its design. citeturn1search3turn4search15turn1search7 If you have a learning-based shape generation pipeline (e.g., optimizing implicit fields to match images), FlexiCubes is relevant; for deterministic runtime destructible stones, it is not a straightforward drop-in replacement for DC. citeturn1search3turn1search11

## Mesh vs volume in a path-tracing-first renderer: which passes truly need meshes

If your main renderer is path tracing, you still need a *visibility acceleration strategy*. Triangles integrate directly with hardware ray tracing pipelines via BVHs, which is why triangle meshes remain the default interoperability layer across rendering systems. citeturn2search3turn2search1

However, it is not correct to say “path tracing implies you must have meshes.” There is explicit work on path tracing SDF grids where the fastest strategy on RTX hardware builds a BVH around non-empty voxels and then performs specialized ray–SDF-grid intersection (including continuous normals and shadow-ray optimizations). citeturn2search3turn2search11 The critical takeaway is: direct SDF path tracing is possible, but it is an “engine feature in itself,” not a free alternative to meshing.

For non-PT passes, meshes are still the simplest integration point:

- **GPU-driven rasterization inputs** (meshlets/mesh shaders) assume batched mesh primitives and are designed around triangle/primitive generation and culling before rasterization. citeturn2search1turn2search13turn2search17  
- **Visibility-buffer or deferred-style pipelines** are most naturally expressed as “render primitives + shade later,” which again assumes a cheap primitive representation. citeturn2search1turn2search2

For physics/collision, the situation flips: SDFs are already a first-class collision representation because distance and gradient queries are efficient and directly give you separation/penetration information. citeturn3search12turn3search9turn3search5 There is active research showing robust collision approaches between general SDF solids and between triangles and SDFs, which supports the idea that “authoritative SDF” can be the physics truth, with meshes being optional caches. citeturn3search0turn3search6turn3search8

This leads to a sharper (and more actionable) answer to your question “if PT is main renderer, which passes still need meshes?”:

- **Needs meshes (or is dramatically simpler with meshes):** GPU-driven rasterization-style auxiliary passes (visibility buffers, shadow maps in hybrid pipelines), meshlet-based culling/LOD, and any tooling that expects triangles as the interchange format. citeturn2search1turn2search17turn2search13  
- **Does not inherently need meshes:** narrow-phase collision/contact (SDF-SDF or tri-SDF), “immediate post-destruction” provisional rendering (ray marching / SDF tracing), and PT if you invest in SDF-grid intersection + acceleration rather than triangle BVHs. citeturn3search12turn3search2turn2search3

## LOD strategy: coarse-brick extraction versus mesh simplification

Your LOD question is where DC’s adaptive roots matter the most.

Dual Contouring was extended early toward octree-based simplification and adaptive contouring; and Manifold Dual Contouring explicitly targets crack-free surfaces on adaptive octrees while preserving manifoldness. citeturn4search19turn4search5turn4search12 This is directly relevant to “LOD from coarse bricks,” because the hardest part isn’t generating a coarser mesh—it’s avoiding cracks and topological weirdness at resolution transitions, especially after destruction when topology is dynamic.

Mesh simplification via Quadric Error Metrics (QEM) (Garland–Heckbert) is a mature polygon simplification method and is a strong fit when you already have a stable high-res mesh and want to generate cheaper far LODs. citeturn4search0turn4search8 But for destructible objects, simplification has a systemic cost: *every time the topology changes, you either re-simplify or maintain a multi-resolution mesh structure*, and you also need to keep the simplified mesh consistent with collision, shading, and any “authoritative SDF” truth. citeturn4search0turn0search0

A robust, production-oriented split tends to be:

- **Field-driven LOD (extract from coarser SDF bricks / octree levels):** best when destruction edits the field frequently, because the LOD meshes can be regenerated from whatever resolution levels are currently resident in your sparse structure, and DC/MDC can be designed to be crack-free under adaptivity. citeturn4search5turn5search11turn5search2  
- **Mesh-driven LOD (QEM simplification):** best for stable, mostly-static meshes (or for far-field cached meshes where you can amortize rebuild cost), and it plays well with meshlet generation and GPU-driven pipelines. citeturn4search0turn2search1turn2search24

### Consolidated recommendations aligned to your four discussion points

On your point-by-point feedback, the strongest design I see (given your constraints and the cited state of the art) is:

- **Surface extraction:** keep DC as primary because sharp-feature preservation is structurally built into DC via Hermite constraints and QEF placement. citeturn0search0turn4search19 MC remains a useful debugging baseline (easy to implement), but if cleavage planes matter, MC’s known ambiguity/topology issues and its tendency toward less feature-aware surfaces make it a weaker default unless you invest in one of the “topology guarantee” variants and still accept softer features. citeturn0search29turn4search6turn4search32  
- **SDF storage and int8 extraction quality:** int8/truncated distance storage is defensible and even shipped in industry distance-field pipelines, but you should explicitly guard thin/large fracture artifacts (the failure mode called out in engine docs). citeturn9search1turn9search0 If normals truly exist as a separate channel, they protect shading normal quality, but you still need QEF stabilization and coherence constraints to prevent quantization noise from blowing up vertex placement. citeturn0search20turn4search31  
- **Mesh vs volume rendering under PT:** treat “mesh extraction” as the default interoperability layer for GPU-driven subsystems; keep direct SDF tracing as an explicit fallback path, not a hidden assumption, because high-performance SDF-grid tracing typically requires specialized acceleration and intersection math, especially in PT. citeturn2search3turn2search1turn2search13  
- **LOD:** prefer field-driven LOD via adaptive sparse structures + manifold-capable DC to prevent cracks across resolution, and use mesh simplification (QEM) only for stable/cached far LODs where rebuild rate is low. citeturn4search5turn4search0turn5search11

Finally, on your DC-variant plan: implement **Manifold Dual Contouring-class behavior as the real baseline** (even if you call it “Classic DC” internally), design the meshing API so you can slot in ODC as a transient occupancy/sign-only fallback, and treat SDC/NDC as optional offline or “ultra quality” experiments until you can prove determinism and robustness in adversarial fracture cases. citeturn4search5turn1search2turn1search1turn1search0