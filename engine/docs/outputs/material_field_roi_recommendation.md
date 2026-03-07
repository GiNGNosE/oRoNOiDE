# Per-Voxel マテリアルフィールド ROI 推薦レポート

**参照元:**
- `engine/docs/discussion/02_material_field_discussion.md` — 候補 A/B/C、MECE/SRP 分析
- `engine/docs/design/01_foundation_design.md` — 決定 1a（SBP）、1b（適応的細分化）、1c（量子化）

**目的:** 候補 C（不可逆/可逆分離型、12-13B/voxel）をベースに、ROI（視覚的・構造的インパクト / メモリコスト）で実装フィールドを絞り込み、**現実的に実装可能かつハイパーリアルを達成する最小コアレイアウト** を推薦する。

---

## 1. 設計哲学: ROI 駆動の妥協戦略

### 1.1 問題意識

候補 C の MECE/SRP 分析は「何をカバーすべきか」を網羅的に特定した。しかし、全てを per-voxel で格納するとコア層だけで 12-13B/voxel になる。これは技術的に搭載可能だが、**「搭載可能である」ことと「搭載すべきである」は別の問い**である。

各フィールドの ROI を精査すると、一部のフィールドは「per-voxel 格納」以外の手段で同等以上の効果を達成できることが判明する。さらに、フィールドの責務を物理的に正しく分離することで、レンダリングパイプラインが読むフィールド数を最小化し、メモリ帯域幅の消費も削減できる。

### 1.2 ROI の定義

```
ROI = (視覚的インパクト + 構造的インパクト) / (per-voxel メモリコスト + 実装複雑性)
```

- **視覚的インパクト**: そのフィールドが存在しない場合、観察者が違いを知覚できるか
- **構造的インパクト**: そのフィールドが存在しない場合、破壊・風化の物理的正確性が低下するか
- **メモリコスト**: 28M ボクセル基準での VRAM 消費量
- **実装複雑性**: 代替手段の方がシンプルであれば、格納の ROI は下がる

### 1.3 妥協の原則

1. **適応的細分化が解像度を担保する**: ズームアップ時にボクセル密度が自然に上がるため、サブボクセル精度の追加フィールドは不要。表現限界は常にボクセル解像度であり、それで十分
2. **PT がジオメトリから光輸送を自動計算する**: SDF に刻まれた形状変化は、特別なレンダリングコードなしで正確にシェーディングされる
3. **プロシージャル導出が格納より高品質な場合がある**: 量子化された格納値より、連続値を返すプロシージャル関数の方が精度が高いケースでは、格納は冗長どころか品質劣化になる
4. **視覚的責務と構造的責務を物理的に正しく分離する**: 内部の微細亀裂（microcrack_density）はレンダリングに影響しない。表面の見た目（粗さ、クラック描画）は SDF ジオメトリと weathering_degree が担う
5. **導出可能なものは導出する**: mineral_id + LUT で計算可能なフィールドの per-voxel 格納は冗長

---

## 2. フィールド別 ROI 分析

### 2.1 ROI 評価テーブル

全ての候補フィールドを ROI で評価する。「Impact」は視覚 + 構造の総合。「Cost」は per-voxel バイト数。

| フィールド | Impact | Cost | 代替手段 | ROI | Verdict |
|-----------|:------:|:----:|---------|:---:|---------|
| **SDF** | 最高 | 1B | なし（形状の権威的定義） | ∞ | **コア必須** |
| **法線** | 最高 | 2B | SDF 勾配から導出（int8 量子化で精度不足） | 最高 | **コア必須**（決定 1c の依存事項） |
| **mineral_id** | 最高 | 1B | なし（鉱物同一性の権威的定義） | ∞ | **コア必須** |
| **porosity** | 高 | 1B | mineral_id LUT で「典型値」は出せるが、空間変動を表現不能 | 高 | **コア採用** |
| **weathering_degree** | 高 | 1B | なし（不可逆な表面変化の唯一の記録） | 最高 | **コア必須** |
| **microcrack_density** | 中-高 | 1B | なし（不可逆な内部構造損傷の唯一の記録） | 中-高 | **コア採用**（構造専用） |
| **crystal_orientation** | 高 | 2B | **プロシージャル Voronoi（連続精度、格納より高品質）** | **低**（格納の ROI） | **プロシージャル導出** |
| **volume_fraction[N]** | 中 | 3-4B | 適応的細分化 + mineral_id 境界（後述） | **低** | **排除** |
| **grain_size** | 低-中 | 1B | mineral_id LUT + per-Entity modifier（後述） | **低** | **排除（LUT 導出）** |
| **damage_d** | — | — | SRP により既に排除（phase_field_d として拡張層） | — | **排除済み** |

