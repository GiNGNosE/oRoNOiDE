# 決定 5: 表面レンダリング設計 — BRDF・光学応答・Implicit Function Detail・LOD

**関連レポート**: `engine/docs/outputs/hyperreal_stone_technology_report.md` Section 2

---

## 設計原則

> **PT + Implicit Function の組み合わせが、BRDF の単純さを許す。**

ラスタライザでは glint・SSS・薄膜干渉を全て BRDF に押し込む必要があるが、PT ではそれぞれが本来属するレイヤー（ジオメトリ、transport、Fresnel）で処理される。BRDF は単一モデルに統一し、視覚的複雑さはジオメトリ（implicit function）と light transport（volumetric scattering）から得る。

---

## 決定 5a: BRDF / Material Model

> **Anisotropic GGX (Cook-Torrance) + Energy Compensation。単一モデル、LUT パラメタライズ。**

### 決定理由

1. 鉱物の見た目の違いはモデルの違いではなくパラメータの違い。硝子光沢（低roughness）、真珠光沢（anisotropy）、土状光沢（高roughness）は全て GGX パラメータ範囲内
2. 鉱物ごとに異なる BRDF モデルを分岐すると、ワープ内スレッドが diverge し GPU 性能が崩壊する。単一モデル + LUT lookup なら全スレッドが同一コードパス
3. PT + implicit function により、macro glint は実際のジオメトリとして、SSS は volumetric transport として処理される。BRDF に複雑さを持たせる必要がない

### パラメータ導出

```
F0:          LUT[mineral_id].ior → Schlick or full Fresnel
roughness:   LUT[mineral_id].base_roughness × f(weathering_degree) + procedural noise
anisotropy:  LUT[mineral_id].anisotropy_ratio
tangent:     crystal_orientation から導出
albedo:      lerp(LUT[mineral_id].albedo, LUT[weathering_product_id].albedo, weathering_degree/255)
             × procedural variation
```

### 却下した選択肢

| 選択肢 | 却下理由 |
|--------|---------|
| Per-mineral BRDF 分岐 | GPU divergence。GGX パラメタライズで同等の表現力 |
| Disney/Principled BRDF | Sheen/Clearcoat/Metallic が鉱物に不要。SSS を BRDF に内包するが、本設計では volumetric transport で処理するため冗長。物理量（IOR, extinction）から Disney パラメータへの間接変換が不要な複雑さ |
| スペクトルレンダリング | 複屈折・分散は魅力的だが、RGB で十分な知覚品質。ROI が低い |

---

## 決定 5b: Subsurface Scattering

> **BRDF 外の volumetric transport として統合。分岐なし、extinction 値が挙動を連続的に制御。**

### 設計

レイが表面を通過（SDF < 0）したら volumetric scattering に移行する。「不透明」と「半透明」を分岐せず、extinction の大小で自然に挙動が変わる：

- 不透明鉱物（高 extinction）→ 数ボクセルで吸収 ≈ Lambertian 相当
- 半透明鉱物（低 extinction）→ 深く散乱 → SSS 効果が自然に発生

### パラメータ導出

```
extinction:        LUT[mineral_id].extinction × (1 + porosity_factor)
scattering_albedo: LUT[mineral_id].scattering_albedo
phase_function:    Henyey-Greenstein, g = LUT[mineral_id].phase_g
```

### 性能懸念

高 extinction の不透明鉱物に対して毎回 volumetric scattering を実行するコストが問題になる場合、`LUT[mineral_id].max_scatter_steps` で上限を設定可能。最初は分岐なしで実装し、プロファイリング後に判断する。

---

## 決定 5c: 薄膜干渉

> **Fresnel 項の修正として統合。Belcour RGB 近似。weathering_degree 駆動。**

### 設計

```
film_thickness = weathering_degree / 255.0 × LUT[mineral_id].max_film_thickness
F = belcour_thinfilm(cos_theta, ior_air, ior_film, ior_mineral, film_thickness)
```

- コードパス分岐なし。`film_thickness = 0`（`weathering_degree = 0`）で通常の Fresnel に自動退化
- RGB で完結。スペクトルレンダリング不要
- 追加フィールド不要（weathering_degree は既にコア層に存在）
- 全鉱物に一律適用。`max_film_thickness = 0` の鉱物では自動的に効果なし
- 鉄を含む鉱物（輝石、角閃石）は酸化被膜が厚くなりやすく、効果が顕著

---

## 決定 5d: Glint（結晶面の煌めき）

> **特別な glint モデルは不要。Implicit function geometry + GGX を PT が直接サンプリング。NDF filtering でノイズ制御。**

### Macro Glint（粒スケール: 1-10mm）

花崗岩の雲母の煌めき、石英の光沢面など。grain_size が 1-10mm で、ボクセル解像度 0.25mm に対して十分大きい。Implicit function が劈開面・結晶面のジオメトリを生成し、PT が直接サンプリングする。

ラスタライザ向けの近似技法（Discrete Stochastic Microfacet Model、Position-Normal Manifold、LTC）は全て不要。

### Micro Glint（サブグレインスケール）

