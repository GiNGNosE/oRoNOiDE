/// The Boundary - 2D multi-channel Lenia (3 fields → RGB).
///
/// Architecture: Laws (9 meta-params) → Params (derived) → Simulation.
/// 3 fields = 3 fundamental "particles." Cross-coupling emerges from
/// differences in intrinsic properties (gc, scale). Substances are not
/// parameterized individually — they emerge as stable spatial patterns.

pub const GRID_SIZE: usize = 256;
pub const NUM_FIELDS: usize = 3;
pub const NUM_GROUPS: usize = 3;
pub const FIELDS_PER_GROUP: usize = NUM_FIELDS / NUM_GROUPS; // 1
const NUM_KERNELS: usize = NUM_FIELDS * NUM_FIELDS; // 9

// Derivation constants: how Laws map to Params
const KW_RATIO: f32 = 0.13;
const COUPLING_GC: f32 = 0.15;
const COUPLING_SCALE: f32 = 0.10;

struct Kernel {
    entries: Vec<(i32, i32, f32)>,
}

impl Kernel {
    fn ring(radius: f32, width: f32) -> Self {
        let r_max = (radius + 3.0 * width).ceil() as i32;
        let mut entries = Vec::new();
        let mut total = 0.0f32;

        for dy in -r_max..=r_max {
            for dx in -r_max..=r_max {
                let dist = ((dx * dx + dy * dy) as f32).sqrt();
                let w = (-(dist - radius).powi(2) / (2.0 * width * width)).exp();
                if w > 0.001 {
                    entries.push((dx, dy, w));
                    total += w;
                }
            }
        }
        if total > 0.0 {
            for e in &mut entries {
                e.2 /= total;
            }
        }
        Kernel { entries }
    }

    fn empty() -> Self {
        Kernel { entries: Vec::new() }
    }
}

// ── Laws: 9 meta-parameters defining the physics ──
//
// Cross-coupling weight[i,j] = f(gc_diff, scale_diff).
// Antisymmetric: weight(i,j) = -weight(j,i).
// Equal properties → zero coupling → independent fields.

#[derive(Clone)]
pub struct Laws {
    /// Growth center — what potential density each field prefers.
    pub gc: [f32; NUM_FIELDS],
    /// Growth width — selectivity of growth function.
    pub gw: [f32; NUM_FIELDS],
    /// Spatial scale — self-interaction kernel radius.
    pub scale: [f32; NUM_FIELDS],
}

impl Default for Laws {
    fn default() -> Self {
        Self {
            gc: [0.10, 0.12, 0.07],
            gw: [0.018, 0.010, 0.014],
            scale: [5.0, 7.0, 10.0],
        }
    }
}

impl Laws {
    pub fn derive_params(&self) -> Params {
        let mut kernel_radius = vec![0.0f32; NUM_KERNELS];
        let mut kernel_width = vec![0.0f32; NUM_KERNELS];
        let mut kernel_weight = vec![0.0f32; NUM_KERNELS];

        for i in 0..NUM_FIELDS {
            for j in 0..NUM_FIELDS {
                let k = i * NUM_FIELDS + j;
                if i == j {
                    kernel_radius[k] = self.scale[i];
                    kernel_width[k] = self.scale[i] * KW_RATIO;
                    kernel_weight[k] = 1.0;
                } else {
                    kernel_radius[k] = (self.scale[i] * self.scale[j]).sqrt();
                    kernel_width[k] = kernel_radius[k] * KW_RATIO;
                    kernel_weight[k] = Self::cross_weight(
                        self.gc[i], self.gc[j],
                        self.scale[i], self.scale[j],
                    );
                }
            }
        }

        let min_gw = self.gw.iter().copied().fold(f32::MAX, f32::min);

        Params {
            kernel_radius,
            kernel_width,
            kernel_weight,
            growth_center: self.gc.to_vec(),
            growth_width: self.gw.to_vec(),
            dt: (min_gw * 0.8).clamp(0.003, 0.05),
        }
    }