### 2.2 各フィールドの詳細評価

#### SDF (1B) — 形状の権威的真実

全てのサブシステム（レンダリング、衝突、破壊、CSG 編集）が依存する。代替不可能。int8 量子化 + ブリックヘッダー（scale/offset）で高精度を維持。

#### 法線 (2B) — レンダリング性能の生命線

SDF を int8 に量子化した結果、隣接ボクセル差分による法線計算の精度が著しく低下する。snorm8×2 の八面体マッピングで別チャネル格納することで、PT でのシェーディング品質を保証する。この 2B は決定 1c で「決定 2 で確定する」と記載された依存事項であり、本推薦で確定させる。

#### mineral_id (1B) — 鉱物同一性

「何の鉱物か」という問いに答える唯一のフィールド。色、反射率、比熱、破壊靭性 — 全ての物理的性質の出発点。離散値（uint8 = 最大 256 種）で十分。

#### porosity (1B) — 空隙構造

空隙率は鉱物種から独立に空間変動する。同じ石英でも、表面近くは風化で空隙が大きく、内部は緻密。この勾配は SSS（サブサーフェススキャッタリング）の透過深度、密度（→ 質量）、風化速度、破壊靭性の全てに影響する。mineral_id LUT では表現できない空間変動があるため、per-voxel 格納が正当化される。

#### weathering_degree (1B) — 表面状態の永続記録

**責務**: 「この表面はどれだけ変化したか」という唯一の問いに答える。化学風化（パティナ、色変化、薄膜干渉）と物理的な表面劣化（粗さの増加）の両方を統合的に表現する。

**視覚的役割**:
- 色変化（パティナ、酸化）: `LUT[mineral_id].albedo × weathering_color_shift(weathering_degree)`
- 薄膜干渉（虹色）: weathering_degree が連続的に増加すると薄膜厚が増し、干渉色が変化
- 表面粗さ: `roughness = f(LUT[mineral_id].base_roughness, weathering_degree)`
- 風化が進むほど表面がざらつく（差分侵食）。研磨操作では weathering_degree を局所的に低下させ、光沢面を表現

**02 の V2（表面粗さ）と V9（可視クラック）のカバー**:
- V2: weathering_degree + mineral_id LUT で導出。風化・研磨による粗さ変化を表現
- V9: **可視クラック = SDF ジオメトリ**。目に見えるクラックは SDF の形状変更として刻まれる（適応的細分化で十分な解像度）。per-voxel のクラック描画フィールドは不要

#### microcrack_density (1B) — 内部構造損傷の永続記録

**責務の明確化**: microcrack_density は**内部の微細亀裂**の密度を記録する。これらは肉眼では視認不能な構造的損傷であり、**レンダリングに直接影響しない**。

**構造的役割（専用）**:
- 破壊靭性の低下: `Gc = LUT[mineral_id].Gc × g(porosity) × h(microcrack_density)`
- 弾性率の低下: `E = LUT[mineral_id].E × g(porosity) × h(microcrack_density)`
- 引張強度の低下: `sigma_t = LUT[mineral_id].sigma_t × g(porosity) × h(microcrack_density)`
- 同じ箇所を繰り返し叩くと microcrack_density が蓄積し、次第に割れやすくなる

**レンダリングに影響しない理由**: 微細亀裂（マイクロクラック）は材料内部の結晶粒界や結晶内の微小な不連続面であり、その個々のサイズは典型的に数μm〜数十μm。ボクセル解像度（0.25mm = 250μm）の 1/10 以下であり、表面の見た目を直接変えるものではない。表面の粗さや色変化は風化（weathering_degree）や SDF ジオメトリ変更が担う。

**コア層に残す理由**: microcrack_density は不可逆な損傷蓄積であり、拡張層に置くとシミュレーション解放時に損傷履歴が失われる。「同じ場所を何度も叩くと割れやすくなる」という累積ダメージの正確な表現に不可欠。

#### crystal_orientation — プロシージャル Voronoi 導出

**per-voxel 格納を排除する理由**:

1. **精度**: snorm8×2（八面体マッピング）は 256×256 = 65,536 方向、約 1.4° 刻み。プロシージャル関数は連続値を返し、方向精度は事実上無限。スペキュラハイライトの位置に量子化バンディングが生じない分、**プロシージャルの方が高品質**

2. **グレインパターン**: Voronoi ベースの関数は自然な結晶粒パターンを生成する。セルサイズを `LUT[mineral_id].grain_size × grain_size_modifier` で制御すれば、鉱物ごとに異なる粒径の結晶構造が自動的に出現する。格納値は「この方向」という結果のみだが、プロシージャルは **グレイン構造ごと** 表現する

