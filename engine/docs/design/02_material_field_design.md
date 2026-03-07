# 決定 2: Per-Voxel マテリアルフィールド設計

**議論ファイル**: `engine/docs/discussion/02_material_field_discussion.md`

---

## 設計原則

### 不可逆/可逆分離

- **コア層** = 不可逆的または内在的な状態。「このボクセルが何であるか」「何が永続的に起きたか」を定義する。常に存在し、常に永続化される。レンダリング品質はコア層のみで保証される。
- **拡張層** = 可逆的、一時的、または導出可能な状態。シミュレーション完了後に解放しても情報損失がない。拡張層が不在でもレンダリング品質は劣化しない。

### 妥協戦略

- 遠方の石（シミュレーションなし）: コアのみで完璧にリアルに見える
- 衝撃を受けた石: コア + 拡張層でフル精度シミュレーション
- 衝撃完了後: 結果をコアにコミット → 拡張層を解放 → 再びコアのみ

---

## コア層: 9 bytes/voxel

全アクティブボクセルに常時存在。レンダリングと永続化の基盤。

```
SDF:                 int8 + header     (1B)   形状の権威的定義
法線:                snorm8x2          (2B)   八面体マッピング。SDF int8量子化で勾配精度不足のため必須
mineral_id:          uint8             (1B)   鉱物同一性。全サブシステムの起点
porosity:            uint8             (1B)   空隙率。空間変動がありLUT不可
weathering_degree:   uint8             (1B)   風化進行度。静的（生成時に設定、ランタイム風化なし）
microcrack_density:  uint8             (1B)   機械的損傷蓄積。構造専用、レンダリング非参照
crystal_orientation: snorm8x2          (2B)   結晶軸方向。八面体マッピング
────────────────────────────────────────────
合計: 9 bytes/voxel
```

### メモリ見積もり

| シナリオ | ボクセル数 | VRAM |
|---------|:---------:|:----:|
| 平均的な風景（石 ~300個 + 地形） | 28M | 252 MB |
| 大規模シーン（石 1000個 + 広域地形） | 200M | 1.8 GB |

---

## 各フィールドの決定根拠

### crystal_orientation: per-voxel格納（プロシージャル導出を却下）

Voronoiベースのプロシージャル導出案（0B/voxel）を検討したが、格納（2B）を採用。

**却下理由:**
1. 破壊時のgeneration_transform継承が破綻すると、破断面の両側でグリントパターンが不連続になる。Phase 3まで検証不能なリスクを受け入れられない
2. Voronoiは等軸晶（花崗岩の石英等）に適するが、雲母の葉状組織、角閃石の繊維状晶では不自然なパターンになる。鉱物種ごとの分岐でプロシージャルの単純性が失われる
3. PTで毎フレーム数百万レイに対して27 hash + distance計算を実行するALUコストを、56MBの節約のために払い続けるのは不合理
4. 格納値はデバッグ時に直接読める。破壊時の一貫性はボクセルデータのコピーで自動保証

**生成パイプライン:** 生成時にVoronoi（または鉱物種に適した関数）で結晶方位を計算し、結果をコア層にキャッシュする。プロシージャル関数は生成時の技術であり、ランタイムのフィールド代替ではない。

### volume_fraction: 排除

per-voxelの鉱物混合比（3-4B/voxel）を格納しない。

**排除理由:**
1. 最細解像度（0.25mm）では各ボクセルは物理的に単一鉱物。花崗岩の粒径1-10mmに対し、粒界は原子スケールで急峻。混合比は物理的に不正確
2. PTのサブピクセルサンプリングが離散的mineral_id境界を自然にアンチエイリアスする
3. シェーダの全マテリアルルックアップが2鉱物ブレンドになる複雑化のコストが、大半のボクセル（単一鉱物）で無駄になる。ROIが極めて低い
4. 粗いLODでの鉱物混合はブリックヘッダーメタデータで対応（後述）

### grain_size: LUT導出（per-voxel格納を却下）

mineral_id LUT + per-Entityのgrain_size_modifierで導出。

