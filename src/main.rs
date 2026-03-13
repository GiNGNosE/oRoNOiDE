mod simulation;

use simulation::{Laws, Simulation, GRID_SIZE};
use std::sync::Arc;
use std::time::Instant;
use winit::application::ApplicationHandler;
use winit::event::{ElementState, MouseButton, WindowEvent};
use winit::event_loop::{ActiveEventLoop, EventLoop};
use winit::keyboard::{KeyCode, PhysicalKey};
use winit::window::{Window, WindowId};

const WINDOW_SIZE: u32 = 800;

struct GpuState {
    surface: wgpu::Surface<'static>,
    device: wgpu::Device,
    queue: wgpu::Queue,
    config: wgpu::SurfaceConfiguration,
    texture: wgpu::Texture,
    bind_group: wgpu::BindGroup,
    pipeline: wgpu::RenderPipeline,
}

struct App {
    window: Option<Arc<Window>>,
    gpu: Option<GpuState>,
    sim: Simulation,
    pixels: Vec<u8>,
    last_step: Instant,
    step_interval_ms: u64,
    cursor_pos: Option<(f64, f64)>,
    prev_laws: Option<Laws>,
    fine_mode: bool,
}

impl App {
    fn new() -> Self {
        Self {
            window: None,
            gpu: None,
            sim: Simulation::new(),
            pixels: vec![0u8; GRID_SIZE * GRID_SIZE * 4],
            last_step: Instant::now(),
            step_interval_ms: 50,
            cursor_pos: None,
            prev_laws: None,
            fine_mode: false,
        }
    }

    fn init_gpu(&mut self, window: Arc<Window>) {
        let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor {
            backends: wgpu::Backends::all(),
            ..Default::default()
        });

        let surface = instance.create_surface(window.clone()).unwrap();

        let adapter = pollster::block_on(instance.request_adapter(&wgpu::RequestAdapterOptions {
            power_preference: wgpu::PowerPreference::HighPerformance,
            compatible_surface: Some(&surface),
            force_fallback_adapter: false,
        }))
        .unwrap();

        let (device, queue) = pollster::block_on(adapter.request_device(
            &wgpu::DeviceDescriptor {
                label: None,
                required_features: wgpu::Features::empty(),
                required_limits: wgpu::Limits::default(),
                ..Default::default()
            },
        ))
        .unwrap();

        let config = surface
            .get_default_config(&adapter, WINDOW_SIZE, WINDOW_SIZE)
            .unwrap();
        surface.configure(&device, &config);

        let texture = device.create_texture(&wgpu::TextureDescriptor {
            label: Some("sim_texture"),
            size: wgpu::Extent3d {
                width: GRID_SIZE as u32,
                height: GRID_SIZE as u32,
                depth_or_array_layers: 1,
            },
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgpu::TextureDimension::D2,
            format: wgpu::TextureFormat::Rgba8UnormSrgb,
            usage: wgpu::TextureUsages::TEXTURE_BINDING | wgpu::TextureUsages::COPY_DST,
            view_formats: &[],
        });

        let sampler = device.create_sampler(&wgpu::SamplerDescriptor {
            mag_filter: wgpu::FilterMode::Nearest,
            min_filter: wgpu::FilterMode::Nearest,
            ..Default::default()
        });

        let texture_view = texture.create_view(&Default::default());

        let bind_group_layout =
            device.create_bind_group_layout(&wgpu::BindGroupLayoutDescriptor {
                label: None,
                entries: &[
                    wgpu::BindGroupLayoutEntry {
                        binding: 0,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Texture {
                            sample_type: wgpu::TextureSampleType::Float { filterable: true },
                            view_dimension: wgpu::TextureViewDimension::D2,
                            multisampled: false,
                        },
                        count: None,
                    },
                    wgpu::BindGroupLayoutEntry {
                        binding: 1,
                        visibility: wgpu::ShaderStages::FRAGMENT,
                        ty: wgpu::BindingType::Sampler(wgpu::SamplerBindingType::Filtering),
                        count: None,
                    },
                ],
            });