3. **破壊一貫性**: per-Entity の `generation_transform` で解決:
   ```
   seed_pos = generation_transform × voxel_entity_local_pos
   orientation = voronoi_crystal_orientation(seed_pos, mineral_id)
   ```
   破壊時、子エンティティは親の generation_transform を継承。同じボクセルは同じ seed_pos → 同じ orientation を返す

4. **節約**: 2B × 28M = 56MB の VRAM 削減。代わりに per-Entity で ~24-48B（数百エンティティで数 KB）

---

## 3. 四つの重要な妥協決定

### 3.1 サブボクセルスクラッチの排除

**妥協内容**: ボクセル解像度以下のスクラッチは表現しない。方向フィールドやスクラッチマップの追加を行わない。

**根拠**: 適応的細分化（決定 1b）により、プレイヤーが表面をスクラッチする際にはズームアップ状態にあり、ボクセル密度は自然に最大（~0.25mm/voxel）になっている。0.25mm 以下のスクラッチは肉眼では個々の溝として視認困難である。

```
ボクセル解像度別のスクラッチ表現:

  最細 (0.25mm):  ────╱╲──── SDF で溝を直接表現。PT が壁面法線・影を計算
  中間 (1mm):     ────╱╲──── SDF で表現可能だが、溝幅 ≥ 1mm
  粗い (5mm):     ──────── SDF 変更なし。目に見える変化なし

  → いずれのケースでも新規フィールドは不要
```

**リスク**: 0.25mm 以下の方向性スクラッチ（研磨痕のような平行線群）は表現できない。ただし、この精度レベルは実写映像でも近接マクロ撮影でなければ視認不能であり、ゲーム視点では知覚限界の外にある。

### 3.2 volume_fraction の排除

**妥協内容**: per-voxel の鉱物混合比（volume_fraction[N]、3-4B/voxel）を格納しない。

**根拠 — 三段論法**:

1. **適応的細分化により、最細解像度では各ボクセルは物理的に単一鉱物である**: 花崗岩の典型的な結晶粒径は 1-10mm。0.25mm ボクセルでは、1 つのボクセルが複数鉱物を含む確率は極めて低い。ボクセルは石英 OR 長石 OR 雲母であり、「60% 石英 + 40% 長石」は物理的に正しくない

2. **PT はボクセル境界で自然に鉱物遷移を表現する**: 光線が SDF 表面上の一点に当たる。その点のボクセルの mineral_id が材質を決定する。隣接ボクセルが異なる mineral_id を持つ場合、そこが粒界である。これは「離散的な鉱物境界」であり、物理的に正しい

3. **粗い LOD での鉱物混合は、集約時に計算すればよい**: 5mm ボクセルが複数の 0.25mm ボクセルを代表する場合、集約時に mineral_id の多数決と混合比を計算し、粗いボクセルのメタデータとして保持できる。これは LOD 生成の問題であり、per-voxel 格納の問題ではない

**節約効果**: 3-4B × 28M = 84-112 MB の VRAM 削減。

**候補 C の E1 評価との差異**: 候補 C は volume_fraction の存在を前提に E1（レンダリング忠実度）を「最高」と評価した。しかし上記の論証により、最細解像度では volume_fraction は物理的に冗長であり、排除しても E1 の実質的低下はない。むしろ、ボクセル解像度での離散的な鉱物境界は物理的により正確である。

**粒界バンディングへの対策**: 候補 A の E1 評価が「中（粒界でバンディング）」とされた問題は、volume_fraction の不在ではなく、**解像度不足**に起因する。適応的細分化により近距離では十分な解像度が確保されるため、バンディングは発生しない。遠距離では LOD 集約メタデータで緩和する。

### 3.3 grain_size の LUT 導出化

**妥協内容**: per-voxel の grain_size（1B）を格納せず、mineral_id LUT + per-Entity modifier で導出する。

**根拠**:

- **鉱物種ごとの粒径は概ね一定**: 同一岩石内で、石英は石英なりの粒径、雲母は雲母なりの粒径を持つ。mineral_id LUT に代表的粒径を格納すれば、大半のケースをカバーできる
- **岩石間の粒径差は per-Entity modifier で表現**: 「この花崗岩は特に粗粒」という情報は per-Entity の `grain_size_modifier`（float16、2B）で表現する。全ボクセルの grain_size に一律に乗算される
- **斑状組織（大結晶 + 微細基質）**: 大きな斑晶は別の mineral_id を持つため、LUT で異なる粒径が自然に返される
- **級化層理（堆積岩の粒径変化）**: per-Entity の `layering_orientation` + 深度に基づく簡易関数で近似可能。完全な per-voxel 精度は不要