    fn cross_weight(gc_i: f32, gc_j: f32, scale_i: f32, scale_j: f32) -> f32 {
        let gc_avg = (gc_i + gc_j) * 0.5;
        let gc_diff = gc_j - gc_i;
        let scale_avg = (scale_i + scale_j) * 0.5;
        let scale_diff = scale_j - scale_i;
        (COUPLING_GC * gc_diff / gc_avg + COUPLING_SCALE * scale_diff / scale_avg)
            .clamp(-0.4, 0.4)
    }

    pub fn mutate(&mut self) {
        for i in 0..NUM_FIELDS {
            self.gc[i] = (self.gc[i] + rand_delta(0.008)).clamp(0.02, 0.4);
            self.gw[i] = (self.gw[i] + rand_delta(0.002)).clamp(0.005, 0.05);
            self.scale[i] = (self.scale[i] + rand_delta(1.0)).clamp(2.0, 15.0);
        }
    }

    pub fn mutate_field(&mut self, field: usize) {
        self.gc[field] = (self.gc[field] + rand_delta(0.008)).clamp(0.02, 0.4);
        self.gw[field] = (self.gw[field] + rand_delta(0.002)).clamp(0.005, 0.05);
        self.scale[field] = (self.scale[field] + rand_delta(1.0)).clamp(2.0, 15.0);
    }

    pub fn mutate_fine(&mut self) {
        for i in 0..NUM_FIELDS {
            self.gc[i] = (self.gc[i] + rand_delta(0.0016)).clamp(0.02, 0.4);
            self.gw[i] = (self.gw[i] + rand_delta(0.0004)).clamp(0.005, 0.05);
            self.scale[i] = (self.scale[i] + rand_delta(0.2)).clamp(2.0, 15.0);
        }
    }

    pub fn mutate_field_fine(&mut self, field: usize) {
        self.gc[field] = (self.gc[field] + rand_delta(0.0016)).clamp(0.02, 0.4);
        self.gw[field] = (self.gw[field] + rand_delta(0.0004)).clamp(0.005, 0.05);
        self.scale[field] = (self.scale[field] + rand_delta(0.2)).clamp(2.0, 15.0);
    }

    pub fn summary(&self) -> String {
        format!(
            "gc=[{:.3},{:.3},{:.3}]  gw=[{:.4},{:.4},{:.4}]  scale=[{:.1},{:.1},{:.1}]",
            self.gc[0], self.gc[1], self.gc[2],
            self.gw[0], self.gw[1], self.gw[2],
            self.scale[0], self.scale[1], self.scale[2],
        )
    }

    pub fn dump(&self) -> String {
        let p = self.derive_params();
        let mut s = String::new();
        s.push_str("Laws {\n");
        s.push_str(&format!("    gc: {:?},\n", self.gc));
        s.push_str(&format!("    gw: {:?},\n", self.gw));
        s.push_str(&format!("    scale: {:?},\n", self.scale));
        s.push_str("}\n");
        s.push_str("Derived {\n");
        s.push_str(&format!("    kernel_radius: {:?},\n", p.kernel_radius));
        s.push_str(&format!("    kernel_width:  {:?},\n", p.kernel_width));
        s.push_str(&format!("    kernel_weight: {:?},\n", p.kernel_weight));
        s.push_str(&format!("    dt: {},\n", p.dt));
        s.push_str("}");
        s
    }

    // ── Presets ──

    pub fn preset_f1() -> Self {
        Self {
            gc: [0.077, 0.097, 0.064],
            gw: [0.0098, 0.0123, 0.0058],
            scale: [5.1, 4.0, 13.6],
        }
    }

    /// 3-field coexistence: all localized non-equilibrium structures on vacuum.
    pub fn preset_3field_coexist() -> Self {
        Self {
            gc: [0.077, 0.097, 0.064],
            gw: [0.010, 0.0107, 0.006],
            scale: [5.1, 4.0, 13.6],
        }
    }

