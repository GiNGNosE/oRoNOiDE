# 決定 6: 生成パイプライン設計

**関連レポート**: `engine/docs/outputs/hyperreal_stone_technology_report.md` Section 2.a, 2.b, 2.c

---

## 設計原則

> **位置ベースの決定論的関数で全フィールドを生成。解像度を意識しない。**

生成パイプラインの全ステージは `f(position, seed, params) → value` の形式。同じ seed + 同じ位置から常に同じ結果が得られる。適応的細分化時は同じパイプラインを細かい解像度で再実行するだけで、特別な細分化ロジックは不要。

---

## 決定 6a: パイプライン全体アーキテクチャ

### 入力

```
必須:
  rock_type:            enum         花崗岩、玄武岩、砂岩、大理石、片麻岩 ...
  size:                 float3       概略サイズ（bounding box）
  world_position:       float3       ワールド座標（seed の一部）
  seed:                 uint64       決定論的再現用

形状制御:
  shape_params:         ShapeParams  Voronoi fracture パラメータ（決定 6d）
  weathering_shape:     float        風化による形状変化（0=角張り、1=丸い）

岩石学パラメータ（RockType preset から展開、オーバーライド可能）:
  mineral_composition:  [(mineral_id, probability)]
  grain_size:           float (mm)   格納用 Voronoi セルサイズ
  visual_grain_size:    float (mm)   render-time 評価用（決定 5e 拡張）
  grain_size_modifier:  float        per-Entity 粒径スケール
  layering_orientation: float3       堆積/葉理方向
  weathering_age:       float        風化進行度（0=新鮮、1=完全風化）
  fold_params:          FoldParams   褶曲パラメータ（変成岩）
```

### 出力

```
SBP bricks:
  全アクティブボクセルに 9B コア層が充填済み
  ブリックヘッダーに sdf_min/max 計算済み

Per-Entity metadata:
  topology_version, generation_transform, grain_size_modifier,
  layering_orientation, wetness, temperature 設定済み
  mass, center_of_mass, inertia_tensor 計算済み（導出キャッシュ）
```

### ステージ構成（依存関係順）

```
Stage 1: Shape (SDF)                     → 決定 6d
  入力: shape_params, weathering_shape, seed
  出力: SBP にブリック確保 + SDF 値書き込み
  備考: どのボクセルがアクティブかがここで決まる

Stage 2: Mineral Distribution (mineral_id) → 決定 6b, 6c
  入力: rock_type, mineral_composition, layering_orientation, fold_params, seed
  出力: 全アクティブボクセルに mineral_id

Stage 3: Crystal Orientation
  入力: mineral_id, Stage 2 の Voronoi cell データ, seed
  出力: crystal_orientation per voxel
  手法: Voronoi cell ごとにランダム方位を割り当て。
        LUT[mineral_id].crystal_system で方位の自由度を制約

Stage 4: Porosity
  入力: mineral_id, rock_type params, seed
  出力: porosity per voxel
  手法: LUT[mineral_id].base_porosity + spatial noise
        粒界近傍でやや高めに設定可能

Stage 5: Weathering
  入力: SDF（表面距離）, weathering_age
  出力: weathering_degree per voxel
  手法: 表面からの距離に反比例 + mineral_id 依存の風化速度
        内部 = 低、表面 = weathering_age に比例

Stage 6: Normal
  入力: SDF
  出力: normal per voxel（snorm8x2）
  備考: SDF 勾配から計算。全フィールド書き込み後に実行

Stage 7: Derived Metadata
  入力: 全コアフィールド
  出力: per-Entity の mass, center_of_mass, inertia_tensor
        ブリックヘッダーの sdf_min/max
  備考: microcrack_density = 0（新規生成時は損傷なし）
```

### CPU / GPU

Phase 1 は CPU で実装。各ステージは per-voxel 独立処理であり、GPU compute shader への移行パスは自明。移行はプロファイリングで生成がボトルネックと判明した場合。