**節約効果**: 1B × 28M = 28 MB の VRAM 削減。

**リスク**: 同一鉱物内で粒径が空間的に大きく変動する特殊な岩石（例: 接触変成帯の再結晶）では、LUT + modifier では不十分な場合がある。ただし、これは極めて稀なケースであり、必要に応じて per-Entity の低解像度 3D テクスチャ（例: 16^3、~4KB）で補完できる。

### 3.4 crystal_orientation のプロシージャル導出化

**妥協内容**: per-voxel の crystal_orientation（snorm8×2、2B）を格納せず、Voronoi ベースのプロシージャル関数で導出する。

**根拠**:

| 側面 | 格納 (snorm8×2) | プロシージャル (Voronoi) |
|------|:---:|:---:|
| 方向精度 | 1.4° 刻み（65,536 方向） | **連続値（無限精度）** |
| グレインパターン | 生成器依存の個別値 | **Voronoi で自然な粒界が自動出現** |
| メモリ | 2B/voxel (56MB @ 28M) | **0B/voxel + ~24-48B/Entity** |
| 破壊一貫性 | 自動（格納済み） | generation_transform で保証 |
| 計算コスト | メモリ読み取り (帯域消費) | Voronoi 評価 (~27 hash + distance) |
| 地質学的制御 | 生成時に自由設定 | per-Entity パラメータで制御 |

**Voronoi 関数の設計**:
```
voronoi_crystal_orientation(seed_pos, mineral_id, entity_params):
  cell_size = LUT[mineral_id].grain_size × entity_params.grain_size_modifier
  quantized_pos = floor(seed_pos / cell_size)

  // 3×3×3 近傍セルの中で最近接を探索
  min_dist = ∞
  for offset in [-1, 0, +1]^3:
    cell = quantized_pos + offset
    cell_center = (cell + hash_offset(cell)) × cell_size
    dist = distance(seed_pos, cell_center)
    if dist < min_dist:
      min_dist = dist
      nearest_cell = cell

  // セル ID から決定論的に方位を生成
  return hash_to_orientation(nearest_cell, mineral_id)
```

**破壊時の座標安定性**:
```
[per-Entity に追加: generation_transform]

生成時:
  entity.generation_transform = entity の初期ワールド変換

プロシージャル評価:
  seed_pos = generation_transform × voxel_entity_local_pos
  → 常に「生成時のワールド空間」で評価

破壊時:
  child.generation_transform =
    parent.generation_transform × parent_to_child_local_transform
  → 子エンティティの voxel が同じ seed_pos を得ることを保証
```

**節約効果**: 2B × 28M = 56 MB の VRAM 削減。per-Entity コストは ~24-48B × 数百 Entity = 数 KB（無視可能）。

**品質向上**: 格納値は 1.4° の量子化ノイズを含むが、プロシージャルは連続値。スペキュラハイライトの滑らかさと粒界パターンの自然さの両面で、**プロシージャルの方が高品質**。

---

## 4. 推薦コアレイアウト

### 4.1 コア層 — 7 bytes/voxel

```
[コア層 — 全アクティブボクセル — リアリティの源泉]
SDF:                 int8 + header     (1B + ブリックヘッダ)
法線:                snorm8×2          (2B)   ← 八面体マッピング
mineral_id:          uint8             (1B)
porosity:            uint8             (1B)
weathering_degree:   uint8             (1B)   ← 表面状態の唯一の視覚的記録
microcrack_density:  uint8             (1B)   ← 内部構造損傷の唯一の記録（非視覚）
──────────────────────────────────────────
合計: 7 bytes/voxel（量子化後）
```

### 4.2 責務分離マップ

```
[レンダリングが読むフィールド]

  SDF ──→ 形状（レイマーチング / メッシュ抽出）
  法線 ──→ シェーディング方向
  mineral_id ──→ LUT ──→ 基本色、反射率、SSS パラメータ
  porosity ──→ SSS 深度、透過率
  weathering_degree ──→ 色変化（パティナ）、薄膜干渉、表面粗さ

  crystal_orientation（プロシージャル）──→ 異方性グリント

[レンダリングが読まないフィールド]

  microcrack_density ──→ 破壊シミュレーション専用
    └──→ Gc 低下、E 低下、sigma_t 低下（LUT 導出時の劣化係数）

[レンダリングパスのメモリアクセス: 5 フィールド / 6B]
  SDF(1) + 法線(2) + mineral_id(1) + porosity(1) + weathering_degree(1) = 6B
  + プロシージャル crystal_orientation（ALU コスト、メモリアクセスなし）

[破壊シミュレーションが読むフィールド: 全 6 フィールド + プロシージャル]
  SDF, mineral_id, porosity, microcrack_density
  + crystal_orientation（プロシージャル → 劈開面方向）
  → 拡張層に E, nu, Gc, sigma_t, anisotropy_tensor を展開
```