NDF filtering で対処する（決定 5f の LOD 戦略と統合）。遠距離でファセットが sub-pixel になった場合、個々のファセットを解像する代わりに roughness を増加させて統計的に表現する。

Stochastic NDF model は初期実装では不要。grain_size が極端に小さい鉱物で必要になった場合のフォールバックとして位置付ける。

---

## 決定 5e: Implicit Function によるサブボクセルディテール

> **段階的実装: Generic FBM perturbation → 劈開を持つ鉱物にのみ mineral-specific implicit function を追加。**

### なぜ Implicit Function か（Explicit でない理由）

1. **パイプラインとの構造的一致**: SDF + sphere tracing は implicit function を native に評価する。Explicit geometry を追加するには別のアルゴリズム（ray-triangle intersection 等）が必要になり、決定 3 の「派生データを作らない」原則に反する
2. **UV/パラメタライゼーション不要**: メッシュなし・UV 展開なしの設計で、explicit な displacement map は適用不能。Implicit function は 3D 位置のみを入力とする
3. **破壊との一貫性**: 3D 空間全体で定義されているため、任意の断面にディテールが自動的に存在する。Explicit surface detail は断面で情報が消失する
4. **合成が代数的**: `f_total = f_macro + f_meso + f_micro`。Explicit では各スケールのメッシュ生成 + boolean 合成 + トポロジー管理が必要

### Step 1: Generic FBM Perturbation（全鉱物共通）

SDF perturbation + normal perturbation を FBM noise で実装。振幅・周波数を `mineral_id` LUT から制御：

```
sdf_perturbed = sdf + LUT[mineral_id].detail_amplitude × fbm(p × LUT[mineral_id].detail_frequency)
n_perturbed = normalize(n + ε × ∇fbm(p))
```

これだけでボクセルの平面感は大幅に消える。まずここで視覚品質を評価する。

### Step 2: Mineral-Specific Implicit Function（必要な鉱物のみ）

Step 1 の結果を見て、generic noise では表現不能な構造を持つ鉱物にのみ専用関数を追加：

| 鉱物 | Generic で足りるか | 専用関数が必要な理由 |
|------|:---:|---|
| 石英 | ほぼ足りる | 貝殻状断口は滑らかな noise で近似可能 |
| 長石 | 足りない | 劈開面のシャープなステップは noise では出ない。crystal_orientation に沿った不連続関数が必要 |
| 黒雲母 | 足りない | 1方向に極端に異方的な薄層剥離構造。等方的 noise では不可能 |
| 方解石 | 足りない | 菱面体劈開の3方向交差は generic に生成不能 |
| 玄武岩基質 | 足りる | 微細粒で均質。noise で十分 |

**判定基準: 劈開（cleavage）を持つ鉱物が専用 implicit function を必要とする。** 劈開は結晶学的に定義された平面に沿った不連続構造であり、連続ノイズでは原理的に再現できない。

### Render-Time Procedural Mineral Evaluation（サブボクセル鉱物解像）

格納 mineral_id（per-voxel）は構造的用途（物理・破壊）のための権威的データ。**レンダリング時の色は格納 mineral_id に縛られない。**

細粒岩（玄武岩等）では grain_size がボクセル解像度（0.25mm）以下だが、クロースアップでは個々の鉱物粒（斜長石の白い斑点等）が明確に視認できる。ボクセルを細かくするとメモリが破綻するため、render-time に Voronoi を連続座標で再評価し、サブボクセル精度の鉱物分布を取得する：

```glsl
// sphere tracing で表面到達後のシェーディング
vec3 hit_pos = ray_origin + ray_dir * t;

// 格納値（構造用: 物理・破壊）
uint8 structural_mineral = read_mineral_id(hit_pos);

// 視覚用: 同じ Voronoi を visual_grain_size で評価
VoronoiResult cell = voronoi_3d(hit_pos, rock_type.visual_grain_size, seed);
uint8 visual_mineral = assign_mineral(cell.id, rock_type.composition);

// visual_mineral でアルベド・粗さ・F0 を決定
vec3 albedo = LUT[visual_mineral].albedo;
```

**格納 mineral_id と視覚 mineral_id の役割分離:**

| 用途 | 参照するもの |
|------|:---:|
| 破壊シミュレーション（力学的性質） | 格納 mineral_id |
| SSS extinction（マクロな光輸送） | 格納 mineral_id |
| 粗い LOD でのフォールバック色 | 格納 mineral_id |
| 風化シミュレーション | 格納 mineral_id |
| ピクセルの最終的な色・反射率 | **視覚 mineral_id（render-time procedural）** |

粗粒岩（花崗岩等）では visual_grain_size ≈ grain_size であり、格納 mineral_id がそのまま視覚にも使われる（Voronoi 再評価は格納値と一致する）。RockType preset に `visual_grain_size` を追加し、格納用 `grain_size` と分離する。

PT のマルチサンプリングがサブボクセル鉱物境界を自然にアンチエイリアスする。同じ seed + 同じ Voronoi 関数が生成時と render-time で共有されるため、断面でも一貫したパターンが出る。