    pub fn preset_f3() -> Self {
        Self {
            gc: [0.077, 0.097, 0.064],
            gw: [0.0100, 0.0117, 0.0060],
            scale: [5.1, 4.0, 13.6],
        }
    }

    /// MUTATED(R) — R-field soliton candidate.
    pub fn preset_f6() -> Self {
        Self {
            gc: [0.077, 0.108, 0.064],
            gw: [0.022, 0.006, 0.014],
            scale: [5.06, 3.38, 13.56],
        }
    }

    /// MUTATED(G) — green near-soliton.
    pub fn preset_f7() -> Self {
        Self {
            gc: [0.077, 0.097, 0.064],
            gw: [0.022, 0.0075, 0.014],
            scale: [5.1, 2.1, 13.6],
        }
    }

    /// Green stable non-equilibrium soliton.
    pub fn preset_f8() -> Self {
        Self {
            gc: [0.077, 0.097, 0.064],
            gw: [0.022, 0.0107, 0.014],
            scale: [5.1, 4.0, 13.6],
        }
    }
}

// ── Params: derived runtime parameters ──

#[derive(Clone)]
pub struct Params {
    pub kernel_radius: Vec<f32>,
    pub kernel_width: Vec<f32>,
    pub kernel_weight: Vec<f32>,
    pub growth_center: Vec<f32>,
    pub growth_width: Vec<f32>,
    pub dt: f32,
}

// ── Simulation ──

pub struct Simulation {
    pub fields: Vec<Vec<f32>>,
    temp: Vec<Vec<f32>>,
    kernels: Vec<Kernel>,
    pub params: Params,
    pub laws: Laws,
    pub paused: bool,
    pub step_count: u64,
}

impl Simulation {
    pub fn new() -> Self {
        let laws = Laws::default();
        let params = laws.derive_params();
        let kernels = Self::build_kernels(&params);
        let fields = vec![vec![0.0f32; GRID_SIZE * GRID_SIZE]; NUM_FIELDS];
        let temp = vec![vec![0.0f32; GRID_SIZE * GRID_SIZE]; NUM_FIELDS];

        let mut sim = Self {
            fields,
            temp,
            kernels,
            params,
            laws,
            paused: false,
            step_count: 0,
        };
        sim.seed_random();
        sim
    }

    fn build_kernels(params: &Params) -> Vec<Kernel> {
        (0..NUM_KERNELS)
            .map(|k| {
                if params.kernel_weight[k].abs() < 1e-6 {
                    Kernel::empty()
                } else {
                    Kernel::ring(params.kernel_radius[k], params.kernel_width[k])
                }
            })
            .collect()
    }

    /// Re-derive params from laws and rebuild kernels.
    pub fn rebuild_from_laws(&mut self) {
        self.params = self.laws.derive_params();
        self.kernels = Self::build_kernels(&self.params);
    }

    pub fn seed_random(&mut self) {
        for field in &mut self.fields {
            field.fill(0.0);
        }
        for f in 0..NUM_FIELDS {
            let diag_kr = self.params.kernel_radius[f * NUM_FIELDS + f];
            let base_r = (diag_kr * 1.2) as usize;
            for _ in 0..10 {
                let cx = xorshift() % GRID_SIZE;
                let cy = xorshift() % GRID_SIZE;
                let r = base_r.max(3) + xorshift() % (base_r.max(2));
                for dy in -(r as i32)..=(r as i32) {
                    for dx in -(r as i32)..=(r as i32) {
                        let dist = ((dx * dx + dy * dy) as f32).sqrt();
                        if dist > r as f32 {
                            continue;
                        }
                        let nx = ((cx as i32 + dx).rem_euclid(GRID_SIZE as i32)) as usize;
                        let ny = ((cy as i32 + dy).rem_euclid(GRID_SIZE as i32)) as usize;
                        let val = 0.5 * (1.0 - dist / r as f32);
                        self.fields[f][ny * GRID_SIZE + nx] =
                            (self.fields[f][ny * GRID_SIZE + nx] + val).clamp(0.0, 1.0);
                    }
                }
            }
        }
        self.step_count = 0;
    }