### 4.3 導出可能なレンダリングパラメータ

コア層 + プロシージャルから以下のパラメータがシェーダで導出される（格納不要）:

| パラメータ | 導出式 | 用途 |
|-----------|--------|------|
| **基本色（アルベド）** | `LUT[mineral_id].albedo × weathering_color_shift(weathering_degree)` | PBR ベースカラー |
| **表面粗さ** | `f(LUT[mineral_id].base_roughness, weathering_degree)` | PBR ラフネス |
| **金属/スペキュラ応答** | `LUT[mineral_id].F0` | フレネル反射率 |
| **SSS パラメータ** | `g(LUT[mineral_id].extinction, porosity)` | サブサーフェス散乱 |
| **密度** | `LUT[mineral_id].density × (1 - porosity)` | 質量計算 |
| **grain_size** | `LUT[mineral_id].grain_size × grain_size_modifier` | テクスチャスケール |
| **crystal_orientation** | `voronoi_crystal_orientation(seed_pos, mineral_id)` | 異方性反射、劈開面 |

表面粗さの導出式から microcrack_density を除外した点に注意。粗さは weathering_degree と mineral_id の固有粗さのみで決定される。

### 4.4 候補 C との差分

| フィールド | 候補 C | 本推薦 | 差分 | 理由 |
|-----------|--------|--------|------|------|
| SDF | 1B | 1B | ±0 | — |
| 法線 | 2B | 2B | ±0 | — |
| mineral_id | 1B | 1B | ±0 | — |
| volume_fraction | 3-4B | **0B** | **-3~4B** | 適応的細分化で冗長（Section 3.2） |
| porosity | 1B | 1B | ±0 | — |
| grain_size | 1B | **0B** | **-1B** | LUT 導出（Section 3.3） |
| crystal_orientation | 2B | **0B** | **-2B** | プロシージャル Voronoi（Section 3.4） |
| weathering_degree | 1B | 1B | ±0 | — |
| microcrack_density | 1B | 1B | ±0 | 責務を構造専用に限定 |
| **合計** | **12-13B** | **7B** | **-5~6B** | **コア層 46% 削減** |

---

## 5. スクラッチパイプライン設計

### 5.1 前提

- 当たり判定は細かい（SDF ベースの衝突検出）
- 接触面の座標（position）と方向（direction）が衝突検出から得られる
- 適応的細分化により、スクラッチ操作時のボクセルは最細解像度（~0.25mm）

### 5.2 パイプライン

```
接触検出（衝突システム）
  │
  ├── contact_position:  接触点のワールド座標
  ├── contact_direction: 接触の移動方向（正規化ベクトル）
  ├── contact_depth:     接触の押し込み深さ
  └── contact_width:     接触面の幅（ツール依存）
  │
  ▼
適応的細分化の確認
  │
  ├── 接触領域のボクセル解像度を確認
  ├── 必要に応じて細分化をトリガー（ズームアップで既に細分化済みの想定）
  └── 細分化完了後、以下の処理を実行
  │
  ▼
SDF 修正（ジオメトリレベルのスクラッチ）
  │
  ├── 接触軌跡に沿ったボクセル列を特定
  ├── 各ボクセルの SDF 値を溝プロファイルに従って減算:
  │     SDF_new = min(SDF_old, SDF_old - scratch_profile(d))
  │     d = ボクセル中心からスクラッチ中心軸への距離
  │     scratch_profile: ガウシアン or V 字形状（ツール依存）
  ├── 溝の深さと幅は contact_depth と contact_width から決定
  └── SDF 変更はブリック dirty flag を発火
  │
  ▼
法線の再計算
  │
  └── SDF 変更されたボクセルの法線を SDF 勾配から再計算し格納
      （溝の壁面法線が自然に発生）
  │
  ▼
microcrack_density の更新（内部構造損傷）
  │
  └── 接触圧力が十分高い場合、接触領域周辺のボクセルの
      microcrack_density を増加（構造的弱化の蓄積）
      → 視覚的変化なし。次回の衝撃でより割れやすくなる
  │
  ▼
topology_version のインクリメント
  │
  └── 構造不変条件に従い、SDF 変更後に version を更新
  │
  ▼
PT レンダリング（自動）
  │
  ├── 光線が修正された SDF 表面にヒット
  ├── 溝の壁面法線 → 方向性のあるスペキュラ反射
  ├── 溝内部の自己遮蔽 → グレージング角でのシャドウ
  ├── 溝内マルチバウンス → 自然な暗化
  └── 新鮮な破断面（低 weathering_degree）→ 鉱物本来の色が露出
```