### Albedo / Roughness Variation

上記の render-time mineral evaluation に加え、同一鉱物内の微小変動も procedural noise で表現する：

```
albedo × (1 + 0.05~0.15 × noise3D(p × scale))
```

Per-voxel の色格納は不要。テスト後に不十分と判明した場合、per-voxel albedo 格納（+3B/voxel）を再検討する。

---

## 決定 5f: Implicit Function の LOD 戦略

> **Ray footprint 基準の連続的フェードアウト。NDF filtering で補償。**

### LOD 基準: Ray Footprint

距離や Brick LOD レベルではなく、レイの footprint（1ピクセルが表面上で覆う面積）を基準とする：

```
footprint = pixel_solid_angle × distance²
max_resolvable_freq = 1.0 / sqrt(footprint)
```

- 距離だけでは不正確（画角による差異）
- Brick LOD レベルは空間管理の都合であり知覚の基準ではない
- Ray footprint は「このレイで何が見えるか」の直接的尺度

### 連続的退化

離散レベルではなく、連続的にフェードアウトする（ポッピング防止）：

```
detail_weight = saturate(1.0 - footprint / detail_fadeout_area)
```

| Footprint | SDF Perturbation | Normal Perturbation | Roughness | Albedo Variation |
|:---------:|:---:|:---:|:---:|:---:|
| 小（近距離） | 全オクターブ + mineral-specific | procedural gradient | LUT 値 | procedural noise |
| 中 | 高周波オクターブ削減 | 維持 | NDF filtering で微増 | 維持 |
| 大（遠距離） | なし（格納 SDF のみ） | 振幅減衰 | NDF filtering で増加 | 維持（低コスト） |
| 極大 | なし | なし（格納法線のみ） | NDF filtering 最大 | なし or 最低1オクターブ |

### NDF Filtering（ディテール退化の補償）

Implicit function のディテールが落ちた分を roughness 増加で補償する。微小ファセットを個別に解像する代わりに、統計的な roughness として表現する：

```
effective_roughness = max(base_roughness, base_roughness + (1.0 - detail_weight) × ndf_boost)
```

物理的にも正しい挙動（遠距離で鉱物の煌めきが個別に見えなくなるのは現実と一致）。

### Adaptive Subdivision との関係

独立だが補完的：

- 粗い Brick + 小 footprint（カメラ急接近、未細分化）: implicit function がサブボクセルディテールを提供し、ボクセル感を隠す。細分化完了で役割交代
- 細かい Brick + 大 footprint（遠距離、未粗化）: footprint が大きいので implicit function 不要。コスト節約

### Perturbation 振幅の安全制限

粗い SDF に対する過大な perturbation は sphere tracing の破綻（表面突き抜け・偽表面生成）を招く：

```
max_amplitude = voxel_size × 0.3
sdf_offset = clamp(sdf_offset, -max_amplitude, max_amplitude)
```

### Lipschitz 定数の管理

Perturbation を加えると SDF の Lipschitz 定数が増加し、sphere tracing のステップサイズに影響する：

```
step = sdf_total(p) / L_total
L_total = L_sdf + L_perturbation
```

鉱物ごとの implicit function の最大勾配を LUT に格納し、正確なステップサイズを維持する：

```
LUT[mineral_id].detail_lipschitz
```

Over-stepping（表面突き抜け）と under-stepping（過剰に保守的なステップ）の両方を防ぐ。

---

## 他の決定への依存関係

```
決定 5 (本決定)
  |
  +---> 決定 2: mineral_id, porosity, weathering_degree, crystal_orientation が
  |     BRDF パラメータ・implicit function・薄膜干渉の全入力源
  |
  +---> 決定 3: SDF sphere tracing が implicit function の評価基盤。
  |     メッシュなし・UV なしの設計が implicit function を構造的に必然にする
  |
  +---> 決定 1b (適応的細分化): Brick 解像度と implicit function LOD は独立だが補完的。
  |     粗い Brick を implicit function が補い、細分化完了で役割交代
  |
  +---> 決定 1c (量子化): int8 SDF の有効精度範囲内で perturbation 振幅を制限
```

---

## 実装フェーズ

### Phase 1: 基本 PT + GGX

```
- Anisotropic GGX + LUT パラメタライズ
- mineral_id → albedo, roughness, F0
- crystal_orientation → anisotropy tangent
- Lambertian diffuse（volumetric SSS は Phase 2）
```

### Phase 2: Implicit Function Detail + Volumetric SSS

```
- Generic FBM perturbation（SDF + normal）
- Procedural albedo/roughness variation
- Volumetric scattering（extinction 駆動）
- Ray footprint LOD + NDF filtering
- 薄膜干渉（Belcour RGB）
- 視覚品質評価: ボクセル感の解消度、glint の自然さ
```

### Phase 3: Mineral-Specific Implicit Functions

```
- 劈開鉱物の専用 implicit function（長石、雲母、方解石）
- Step 1 の FBM と置き換え or 重畳
- Lipschitz 定数の LUT 格納
- 視覚品質評価: generic FBM との A/B 比較
```