    pub fn step(&mut self) {
        if self.paused {
            return;
        }

        for i in 0..NUM_FIELDS {
            for y in 0..GRID_SIZE {
                for x in 0..GRID_SIZE {
                    let mut potential = 0.0f32;

                    for j in 0..NUM_FIELDS {
                        let k = i * NUM_FIELDS + j;
                        let w = self.params.kernel_weight[k];
                        if w.abs() < 1e-6 {
                            continue;
                        }
                        let mut conv = 0.0f32;
                        for &(dx, dy, kw) in &self.kernels[k].entries {
                            let nx = ((x as i32 + dx).rem_euclid(GRID_SIZE as i32)) as usize;
                            let ny = ((y as i32 + dy).rem_euclid(GRID_SIZE as i32)) as usize;
                            conv += self.fields[j][ny * GRID_SIZE + nx] * kw;
                        }
                        potential += w * conv;
                    }

                    let gc = self.params.growth_center[i];
                    let gw = self.params.growth_width[i];
                    let growth =
                        2.0 * (-(potential - gc).powi(2) / (2.0 * gw * gw)).exp() - 1.0;

                    let idx = y * GRID_SIZE + x;
                    self.temp[i][idx] =
                        (self.fields[i][idx] + self.params.dt * growth).clamp(0.0, 1.0);
                }
            }
        }

        std::mem::swap(&mut self.fields, &mut self.temp);
        self.step_count += 1;
    }

    pub fn perturb_group(&mut self, gx: usize, gy: usize, radius: f32, group: usize) {
        if group >= NUM_GROUPS {
            return;
        }
        let base = group * FIELDS_PER_GROUP;
        let r = radius.ceil() as i32;
        for dy in -r..=r {
            for dx in -r..=r {
                let dist = ((dx * dx + dy * dy) as f32).sqrt();
                if dist > radius {
                    continue;
                }
                let nx = ((gx as i32 + dx).rem_euclid(GRID_SIZE as i32)) as usize;
                let ny = ((gy as i32 + dy).rem_euclid(GRID_SIZE as i32)) as usize;
                let idx = ny * GRID_SIZE + nx;
                let val = 0.5 * (1.0 - dist / radius);
                for s in 0..FIELDS_PER_GROUP {
                    let phase = s as f32 / FIELDS_PER_GROUP as f32;
                    let sub_val = val * (1.0 - (dist / radius - phase).abs().min(1.0));
                    self.fields[base + s][idx] =
                        (self.fields[base + s][idx] + sub_val).clamp(0.0, 1.0);
                }
            }
        }
    }

    pub fn to_rgba(&self, pixels: &mut [u8]) {
        for y in 0..GRID_SIZE {
            for x in 0..GRID_SIZE {
                let si = y * GRID_SIZE + x;
                let pi = si * 4;
                for g in 0..NUM_GROUPS {
                    let base = g * FIELDS_PER_GROUP;
                    let mut sum = 0.0f32;
                    for s in 0..FIELDS_PER_GROUP {
                        sum += self.fields[base + s][si];
                    }
                    pixels[pi + g] = (sum.min(1.0) * 255.0) as u8;
                }
                pixels[pi + 3] = 255;
            }
        }
    }
}

fn xorshift() -> usize {
    use std::cell::Cell;
    thread_local! {
        static STATE: Cell<u64> = const { Cell::new(88172645463325252) };
    }
    STATE.with(|s| {
        let mut x = s.get();
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        s.set(x);
        x as usize
    })
}

fn rand_f32() -> f32 {
    (xorshift() % 10000) as f32 / 10000.0
}

fn rand_delta(scale: f32) -> f32 {
    (rand_f32() - 0.5) * 2.0 * scale
}