### 5.3 新規フィールド不要の理由

| スクラッチの側面 | 対応するメカニズム | 追加ストレージ |
|----------------|------------------|:------------:|
| 溝のジオメトリ | SDF（CSG 減算） | 0B |
| 溝の壁面法線 | 法線（SDF から再計算） | 0B |
| 溝の視覚的粗さ | SDF ジオメトリ自体（PT が自動計算） | 0B |
| 溝内の光散乱 | PT が自動計算 | 0B |
| スクラッチ方向 | SDF ジオメトリに内在（溝の形状 = 方向） | 0B |
| 構造的弱化 | microcrack_density（内部損傷蓄積） | 0B |

### 5.4 操作の種類と対応

| 操作 | SDF への影響 | microcrack_density | weathering_degree | 結果 |
|------|:----------:|:-----------------:|:----------------:|------|
| **引っかく** | 線状の溝 | 接触圧力に応じて微増 | 変化なし | 方向性のある傷跡（SDF ジオメトリ） |
| **削る（彫刻）** | 広い面の除去 | 周辺で微増 | 新規露出面 = 0（新鮮） | 内部構造の露出。低 weathering で鉱物色が鮮やか |
| **研ぐ（研磨）** | 表面の微小凸凹を平坦化 | 変化なし | **↓**（研磨で低下） | weathering_degree 低下 → 粗さ低下 → 光沢面 |
| **叩く（衝撃）** | 破壊ソルバー経由 | ↑↑（衝撃損傷） | 変化なし | 内部損傷蓄積 → 次回でより割れやすく |

研磨操作では **weathering_degree** が低下する。これは「表面の風化層を削り取って新鮮面を露出させる」ことに対応し、粗さの低下と光沢の増加を正しく表現する。

---

## 6. 拡張層・per-Entity 設計

候補 C の拡張層設計を踏襲し、crystal_orientation のプロシージャル化に伴う変更を反映する。

### 6.1 拡張層（シミュレーション対象領域のみ、解放可能）

```
[拡張層]
phase_field_d:       float16           (2B)   ← ソルバー作業変数
E:                   float16           (2B)   ← mineral_id + porosity + microcrack_density → LUT 導出
nu:                  float16           (2B)
Gc:                  float16           (2B)
sigma_t:             float16           (2B)
anisotropy_tensor:   float16×3         (6B)   ← プロシージャル crystal_orientation + mineral_id → 導出
moisture:            float16           (2B)   ← 可逆（蒸発する）
──────────────────────────────────────────
拡張: 20 bytes/voxel（対象領域のみ）
```

anisotropy_tensor の導出元が「格納された crystal_orientation」から「プロシージャル crystal_orientation」に変わるが、拡張層のレイアウト自体は不変。展開時に Voronoi 関数を呼び出して初期化する。

拡張層のライフサイクルも候補 C と同一:
- **展開**: 衝突/破壊/風化シミュレーション開始時にコアフィールド + プロシージャルから力学パラメータを導出
- **実行**: ソルバーが phase_field_d を伝播、力学パラメータを更新
- **コミット**: 結果を microcrack_density（コア）と SDF（コア）に反映
- **解放**: VRAM を回復。コア層だけで完璧なレンダリングが継続

### 6.2 per-Entity メタデータ

```
[per-Entity]
topology_version:      uint32           (4B)
temperature:           float16          (2B)    ← 可逆（環境温度に収束）
layering_orientation:  float16×3        (6B)    ← 堆積層方向（石全体で概ね均一）
grain_size_modifier:   float16          (2B)    ← LUT 粒径への乗算係数
generation_transform:  float32×3×4      (48B)   ← 【新規】プロシージャル座標の原点変換
──────────────────────────────────────────
per-Entity: 62 bytes/Entity
```

`generation_transform` はプロシージャル crystal_orientation の座標安定性を保証する。生成時のワールド変換を記録し、破壊時に子エンティティに継承される。48B/Entity は数百エンティティで ~15KB（無視可能）。

### 6.3 温度モデル

02 の Section 3.7 で設計済みの 3 層温度モデルをそのまま踏襲:
- Layer 1: per-Entity temperature（常時、2B/Entity）
- Layer 2: 導出熱物性（格納なし、LUT + コアフィールドから計算）
- Layer 3: per-voxel temperature（拡張層、熱衝撃/局所加熱時のみ）

---

## 7. 妥協点のリスク評価

### 7.1 リスクマトリクス