        let bind_group = device.create_bind_group(&wgpu::BindGroupDescriptor {
            label: None,
            layout: &bind_group_layout,
            entries: &[
                wgpu::BindGroupEntry {
                    binding: 0,
                    resource: wgpu::BindingResource::TextureView(&texture_view),
                },
                wgpu::BindGroupEntry {
                    binding: 1,
                    resource: wgpu::BindingResource::Sampler(&sampler),
                },
            ],
        });

        let shader = device.create_shader_module(wgpu::ShaderModuleDescriptor {
            label: None,
            source: wgpu::ShaderSource::Wgsl(SHADER.into()),
        });

        let pipeline_layout = device.create_pipeline_layout(&wgpu::PipelineLayoutDescriptor {
            label: None,
            bind_group_layouts: &[&bind_group_layout],
            push_constant_ranges: &[],
        });

        let pipeline = device.create_render_pipeline(&wgpu::RenderPipelineDescriptor {
            label: None,
            layout: Some(&pipeline_layout),
            vertex: wgpu::VertexState {
                module: &shader,
                entry_point: Some("vs_main"),
                buffers: &[],
                compilation_options: Default::default(),
            },
            fragment: Some(wgpu::FragmentState {
                module: &shader,
                entry_point: Some("fs_main"),
                targets: &[Some(wgpu::ColorTargetState {
                    format: config.format,
                    blend: Some(wgpu::BlendState::REPLACE),
                    write_mask: wgpu::ColorWrites::ALL,
                })],
                compilation_options: Default::default(),
            }),
            primitive: wgpu::PrimitiveState {
                topology: wgpu::PrimitiveTopology::TriangleList,
                ..Default::default()
            },
            depth_stencil: None,
            multisample: Default::default(),
            multiview: None,
            cache: None,
        });

        self.gpu = Some(GpuState {
            surface,
            device,
            queue,
            config,
            texture,
            bind_group,
            pipeline,
        });
        self.window = Some(window);
    }

    fn render(&mut self) {
        let Some(gpu) = &self.gpu else { return };

        self.sim.to_rgba(&mut self.pixels);

        gpu.queue.write_texture(
            wgpu::TexelCopyTextureInfo {
                texture: &gpu.texture,
                mip_level: 0,
                origin: wgpu::Origin3d::ZERO,
                aspect: wgpu::TextureAspect::All,
            },
            &self.pixels,
            wgpu::TexelCopyBufferLayout {
                offset: 0,
                bytes_per_row: Some(GRID_SIZE as u32 * 4),
                rows_per_image: Some(GRID_SIZE as u32),
            },
            wgpu::Extent3d {
                width: GRID_SIZE as u32,
                height: GRID_SIZE as u32,
                depth_or_array_layers: 1,
            },
        );

        let output = match gpu.surface.get_current_texture() {
            Ok(t) => t,
            Err(_) => return,
        };
        let view = output.texture.create_view(&Default::default());

        let mut encoder = gpu
            .device
            .create_command_encoder(&wgpu::CommandEncoderDescriptor { label: None });

        {
            let mut pass = encoder.begin_render_pass(&wgpu::RenderPassDescriptor {
                label: None,
                color_attachments: &[Some(wgpu::RenderPassColorAttachment {
                    view: &view,
                    resolve_target: None,
                    ops: wgpu::Operations {
                        load: wgpu::LoadOp::Clear(wgpu::Color::BLACK),
                        store: wgpu::StoreOp::Store,
                    },
                })],
                depth_stencil_attachment: None,
                ..Default::default()
            });
            pass.set_pipeline(&gpu.pipeline);
            pass.set_bind_group(0, &gpu.bind_group, &[]);
            pass.draw(0..6, 0..1);
        }

        gpu.queue.submit(std::iter::once(encoder.finish()));
        output.present();
    }
}