### 実行タイミング

| トリガー | 処理 |
|---------|------|
| ワールド生成 | 全石をバッチ生成（非同期で順次） |
| Entity spawn | 1石を生成 |
| 破壊 | 生成ではない。SDF 連結成分分析 + Entity 分割。内部ボクセルの weathering_degree は生成時に「低」で格納済み |
| 適応的細分化 | 同一 seed で同じパイプラインを細かい解像度で再実行（決定 6e） |

### Seed 管理

```
stone_seed = hash(world_seed, entity_spawn_position) or explicitly assigned uint64
stage_seed = hash(stone_seed, stage_index)  // ステージ間の相関を防ぐ
```

決定論的再現が必須な理由:
1. 適応的細分化で粗い↔細かいの一貫性を保証
2. セーブ/ロード時に seed + params から再生成可能（最適化オプション）
3. 生成時と render-time の procedural 関数が同じ seed を共有

### プロシージャル関数共有

生成パイプラインと描画パイプライン（決定 5e）が同じ関数ライブラリを共有する:

```
共有関数: voronoi_3d, fbm, gabor_noise, fold_field ...

生成時: voronoi_3d(pos, seed) → mineral_id を SBP に格納
描画時: voronoi_3d(pos, seed) → 粒界距離を取得し、ディテール変調に使用
         + visual_grain_size での render-time mineral evaluation
```

---

## 決定 6b: 鉱物分布 — プリミティブと合成

> **Voronoi と Layered の2プリミティブの合成で全岩石カテゴリを表現。**

### プリミティブ

**Voronoi(grain_size, composition, seed):**
- 3D Voronoi セル = 結晶粒。セルごとに mineral_id を確率的割り当て
- 火成岩の粒状組織、非葉理型変成岩の再結晶構造

**Layered(orientation, fold_params, layer_sequence, seed):**
- 変形場（fold_field）適用後の堆積軸に沿った層構造
- 堆積岩の地層、葉理型変成岩のバンド構造

### 合成

| 岩石カテゴリ | 戦略 | 合成方法 |
|:---:|---|---|
| 火成岩（花崗岩、玄武岩） | VoronoiOnly | Voronoi のみ |
| 堆積岩（砂岩、石灰岩） | LayeredOnly | Layered（+ オプション層内 Voronoi） |
| 葉理型変成岩（片麻岩、片岩） | LayeredVoronoi | Layered がVoronoi の composition を空間的に変調 |
| 非葉理型変成岩（大理石、珪岩） | VoronoiOnly | Voronoi のみ |

### 細粒岩の扱い（grain_size < voxel_size）

grain_size がボクセル解像度以下の場合（玄武岩等）:
- 格納 mineral_id: resolvable な粒（> 0.25mm）のみ Voronoi で配置。それ以下は dominant mineral
- 視覚的表現: render-time に visual_grain_size で Voronoi を再評価（決定 5e: Render-Time Procedural Mineral Evaluation）

### 斑状組織

二重スケール Voronoi:

```
1. 粗い Voronoi（phenocryst.size）で斑晶領域を配置
   各セルが phenocryst.fraction の確率で斑晶になる
2. 残り領域に細かい Voronoi（grain_size）で基質の粒構造を生成
```

---

## 決定 6c: 変形場（Fold Field）による複雑な層構造

> **Layered プリミティブの前段に変形場を適用し、褶曲・層厚変動・差別侵食を表現。**

### 変形場

```
// 単純な線形レイヤリング（不十分）:
depth = dot(pos, orientation)

// 変形場を適用したレイヤリング:
deformed_pos = pos + fold_field(pos, fold_params, seed)
depth = dot(deformed_pos, orientation)
```

fold_field は複数スケールの褶曲を重畳:

```
fold_field(pos, params, seed):
  displacement = vec3(0)
  // 大スケール褶曲: 層全体がうねる
  displacement += major_amplitude × sin(dot(pos, fold_axis) × major_frequency) × orientation
  // 中スケール褶曲: 不規則な波打ち
  displacement += minor_amplitude × fbm(pos × minor_frequency, seed)
  // 小スケール擾乱: 層境界の微小な凹凸
  displacement += perturbation × noise(pos × perturbation_frequency, seed)
  return displacement
```

### パラメータ

```
FoldParams:
  major_amplitude:          float    大褶曲の振幅
  major_frequency:          float    大褶曲の周波数
  fold_axis:                float3   褶曲軸方向
  minor_amplitude:          float    中スケール褶曲の振幅
  minor_frequency:          float    中スケール褶曲の周波数
  perturbation:             float    小スケール擾乱の振幅
  perturbation_frequency:   float    小スケール擾乱の周波数
  thickness_variance:       float    層厚の空間変動
  thickness_frequency:      float    層厚変動の周波数
```

### LayeredVoronoi の合成方法（片麻岩等）

```
1. 変形場適用後の depth から band_type を決定
2. band_type が Voronoi の composition を変調
   FELSIC_BAND → feldspar+quartz を boost
   MAFIC_BAND  → biotite+garnet を boost
3. 変調された composition で Voronoi 割り当て
```

層構造が Voronoi の確率分布を空間的に変調する。Voronoi 自体は等方的なまま。

### 差別侵食

層ごとの硬さの違いで SDF を変調（形状生成 Stage 1 と連携）:

```
erosion_resistance = LUT[layer_mineral].hardness
sdf_eroded = sdf_base + (1.0 - erosion_resistance) × erosion_depth × noise(pos)
```

---

## 決定 6d: 形状生成（SDF）

> **Voronoi Fracture Fragment をベース + 曲率選択的風化 + 構造的差別侵食。**

### なぜ Voronoi Fracture か

自然界の石の形は「破壊 + 風化」の結果であり、ランダムノイズの結果ではない。Noise-deformed primitive（球に FBM を加える等）は有機的で「溶けた」形状になり、石に見えない。

Voronoi fracture は:
- 物理的に正しい（破壊力学の準静的近似）
- 自然に平面的ファセットと鋭い稜線が生まれる
- パラメータ変更で無限のバリエーション
- 岩石構造による異方性を表現可能

### 形状生成パイプライン

**Stage 1a: Base Shape（Voronoi Fracture Fragment）**

```
1. バウンディングボリューム内に Voronoi 分割を生成
2. 1つのセル（or 複数セルの smooth union）を選択
3. SDF に変換
```

岩石構造による異方性:
- 火成岩: 等方的 Voronoi → 不規則な塊
- 堆積岩: layering_orientation 方向に Voronoi ポイントを圧縮 → 板状・扁平
- 変成岩: foliation_orientation 方向に圧縮 → 薄板（片岩）〜中程度（片麻岩）

**Stage 1b: Weathering Deformation**

曲率選択的摩耗（spheroidal weathering）:

```
curvature = laplacian(sdf)
sdf_weathered = sdf + weathering_shape × smoothing × max(0, curvature - threshold)
```

角・稜線（高曲率）のみ丸め、平面はそのまま維持。均一な smoothing より遥かに自然。

weathering_shape による連続スペクトル:

| 値 | 形状 | 実例 |
|:---:|---|---|
| 0.0 | 鋭い稜線、平面ファセット | 採石場の砕石、新鮮な落石 |
| 0.3 | 稜線がやや丸い | 山岳の転石 |
| 0.6 | 全体的に丸みを帯びる | 河原の石 |
| 0.9 | ほぼ楕円体 | 海岸の玉石 |

**差別侵食（堆積岩・変成岩）:**

```
layer_mineral = layer_sequence[layer_index(deformed_depth)]
erosion_resistance = LUT[layer_mineral].hardness
sdf_eroded = sdf_base + (1.0 - erosion_resistance) × erosion_depth × noise(pos)
```

### ShapeParams