- 同一岩石内では鉱物種ごとの粒径は概ね一定
- 岩石間の粗粒/細粒の違いはper-Entity modifierで表現
- 空間変動がプレイヤーの知覚を支配するシナリオは考えにくい

### microcrack_density: コア層に永続化（構造シミュレーション専用）

**拡張層への移動を検討し、却下。** 拡張層に置くと解放時に損傷履歴が消失し、「コアから再導出可能」という拡張層の不変条件を破る。microcrack_densityの導出元は過去の衝撃履歴であり、他のフィールドから再計算できない。

**コア層に残す根拠:**
- 空間的な損傷分布の永続化が不可欠。「特定の箇所を集中的に叩いて弱くする」戦略的ゲームプレイを支える
- microcrack_densityはGc（破壊靭性）の低下係数であり、弱い道の空間分布が永続化されていないとクラック伝播の経路選択が毎回リセットされる
- 1B/voxel（28M voxelsで28MB）のコストは設計判断を左右する数字ではない

**microcrack_density と phase_field_d の区別:**

| | microcrack_density (コア) | phase_field_d (拡張) |
|---|---|---|
| 物理的意味 | 微小亀裂の蓄積密度 | マクロクラックの連続場 |
| 値域 | 0=健全, 255=飽和（極端に弱いが存在する） | 0=健全, 1=完全破壊（ボクセル消失） |
| 永続性 | 永続（コア層） | 一時的（ソルバー作業変数） |
| 効果 | Gcを下げる（弱化） | 剛性を(1-d)^2に低下させ、d=1でSDF変更 |

microcrack_density=255でもボクセルは存在し続ける。外力なしでは破壊は起きない（Griffithの基準: G >= Gcが必要）。phase_field_d=1になって初めてSDFが更新されボクセルが消失する。

**レンダリング参照:** Phase 1-2では非参照。ただし大理石・石英岩等でストレスホワイトニング（微小亀裂による拡散散乱で白っぽくなる現象）を表現する場合、将来的にシェーダで1行追加する余地を残す:
```
roughness += microcrack_density * LUT[mineral_id].crack_scatter_factor;
```
フィールド追加は不要。crack_scatter_factorはLUTに持たせ、花崗岩等の不透明岩石では0に設定。

### weathering_degree: 静的フィールド（ランタイム風化なし）

**風化シミュレーションを省略する。** 風化は年数単位の現象であり、ゲーム内でリアルタイムに更新するROIが極めて低い（コストはO(表面ボクセル数)で常時、知覚可能な変化はゲーム内で数時間〜数日）。

- 生成時に石の「歴史」として設定（新鮮な破断面=低、古い露頭=高）
- プレイヤーアクションによる局所変更は許可（研磨→weathering_degree低下→光沢面出現）
- 破壊時に新鮮な内部面が露出（低weathering_degree）

### 風化とmineral_idの遷移

風化で色が大きく変わる問題は、別途色フィールドを持つのではなく、**mineral_idの遷移**で表現する。

```
LUT[mineral_id] に含まれるフィールド:
  weathering_product_id: uint8   -- 風化後の鉱物ID
  weathering_rate: float         -- 風化速度係数（生成時のパラメータ）

色の導出:
  base_color     = LUT[mineral_id].albedo
  weathered_color = LUT[weathering_product_id].albedo
  final_color    = lerp(base_color, weathered_color, weathering_degree / 255.0)
```

- 長石→カオリナイト（白/灰色）、鉄鉱物→褐鉄鉱（赤褐色）等、鉱物学的に正しい遷移
- 色は常にmineral_id LUTから導出。色格納フィールド不要
- 風化連鎖: weathering_product_idがさらに別の生成物を指すことで多段階風化を表現可能
- mineral_id空間（uint8 = 256種）を一次鉱物（0-127）と風化生成物（128-223）で分割

---

## レンダリングシェーダのアクセスパターン