impl ApplicationHandler for App {
    fn resumed(&mut self, event_loop: &ActiveEventLoop) {
        if self.window.is_some() {
            return;
        }
        let attrs = Window::default_attributes()
            .with_title("Boundary — Holographic Universe")
            .with_inner_size(winit::dpi::PhysicalSize::new(WINDOW_SIZE, WINDOW_SIZE))
            .with_resizable(false);
        let window = Arc::new(event_loop.create_window(attrs).unwrap());
        self.init_gpu(window);

        println!("=== Boundary: Laws-based Lenia (9 meta-params) ===");
        println!("Space       Pause/Resume");
        println!("R           Randomize fields");
        println!("F           Toggle fine mode (1/5 delta, no reseed)");
        println!("M           Mutate all laws");
        println!("N           Mutate R only");
        println!("B           Mutate Blue only");
        println!("G           Mutate Green only");
        println!("Z           Undo last mutation");
        println!("P           Dump laws + derived params");
        println!("Up/Down     dt (step size)");
        println!("1-3/4-6     gc +/- for R/G/B");
        println!("Q/W/E A/S/D gw +/- for R/G/B");
        println!("7/8/9 0/-/= scale +/- for R/G/B");
        println!("F1          F1");
        println!("F2          3-field coexistence");
        println!("F3          F3");
        println!("F6          MUTATED(R) soliton candidate");
        println!("F7          Green near-soliton");
        println!("F8          Green stable soliton");
        println!("LMB/RMB/MMB Perturb R/G/B");
        println!();
        println!("{}", self.sim.laws.summary());
    }