```
ShapeParams:
  voronoi_cell_count:   int        破片の複雑さ
  voronoi_jitter:       float      ポイントのランダム性
  anisotropy_axis:      float3     異方性方向（= layering/foliation orientation）
  anisotropy_ratio:     float      異方性強度（1.0 = 等方的）
  multi_cell_count:     int        結合するセル数（1 = 単一破片）
  multi_cell_blend:     float      結合の滑らかさ（smooth union radius）
```

---

## 決定 6e: 適応的細分化との統合

> **同じ seed + 同じパイプラインを細かい解像度で再実行。特別な細分化ロジックは不要。**

### 設計

全ステージが `f(position, seed, params)` の位置ベース決定論的関数であるため、解像度を変えてもパイプラインのコードは同一。空間管理システムが解像度を指定し、パイプラインはそのまま実行する。

### 粗い↔細かいの整合性

粗いボクセルと細分化後の子ボクセル群は**厳密には一致しない可能性がある**（粒界を跨ぐ等）。これは問題ない:
- 粗いブリックは遠距離用。多少の不正確は知覚不能
- 細かいデータが存在する間は粗いデータは参照されない
- 粗化時は決定 2 の LOD 集約方法（多数決等）で再集約
- SDF は連続関数のため表面位置は解像度によらず一貫

### 暫定表示

細分化完了まで: 粗いブリック + implicit function detail（決定 5e）で表示。Implicit function が粗いブリックのボクセル感を隠す。

### 生成コスト管理

細分化リクエストは非同期キューで処理。粗い表示で暫定フィードバックを提供しつつ、バックグラウンドで高解像度データを生成。

---

## RockType Preset 例

```
GRANITE:
  strategy:           VoronoiOnly
  composition:        [(QUARTZ, 0.30), (K_FELDSPAR, 0.35), (PLAGIOCLASE, 0.15),
                       (BIOTITE, 0.15), (MUSCOVITE, 0.05)]
  grain_size:         3.0mm
  visual_grain_size:  3.0mm          // 格納 ≈ 視覚（粗粒岩）
  porosity_base:      0.01
  weathering_rate:    0.3
  shape: { cell_count: 8, jitter: 0.8, anisotropy_ratio: 1.0 }
  phenocryst:         None

BASALT:
  strategy:           VoronoiOnly
  composition:        [(PYROXENE, 0.40), (PLAGIOCLASE, 0.45), (OLIVINE, 0.10),
                       (MAGNETITE, 0.05)]
  grain_size:         0.3mm          // 格納用: resolvable な粒のみ
  visual_grain_size:  0.08mm         // render-time: サブボクセル鉱物評価
  porosity_base:      0.05
  weathering_rate:    0.2
  shape: { cell_count: 12, jitter: 0.6, anisotropy_ratio: 1.0 }

SANDSTONE:
  strategy:           LayeredOnly
  layer_sequence:     [(QUARTZ, 5.0mm, 2.0), (FELDSPAR_MIX, 2.0mm, 1.0),
                       (CLAY, 1.0mm, 0.5)]
  layer_noise_amplitude: 0.3
  grain_size:         0.5mm
  visual_grain_size:  0.5mm
  porosity_base:      0.15
  weathering_rate:    0.6
  shape: { cell_count: 6, jitter: 0.5, anisotropy_ratio: 0.3, anisotropy_axis: layering }
  fold_params:        { major_amplitude: 0, ... }  // 堆積岩: 褶曲なし

GNEISS:
  strategy:           LayeredVoronoi
  composition:        [(FELDSPAR, 0.40), (QUARTZ, 0.30), (BIOTITE, 0.20),
                       (GARNET, 0.10)]
  layer_sequence:     [(FELSIC_BAND, 4.0mm, 2.0), (MAFIC_BAND, 2.0mm, 1.0)]
  grain_size:         2.0mm
  visual_grain_size:  2.0mm
  porosity_base:      0.01
  weathering_rate:    0.25
  shape: { cell_count: 10, jitter: 0.7, anisotropy_ratio: 0.5, anisotropy_axis: foliation }
  fold_params:        { major_amplitude: 5.0, major_frequency: 0.3,
                        minor_amplitude: 1.0, minor_frequency: 1.5,
                        perturbation: 0.2, perturbation_frequency: 5.0 }

MARBLE:
  strategy:           VoronoiOnly
  composition:        [(CALCITE, 0.90), (DOLOMITE, 0.05), (QUARTZ, 0.03),
                       (GRAPHITE, 0.02)]
  grain_size:         1.0mm
  visual_grain_size:  1.0mm
  porosity_base:      0.005
  weathering_rate:    0.5
  shape: { cell_count: 8, jitter: 0.9, anisotropy_ratio: 1.0 }

PORPHYRITIC_GRANITE:
  strategy:           VoronoiOnly
  composition:        [(QUARTZ, 0.25), (PLAGIOCLASE, 0.20), (BIOTITE, 0.15),
                       (FINE_MATRIX, 0.40)]
  grain_size:         0.5mm
  visual_grain_size:  0.5mm
  porosity_base:      0.01
  shape: { cell_count: 8, jitter: 0.8, anisotropy_ratio: 1.0 }
  phenocryst:         { mineral: K_FELDSPAR, size: 15.0mm, fraction: 0.20 }
```