```
レンダリングが読むフィールド: 6フィールド / 8B
  SDF (1B)                 -- レイマーチング
  法線 (2B)                -- シェーディング方向
  mineral_id (1B)          -- LUT: アルベド, F0, SSS, 粗さベース値
  porosity (1B)            -- SSS深度, 透過率
  weathering_degree (1B)   -- 色変化, 薄膜干渉, 粗さ修正
  crystal_orientation (2B) -- 異方性グリント, 劈開面反射

レンダリングが読まないフィールド:
  microcrack_density       -- 破壊シミュレーション専用

導出されるレンダリングパラメータ（格納なし）:
  アルベド       = lerp(LUT[mineral_id].albedo, LUT[weathering_product_id].albedo, weathering_degree/255)
  表面粗さ       = f(LUT[mineral_id].base_roughness, weathering_degree)
  スペキュラF0   = LUT[mineral_id].F0
  SSSパラメータ  = g(LUT[mineral_id].extinction, porosity)
  密度           = LUT[mineral_id].density * (1 - porosity/255)
  粒径           = LUT[mineral_id].grain_size * entity.grain_size_modifier
```

---

## 拡張層: 20 bytes/voxel（シミュレーション対象領域のみ）

破壊シミュレーション時にのみ確保し、完了後に解放。

```
phase_field_d:       float16           (2B)   ソルバー作業変数
E:                   float16           (2B)   弾性率（コアから導出して初期化）
nu:                  float16           (2B)   ポアソン比
Gc:                  float16           (2B)   破壊靭性
sigma_t:             float16           (2B)   引張強度
anisotropy_tensor:   float16x3         (6B)   破壊異方性（crystal_orientation + mineral_idから導出）
moisture:            float16           (2B)   水分量（可逆、シミュレーション時のみ）
────────────────────────────────────────────
合計: 20 bytes/voxel（対象領域のみ）
```

### 力学パラメータの導出

拡張層の展開時にコアフィールドからLUT経由で初期化:

```
E       = LUT[mineral_id].E       * g(porosity) * h(microcrack_density)
nu      = LUT[mineral_id].nu      * g(porosity)
Gc      = LUT[mineral_id].Gc      * g(porosity) * h(microcrack_density)
sigma_t = LUT[mineral_id].sigma_t * g(porosity) * h(microcrack_density)
```

microcrack_densityがコア層にあることが鍵: 過去の損傷蓄積が力学パラメータの劣化に反映される。

### ライフサイクル

```
展開:   衝突検出/破壊シミュレーション開始時
        コアフィールド + LUTから力学パラメータを導出して初期化
実行:   ソルバーがphase_field_dを伝播、力学パラメータを更新
コミット: シミュレーション完了時
        0 < d < 1 の領域 → microcrack_density増加（コア。損傷蓄積したが割れなかった）
        d = 1 の領域 → SDFを更新（コア。ボクセルが割れた）
        SDF連結成分分析 → 分離していたら新Entity生成（破片）
解放:   コミット後に拡張層を解放。VRAM回復
        コア層だけで完璧なレンダリングが継続
```

### 破壊ソルバー: 陽的動力学（Explicit Dynamics）

準静的フェーズフィールドソルバー（毎ステップでグローバル線形システムを解く）はリアルタイムには重すぎる。代わりに**陽的動力学 + 質量スケーリング**を採用。

```
各ボクセル（GPU完全並列）:
  acceleration = divergence(stress) / density
  velocity += acceleration * dt
  displacement += velocity * dt
  strain = gradient(displacement)
  stress = (1-d)^2 * C * strain

  if strain_energy > threshold(Gc):
    d += delta_d
```

**グローバル線形システムの求解が不要。** 各ステップが完全にローカルな並列計算。非局所的な応力伝播は弾性波として自然に伝わる。

**質量スケーリング:** 密度を人工的に増加させてCFL条件のタイムステップ制約を緩和。クラック経路はエネルギー最小化で決まるため、波速の変更は最終的な破壊パターンに影響しない。

```
性能見積もり（100x質量スケーリング）:
  小さな石 (~100K voxels): 1-5ms → 同フレーム完了
  大きな岩 (~1M voxels):   10-50ms → 数フレーム
```