    fn window_event(&mut self, event_loop: &ActiveEventLoop, _id: WindowId, event: WindowEvent) {
        match event {
            WindowEvent::CloseRequested => event_loop.exit(),

            WindowEvent::CursorMoved { position, .. } => {
                self.cursor_pos = Some((position.x, position.y));
            }

            WindowEvent::MouseInput {
                state: ElementState::Pressed,
                button,
                ..
            } => {
                let group = match button {
                    MouseButton::Left => 0,   // R
                    MouseButton::Right => 1,  // G
                    MouseButton::Middle => 2, // B
                    _ => return,
                };
                if let Some((cx, cy)) = self.cursor_pos {
                    let gx = (cx / WINDOW_SIZE as f64 * GRID_SIZE as f64) as usize;
                    let gy = (cy / WINDOW_SIZE as f64 * GRID_SIZE as f64) as usize;
                    if gx < GRID_SIZE && gy < GRID_SIZE {
                        let radius = self.sim.laws.scale[group] * 1.5;
                        self.sim.perturb_group(gx, gy, radius, group);
                    }
                }
            }

            WindowEvent::KeyboardInput { event, .. } => {
                if event.state != ElementState::Pressed {
                    return;
                }
                let PhysicalKey::Code(key) = event.physical_key else {
                    return;
                };
                match key {
                    KeyCode::Space => {
                        self.sim.paused = !self.sim.paused;
                        println!(
                            "{}  step={}",
                            if self.sim.paused { "PAUSED" } else { "RUNNING" },
                            self.sim.step_count
                        );
                    }
                    KeyCode::KeyF => {
                        self.fine_mode = !self.fine_mode;
                        println!("Fine mode: {}", if self.fine_mode { "ON (1/5 delta, no reseed)" } else { "OFF" });
                    }
                    KeyCode::KeyR => {
                        self.sim.seed_random();
                        println!("Randomized");
                    }
                    KeyCode::ArrowUp => {
                        self.sim.params.dt = (self.sim.params.dt * 1.5).min(1.0);
                        println!("dt={:.4}", self.sim.params.dt);
                    }
                    KeyCode::ArrowDown => {
                        self.sim.params.dt = (self.sim.params.dt / 1.5).max(0.001);
                        println!("dt={:.4}", self.sim.params.dt);
                    }
                    // gc: 1-6
                    KeyCode::Digit1 => adjust_law(&mut self.sim, 0, 0, 0.005, self.fine_mode),
                    KeyCode::Digit2 => adjust_law(&mut self.sim, 0, 1, 0.005, self.fine_mode),
                    KeyCode::Digit3 => adjust_law(&mut self.sim, 0, 2, 0.005, self.fine_mode),
                    KeyCode::Digit4 => adjust_law(&mut self.sim, 0, 0, -0.005, self.fine_mode),
                    KeyCode::Digit5 => adjust_law(&mut self.sim, 0, 1, -0.005, self.fine_mode),
                    KeyCode::Digit6 => adjust_law(&mut self.sim, 0, 2, -0.005, self.fine_mode),
                    // gw: Q/W/E (up), A/S/D (down)
                    KeyCode::KeyQ => adjust_law(&mut self.sim, 1, 0, 0.001, self.fine_mode),
                    KeyCode::KeyW => adjust_law(&mut self.sim, 1, 1, 0.001, self.fine_mode),
                    KeyCode::KeyE => adjust_law(&mut self.sim, 1, 2, 0.001, self.fine_mode),
                    KeyCode::KeyA => adjust_law(&mut self.sim, 1, 0, -0.001, self.fine_mode),
                    KeyCode::KeyS => adjust_law(&mut self.sim, 1, 1, -0.001, self.fine_mode),
                    KeyCode::KeyD => adjust_law(&mut self.sim, 1, 2, -0.001, self.fine_mode),
                    // scale: 7/8/9 (up), 0/-/= (down)
                    KeyCode::Digit7 => adjust_law(&mut self.sim, 2, 0, 0.5, self.fine_mode),
                    KeyCode::Digit8 => adjust_law(&mut self.sim, 2, 1, 0.5, self.fine_mode),
                    KeyCode::Digit9 => adjust_law(&mut self.sim, 2, 2, 0.5, self.fine_mode),
                    KeyCode::Digit0 => adjust_law(&mut self.sim, 2, 0, -0.5, self.fine_mode),
                    KeyCode::Minus => adjust_law(&mut self.sim, 2, 1, -0.5, self.fine_mode),
                    KeyCode::Equal => adjust_law(&mut self.sim, 2, 2, -0.5, self.fine_mode),
                    KeyCode::KeyM => {
                        self.prev_laws = Some(self.sim.laws.clone());
                        if self.fine_mode {
                            self.sim.laws.mutate_fine();
                            self.sim.rebuild_from_laws();
                            println!("FINE  {}", self.sim.laws.summary());
                        } else {
                            self.sim.laws.mutate();
                            self.sim.rebuild_from_laws();
                            self.sim.seed_random();
                            println!("MUTATED  {}", self.sim.laws.summary());
                            println!("{}", self.sim.laws.dump());
                        }
                    }
                    KeyCode::KeyN => {
                        self.prev_laws = Some(self.sim.laws.clone());
                        if self.fine_mode {
                            self.sim.laws.mutate_field_fine(0);
                            self.sim.rebuild_from_laws();
                            println!("FINE(R)  {}", self.sim.laws.summary());
                        } else {
                            self.sim.laws.mutate_field(0);
                            self.sim.rebuild_from_laws();
                            self.sim.seed_random();
                            println!("MUTATED(R)  {}", self.sim.laws.summary());
                        }
                    }
                    KeyCode::KeyB => {
                        self.prev_laws = Some(self.sim.laws.clone());
                        if self.fine_mode {
                            self.sim.laws.mutate_field_fine(2);
                            self.sim.rebuild_from_laws();
                            println!("FINE(B)  {}", self.sim.laws.summary());
                        } else {
                            self.sim.laws.mutate_field(2);
                            self.sim.rebuild_from_laws();
                            self.sim.seed_random();
                            println!("MUTATED(B)  {}", self.sim.laws.summary());
                        }
                    }
                    KeyCode::KeyG => {
                        self.prev_laws = Some(self.sim.laws.clone());
                        if self.fine_mode {
                            self.sim.laws.mutate_field_fine(1);
                            self.sim.rebuild_from_laws();
                            println!("FINE(G)  {}", self.sim.laws.summary());
                        } else {
                            self.sim.laws.mutate_field(1);
                            self.sim.rebuild_from_laws();
                            self.sim.seed_random();
                            println!("MUTATED(G)  {}", self.sim.laws.summary());
                        }
                    }
                    KeyCode::KeyZ => {
                        if let Some(prev) = self.prev_laws.take() {
                            self.sim.laws = prev;
                            self.sim.rebuild_from_laws();
                            self.sim.seed_random();
                            println!("REVERTED {}", self.sim.laws.summary());
                        } else {
                            println!("No previous laws to revert to");
                        }
                    }
                    KeyCode::KeyP => {
                        println!("CURRENT  {}", self.sim.laws.summary());
                        println!("{}", self.sim.laws.dump());
                    }
                    KeyCode::F1 => apply_laws(self, Laws::preset_f1(), "F1"),
                    KeyCode::F2 => apply_laws(self, Laws::preset_3field_coexist(), "3-field coexistence"),
                    KeyCode::F3 => apply_laws(self, Laws::preset_f3(), "F3"),
                    KeyCode::F6 => apply_laws(self, Laws::preset_f6(), "F6 (MUTATED R soliton)"),
                    KeyCode::F7 => apply_laws(self, Laws::preset_f7(), "F7 (green near-soliton)"),
                    KeyCode::F8 => apply_laws(self, Laws::preset_f8(), "F8 (green stable soliton)"),
                    _ => {}
                }
            }

            WindowEvent::RedrawRequested => {
                if self.last_step.elapsed().as_millis() >= self.step_interval_ms as u128 {
                    self.sim.step();
                    self.last_step = Instant::now();
                }
                self.render();
            }

            _ => {}
        }

        if let Some(w) = &self.window {
            w.request_redraw();
        }
    }
}