| 妥協 | 影響を受けるシナリオ | 発生頻度 | 視覚的影響 | 構造的影響 | 対処策 |
|------|-------------------|:--------:|:---------:|:---------:|--------|
| サブボクセルスクラッチ排除 | 0.25mm 以下の平行研磨痕の表現 | 極低 | 低（肉眼限界以下） | なし | プロシージャルシェーダで微細テクスチャとして近似可能 |
| volume_fraction 排除 | 粗い LOD での鉱物混合表示 | 中 | 低-中（遠距離のため） | なし | LOD 集約時にブリックメタデータとして混合比を計算・格納 |
| grain_size 排除 | 接触変成帯の再結晶粒径変動 | 極低 | 低 | 低 | per-Entity 低解像度 3D テクスチャ（16^3、~4KB）で補完 |
| crystal_orientation プロシージャル化 | generation_transform の継承ミス | 低 | 中（グリントパターン変化） | 中（劈開面方向変化） | 破壊時の transform 継承を構造不変条件としてテスト |
| microcrack_density の非視覚化 | 衝撃痕の即座の視覚的フィードバック | 中 | 低（SDF 変更で代替） | なし | 衝撃の視覚的結果は SDF ジオメトリ変更で表現 |

### 7.2 volume_fraction 排除の限界ケース

**問題**: 粗い LOD（遠距離）で 1 つのボクセルが複数鉱物の領域を代表する場合、mineral_id の多数決では少数派鉱物の情報が失われる。

**対処**: LOD 集約時に以下のメタデータをブリックヘッダーに格納:
- `dominant_mineral_id`: 多数派鉱物
- `secondary_mineral_id`: 次点鉱物（uint8、1B）
- `blend_ratio`: 混合比（uint8、1B）

これにより、粗い LOD でも 2 鉱物の混合が表現可能になる。ブリックヘッダーへの格納なので per-voxel コストはゼロ。

### 7.3 microcrack_density 非視覚化の限界ケース

**問題**: プレイヤーが石を叩いた時、SDF が変更されるほどの衝撃でなければ視覚的フィードバックがない。「叩いたのに何も変わらない」と感じる可能性。

**対処**:
- 衝撃が SDF 変更閾値未満でも、パーティクルエフェクト（粉塵）や音響フィードバックで「何かが起きた」ことをプレイヤーに伝達
- 内部的には microcrack_density が蓄積しており、同じ箇所を繰り返し叩けば閾値を超えて SDF 変更（破壊）が発生する
- これは現実の石の挙動と一致する: 石を軽く叩いても見た目は変わらないが、内部的にダメージが蓄積している

---

## 8. メモリ見積もりと候補比較

### 8.1 コア層の比較

| 構成 | コア層 B/voxel | 28M voxels | 200M voxels |
|------|:-------------:|:----------:|:-----------:|
| 候補 A（10B） | 10 | 280 MB | 2.0 GB |
| 候補 B（24B） | 24 | 672 MB | 4.8 GB |
| 候補 C（12-13B） | 12-13 | 336-364 MB | 2.4-2.6 GB |
| **本推薦（7B）** | **7** | **196 MB** | **1.4 GB** |

### 8.2 トータル VRAM（シミュレーションアクティブ時）

操作圏 3 石（各 ~4.3M ボクセル）でシミュレーション中の場合:

| 構成 | コア層 | 拡張層 | 合計 |
|------|--------|--------|------|
| 候補 C | 336-364 MB | 3 × 4.3M × 20B = 258 MB | **~600-620 MB** |
| **本推薦** | **196 MB** | 3 × 4.3M × 20B = 258 MB | **~454 MB** |

本推薦はコア層で ~140-168 MB を節約。拡張層は候補 C と同一。

### 8.3 評価軸による比較

| 評価軸 | 重み | 候補 A | 候補 B | 候補 C | **本推薦** |
|--------|------|:------:|:------:|:------:|:----------:|
| E1 レンダリング忠実度 | 最高 | 中 | 最高 | 最高 | **最高** |
| E2 破壊正確性 | 最高 | 中 | 最高 | 最高 | **最高** |
| E3 メモリ効率 | 高 | 最高 (280MB) | 低 (672MB) | 高 (336MB) | **最高 (196MB)** |
| E4 ランタイムコスト | 高 | 最高 | 高 | 中 | **最高** |
| E5 段階的導入 | 中 | 高 | 低 | 最高 | **最高** |
| E6 永続化 | 中 | 高 | 最高 | 最高 | **最高** |
| E7 拡張性 | 中 | 低 | 高 | 最高 | **最高** |

**本推薦は候補 A よりメモリ効率が高く、候補 C の品質と拡張性を維持する。**