**破壊の非局所性:** 損傷発展（d更新）は各ボクセルでローカルだが、応力場は弾性波として伝播する。d=1のボクセルは剛性が(1-d)^2≈0に低下し応力を伝達しなくなるため、周囲に応力が再分配されクラック先端に集中する。この応力集中→隣接ボクセル破壊→さらなる応力集中のカスケードがクラック伝播を駆動する。

### 衝撃エフェクト

衝撃時に物理的に発生する現象を常に描画する。計算時間を隠すためのギミックではなく、物理的事実の再現。

```
衝撃検出（同フレーム、常時発生）:
  1. 粉塵パーティクル: 接触面の微粒子飛散。衝撃強度に比例
  2. 衝撃音: mineral_id + porosity → 音響特性LUT。密な石は高音、多孔質は鈍音
  3. 接触面SDF微小変更: 衝撃痕（微小な凹み）

ソルバー完了時:
  4. クラック出現（SDF更新）
  5. 破片分離（Entity分割 + 運動量分配）
```

大きな岩で計算が数フレームかかる場合でも、1-3は同フレームで発生し即座のフィードバックを提供する。現実の石の衝撃でもクラック伝播前に粉塵と音が発生するため、この遅延は物理的に正しい時間スケールに一致する。

Voronoi事前分割等のフォールバックは採用しない。陽的動力学で全ケースを解く。

---

## per-Entityメタデータ

```
[永続（セーブ対象）]
topology_version:      uint32           (4B)   SDF変更時にインクリメント
temperature:           float16          (2B)   石全体の基本温度（環境温度に収束）
layering_orientation:  float16x3        (6B)   堆積層方向（生成時パラメータ）
grain_size_modifier:   float16          (2B)   LUT粒径への乗算係数
generation_transform:  float32x3x4      (48B)  生成時のワールド変換（破壊時に子に継承）
wetness:               uint8            (1B)   濡れ度（視覚エフェクト用、per-Entity十分）

[導出キャッシュ（topology_version変更時に再計算、セーブ不要）]
mass:                  float32          (4B)   Σ(density(mineral_id, porosity) * voxel_volume)
center_of_mass:        float32x3        (12B)  密度加重位置平均
inertia_tensor:        float32x6        (24B)  対称行列の上三角6成分。重心周りの密度加重二次モーメント
────────────────────────────────────────────
合計: 103 bytes/Entity（うち40Bは導出キャッシュ）
```

### 剛体物理パラメータ

質量・重心・慣性テンソルは剛体物理（投げる、落ちる、積む、跳ねる、回転）に不可欠。全てコアフィールドから導出可能:

```
mass           = Σ LUT[mineral_id[v]].density * (1 - porosity[v]/255) * voxel_volume
center_of_mass = Σ position[v] * density[v] / mass
inertia_tensor = Σ density[v] * ((r・r)I - r⊗r) * voxel_volume   (r = position[v] - center_of_mass)
```

全ボクセル走査が必要なため毎フレーム計算は非合理。topology_versionが変更された時（破壊、彫刻等でSDF変更）のみ再計算し、per-Entityにキャッシュする。ゲームプレイの大部分の時間はtopology_versionが不変のため、キャッシュ値がそのまま使える。

セーブ/ロード時に永続化不要。ロード後にボクセルデータから再計算して復元。

### layering_orientation

堆積層の方向は石のスケール（10-50cm）で一様。褶曲の曲率半径（数m〜km）に対してper-voxelは過剰。

**レンダリングでは参照しない。** 縞模様は生成時にmineral_idの空間分布として焼き込む:

```
生成時:
  depth = dot(voxel_pos, layering_orientation)
  layer_index = floor(depth / layer_spacing)
  voxel.mineral_id = layer_sequence[layer_index % N]
```

用途は生成パイプライン、破壊ソルバー（層に沿った割れやすさ）、風化エージェント（将来的に有効化する場合）に限定。

### 温度モデル

風化シミュレーション省略により簡素化:

| 用途 | 対応 |
|------|------|
| 黒体輻射（加熱時の発光） | per-Entity temperatureからシェーダで計算 |
| 冷却/加熱速度の鉱物差 | per-Entity temperature + LUT比熱から計算 |
| 熱衝撃（急冷→クラック） | 必要時のみper-voxel temperature拡張層を確保（オプション） |
| 凍結融解→風化 | 省略（風化シミュレーション不使用） |

### 水の表現

per-Entityのwetnessフラグで濡れた石の見た目を制御:

- 濡れた表面: 実効IOR変更（air→water film→stone）、粗さ低下、アルベド暗化
- 吸収 vs 撥水: porosity + weathering_degreeから接触角を導出
- 水中描画: PTパイプラインのparticipating media機能（マテリアルフィールド追加不要）

---

## GPUメモリレイアウト: ブリックレベルSoA

ブリック（8^3 = 512 voxels）内でフィールド単位にSoA配置。

```
Brick (4608 bytes = 4.5KB):
  offset 0:    SDF[512]              512B   -- レイマーチングで最頻アクセス
  offset 512:  normal[512x2]        1024B
  offset 1536: mineral_id[512]       512B
  offset 2048: porosity[512]         512B
  offset 2560: weathering_degree[512] 512B
  offset 3072: microcrack_density[512] 512B
  offset 3584: crystal_orientation[512x2] 1024B
  + ブリックヘッダー (scale, offset, LODメタデータ)
```

### 選定理由

1ブリック4.5KBはGPU L1キャッシュ（48-128KB）に丸ごと載る。一度キャッシュに入ればブリック内の全フィールドがキャッシュヒットする。

| アクセスパターン | 純粋SoA | 純粋AoS | ブリックレベルSoA |
|--------------|:---:|:---:|:---:|
| SDF走査（レイマーチング） | 空間局所性崩壊 | 8/9がゴミ | SDF[512]が連続、最適 |
| シェーディング（多フィールド） | 6回キャッシュミス | 1回フェッチ | L1ヒット（ブリック全体がキャッシュ内） |
| 部分フィールドスキャン（風化等） | 最適 | 非効率 | ブリック内で連続、十分効率的 |
| フィールド追加/削除 | 最容易 | 全データ再配置 | オフセット更新 + ブリック再配置 |
| SBPとの親和性 | 低 | 低 | ブリック=アロケーション単位と一致 |

SDFを先頭に配置: レイマーチングが最も高頻度でSDFのみをアクセスするため。

---

## LOD集約方法

粗いLODのブリックは細かいブリックの「表示用近似」。元の細かいブリックはRAM/ディスクに退避し、カメラ接近時にVRAMに復帰する。集約はレンダリング品質のみで判断。

| フィールド | 集約方法 | 理由 |
|-----------|---------|------|
| SDF | trilinear再サンプリング | 距離場の標準手法 |
| 法線 | 粗いSDFから再計算 | 細かい法線の平均は幾何学的に不正確 |
| mineral_id | 多数決 + ブリックヘッダー補完 | 離散値は平均化不能。ヘッダーで2鉱物混合情報を保持 |
| porosity | 体積平均 | 内部/外部均等に寄与する連続値 |
| weathering_degree | SDF距離重み付き平均 | 表面現象のため、内部ボクセルで薄まらないよう表面近傍を優先 |
| microcrack_density | 最大値 | 構造的保守性。粗いLODでクラックが消えないように |
| crystal_orientation | 中心ボクセルの代表値 | 粗いLODでは個々のグレインは不可視。方位の平均は物理的に無意味 |

### ブリックヘッダーLODメタデータ

volume_fractionを排除した代わりに、粗いLODの鉱物混合をブリックヘッダーで管理:

```
dominant_mineral_id:    uint8   (1B)
secondary_mineral_id:   uint8   (1B)
blend_ratio:            uint8   (1B)
────────────────────
+3B per brick header（粗いLODブリックのみ）
```

### 細かいブリックの生存管理