/// param: 0=gc, 1=gw, 2=scale. fine=true → 1/5 delta.
fn adjust_law(sim: &mut Simulation, param: usize, field: usize, delta: f32, fine: bool) {
    let d = if fine { delta * 0.2 } else { delta };
    match param {
        0 => sim.laws.gc[field] = (sim.laws.gc[field] + d).clamp(0.02, 0.4),
        1 => sim.laws.gw[field] = (sim.laws.gw[field] + d).clamp(0.005, 0.05),
        2 => sim.laws.scale[field] = (sim.laws.scale[field] + d).clamp(2.0, 15.0),
        _ => {}
    }
    sim.rebuild_from_laws();
    println!("{}{}", if fine { "fine " } else { "" }, sim.laws.summary());
}

fn apply_laws(app: &mut App, laws: Laws, name: &str) {
    app.prev_laws = Some(app.sim.laws.clone());
    app.sim.laws = laws;
    app.sim.rebuild_from_laws();
    app.sim.seed_random();
    println!("PRESET: {}  {}", name, app.sim.laws.summary());
}

const SHADER: &str = r#"
@group(0) @binding(0) var t: texture_2d<f32>;
@group(0) @binding(1) var s: sampler;

struct VertexOutput {
    @builtin(position) pos: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

@vertex
fn vs_main(@builtin(vertex_index) vi: u32) -> VertexOutput {
    // Full-screen quad from 6 vertices (2 triangles)
    var positions = array<vec2<f32>, 6>(
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),
        vec2(-1.0, 1.0),  vec2(1.0, -1.0), vec2(1.0, 1.0),
    );
    var uvs = array<vec2<f32>, 6>(
        vec2(0.0, 1.0), vec2(1.0, 1.0), vec2(0.0, 0.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(1.0, 0.0),
    );
    var out: VertexOutput;
    out.pos = vec4(positions[vi], 0.0, 1.0);
    out.uv = uvs[vi];
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    return textureSample(t, s, in.uv);
}
"#;

fn main() {
    let event_loop = EventLoop::new().unwrap();
    let mut app = App::new();
    event_loop.run_app(&mut app).unwrap();
}