候補 A との差異: 候補 A は 10B で E1/E2 が「中」。本推薦は 7B で E1/E2 が「最高」。この逆転は三つの洞察による:
1. volume_fraction の不在ではなく**解像度不足**が品質低下の原因（適応的細分化で解決）
2. プロシージャル crystal_orientation は**格納より高品質**（連続精度 + 自然なグレインパターン）
3. microcrack_density のレンダリング責務は物理的に不適切であり、排除が**品質向上**

E4 が「最高」に改善: コア層のフィールド数が 6 に減少し、レンダリングシェーダが読むのは 5 フィールド（6B）のみ。メモリ帯域幅の消費が最小化される。crystal_orientation の Voronoi 評価は ALU コストだが、現代 GPU では ALU よりメモリ帯域幅がボトルネックになることが多く、トータルでは有利。

---

## 9. 実装フェーズとの対応

### 9.1 Phase 1: 最小コア層でレンダリング

```
実装するフィールド:
  SDF (1B) + 法線 (2B) + mineral_id (1B) = 4B/voxel

目標:
  - SBP + NanoVDB 変換でのフレームループ
  - mineral_id → LUT でアルベド + ラフネスを取得
  - 法線でシェーディング
  - 単一鉱物の石をレンダリング
```

### 9.2 Phase 2: フルコア層 + プロシージャル

```
追加フィールド:
  porosity (1B) + weathering_degree (1B) + microcrack_density (1B) = 3B
  → 合計 7B/voxel

追加システム:
  - プロシージャル crystal_orientation（Voronoi 関数）
  - per-Entity generation_transform

目標:
  - 鉱物混合（隣接 mineral_id 境界）のレンダリング
  - 異方性グリント（プロシージャル crystal_orientation）
  - 風化による色変化・粗さ変化（weathering_degree）
  - SSS（porosity）
  - スクラッチパイプラインの実装
```

### 9.3 Phase 3: 拡張層 + 破壊シミュレーション

```
追加:
  拡張層 (20B/voxel、対象領域のみ)

目標:
  - フェーズフィールド破壊ソルバー
  - 力学パラメータの LUT 導出（microcrack_density で劣化反映）→ 拡張層展開
  - microcrack_density へのコミット（構造的損傷蓄積）
  - 拡張層の展開/解放ライフサイクル
  - 破壊時の generation_transform 継承
```

### 9.4 Phase 4: 風化 + 環境連成

```
追加:
  moisture (拡張層)、per-voxel temperature (拡張層)

目標:
  - 風化エージェント（weathering_degree↑, porosity↑）
  - 温度モデル（3 層）
  - 水分浸透シミュレーション
  - 熱衝撃 → microcrack_density↑（拡張層経由でコアにコミット）
```

---

## 付録 A: 決定 1c（量子化テーブル）との整合性

01_foundation_design.md の決定 1c で定義された量子化テーブルとの差分:

| フィールド | 1c テーブル | 本推薦 | 差分 | 備考 |
|-----------|:----------:|:------:|:----:|------|
| SDF | int8 (1B) | int8 (1B) | — | 同一 |
| 法線 | 未記載（決定 2 に委譲） | snorm8×2 (2B) | **+2B** | 本推薦で確定。SDF int8 量子化のため必須 |
| mineral_id | uint8 (1B) | uint8 (1B) | — | 同一 |
| volume_fraction[4] | uint8×4 (4B) | **なし** | **-4B** | 適応的細分化で冗長（Section 3.2） |
| porosity | uint8 (1B) | uint8 (1B) | — | 同一 |
| grain_size | uint8 (1B) | **なし** | **-1B** | LUT 導出（Section 3.3） |
| crystal_orientation | 未記載 | **プロシージャル** | **±0B** | Voronoi 導出。per-Entity transform で座標安定性確保 |
| weathering_degree | uint8 (1B) | uint8 (1B) | — | 同一。責務を拡張（全視覚的表面変化） |
| microcrack_density | uint8 (1B) | uint8 (1B) | — | 同一。責務を限定（構造専用、非視覚） |
| damage_d | uint8 (1B) | **なし** | **-1B** | SRP により拡張層の phase_field_d に移行 |
| **合計** | **11B** | **7B** | **-4B** | |

**注記**: 決定 1c の量子化テーブルは決定 2（本決定）の結果を反映して更新される必要がある。主な変更点は:
1. volume_fraction、grain_size、damage_d の行を削除
2. 法線（snorm8×2、2B）の行を追加
3. crystal_orientation は「プロシージャル導出（per-voxel 格納なし）」と注記
4. microcrack_density に「構造専用、レンダリング非参照」と注記
5. weathering_degree に「全視覚的表面変化を統合」と注記
6. 合計を 11B → 7B に修正
7. メモリ見積もりテーブルを 7B/voxel 基準で再計算（28M → 196MB、200M → 1.4GB）