- 粗化時: 細かいブリックをRAM/ディスクに退避、VRAMには粗いブリックのみ
- 接近時: 細かいブリックをVRAMに復帰（ラウンドトリップで情報損失なし）
- 修正済みデータ（weathering_degree変更、microcrack_density増加等）は退避データに含まれるため完全復元可能

---

## 決定 1c 量子化テーブルの更新

本決定により、決定1cの量子化テーブルを以下に修正:

| チャネル | 元の型 | 量子化後 | 備考 |
|---------|--------|---------|------|
| SDF | float16 | int8 + ブリックヘッダー（scale/offset） | |
| 法線 | — | snorm8x2 | 決定2で確定。SDF int8量子化のため必須 |
| mineral_id | uint8 | uint8 | 離散値 |
| porosity | float16 | uint8 | |
| weathering_degree | float16 | uint8 | 静的。ランタイム風化なし |
| microcrack_density | float16 | uint8 | 構造専用、レンダリング非参照 |
| crystal_orientation | — | snorm8x2 | 決定2で確定。生成時にVoronoi等で計算し格納 |

**削除:**
- volume_fraction[4] — 排除（PT anti-aliasing + ブリックヘッダー）
- grain_size — LUT導出（mineral_id + per-Entity modifier）
- damage_d — SRP分析により排除（拡張層のphase_field_dに統合）

**量子化後コア層: 9 bytes/voxel**（旧11Bから18%削減）

---

## 実装フェーズ

### Phase 1: 最小レンダリング (4B/voxel)

```
フィールド: SDF (1B) + 法線 (2B) + mineral_id (1B) = 4B

目標:
  - SBP + NanoVDB変換でフレームループ確立
  - mineral_id → LUTでアルベド + ラフネス取得
  - 単一鉱物の石をレンダリング
```

### Phase 2: フルコア + 多鉱物レンダリング (9B/voxel)

```
追加: porosity (1B) + weathering_degree (1B) + microcrack_density (1B)
      + crystal_orientation (2B) = +5B

目標:
  - 多鉱物岩（花崗岩）のレンダリング
  - 異方性グリント
  - 風化による色変化・粗さ変化・mineral_id遷移
  - SSS（porosity依存）
  - LOD集約 + ブリックヘッダーメタデータ

検証項目:
  - 粒界の離散的遷移の視覚品質（PT anti-aliasing効果の確認）
  - LOD遷移時の鉱物混合品質
  - crystal_orientationのグリント品質（花崗岩、大理石、片麻岩）
```

### Phase 3: 拡張層 + 破壊 (+20B/voxel, 対象領域のみ)

```
追加: 拡張層 (20B/voxel、シミュレーション対象領域のみ)

目標:
  - 陽的動力学 + 質量スケーリングによる破壊ソルバー
  - 力学パラメータのLUT導出 → 拡張層展開
  - phase_field_d → microcrack_density/SDFへのコミット
  - SDF連結成分分析 → Entity分割
  - 拡張層の展開/解放ライフサイクル
  - 衝撃エフェクト（粉塵パーティクル、音響、接触面SDF変更）
```

### Phase 4: 環境インタラクション

```
目標（風化省略により大幅スコープ縮小）:
  - 温度モデル（加熱/冷却/黒体輻射発光）
  - 熱衝撃（オプション: per-voxel temperature拡張層）
  - 濡れ表現（per-Entity wetness、視覚エフェクトのみ）
```

---

## 他の決定への依存関係

```
決定 2 (本決定)
  |
  +---> 決定 1c: 量子化テーブル更新（9B/voxel、法線+crystal_orientation確定）
  |
  +---> 決定 3 (メッシュ抽出): 法線格納確定済み。DCは格納済み法線を使用可能
  |
  +---> 第2章 (テクスチャ技術): mineral_id, porosity, weathering_degree,
  |     crystal_orientationがプロシージャルテクスチャの入力
  |
  +---> 第3章 (破壊): 拡張層のフィールドが破壊ソルバーの入出力。
  |     microcrack_density (コア) がソルバーへの入力かつコミット先
  |
  +---> 第5章 (植生): weathering_degreeが苔の成長条件（生物風化は植生システムの責務）
```