---

## 派生フィールドの生成

### crystal_orientation (Stage 3)

```
cell = voronoi_3d(voxel_pos, grain_size, seed)
orientation = random_unit_vector(hash(cell.id, ORIENTATION_SALT))
// 同一 Voronoi セル内の全ボクセルが同一方位を共有
// LUT[mineral_id].crystal_system で方位の自由度を制約
```

### porosity (Stage 4)

```
porosity = LUT[mineral_id].base_porosity
         + rock_type.porosity_variance × noise(voxel_pos, POROSITY_SALT)
// 粒界近傍でやや高く設定可能
porosity += grain_boundary_boost × (1.0 - smoothstep(0, boundary_width, edge_distance))
```

### weathering_degree (Stage 5)

```
surface_distance = abs(sdf_value) × sdf_scale
weathering_penetration = LUT[mineral_id].weathering_rate × weathering_age
weathering_degree = saturate(1.0 - surface_distance / weathering_penetration)
// 表面 = 高風化度、内部 = 低風化度
```

### microcrack_density (Stage 7)

```
microcrack_density = 0  // 新規生成は損傷なし
```

---

## 他の決定への依存関係

```
決定 6 (本決定)
  |
  +---> 決定 1a (SBP): ブリック確保・ボクセル書き込みの対象
  |
  +---> 決定 1b (適応的細分化): 同じパイプラインを異なる解像度で再実行
  |
  +---> 決定 2 (マテリアルフィールド): 生成出力 = コア層 9B/voxel
  |
  +---> 決定 5e (Implicit Function): 共有プロシージャル関数ライブラリ。
  |     Render-Time Procedural Mineral Evaluation で visual_grain_size を使用
  |
  +---> 決定 6d → 決定 6c: 形状 SDF に差別侵食を適用する際に層構造を参照
```

---

## 実装フェーズ

### Phase 1: 単一鉱物の石

```
- Voronoi fracture → SDF 生成
- 単一 mineral_id（全ボクセル同一）
- weathering_shape による形状バリエーション
- 法線計算
```

### Phase 2: 多鉱物 + 層構造

```
- Voronoi grain による mineral_id 分布
- Layered プリミティブ
- crystal_orientation, porosity, weathering_degree 生成
- Render-time procedural mineral evaluation
```

### Phase 3: 複雑な岩石タイプ

```
- LayeredVoronoi 合成（片麻岩）
- 変形場（fold_field）による褶曲
- 差別侵食
- 斑状組織（二重スケール Voronoi）
- 適応的細分化との統合検証
```
