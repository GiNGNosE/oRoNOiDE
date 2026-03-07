# 第1章 基盤設計 — 決定事項

**議論ファイル**: `engine/docs/discussion/01_foundation.md`

---

## 決定 0: 設計原則と構造不変条件

> **6原則 + 5構造不変条件**

### 設計原則

1. **SDFが唯一の権威的形状定義** — メッシュ、衝突形状、LODは全てSDFからの導出。二重管理を排除
2. **コア/拡張の分離不変条件** — コア層のみで完璧なレンダリング。拡張層は解放しても情報損失なし
3. **ROI駆動の設計判断** — 物理的正確性を目指すが、知覚不能な精度やコストに見合わない機能は排除する
4. **導出可能なものは格納しない** — LUTから導出可能なフィールド、他のフィールドから再計算可能なキャッシュは権威的データとして持たない
5. **GPU中心の計算モデル** — パフォーマンスクリティカルな処理（PT、破壊ソルバー、衝突判定）はGPU上で完結
6. **段階的実装** — Phase 1から動作する状態を確保し、段階的に自前実装に移行。後段Phaseは性能が要求した場合のみ

### 構造不変条件

| ID | 不変条件 |
|----|---------|
| INV-1 | SBPのブリックが形状の唯一のソース。全サブシステムがSBPを直接参照。派生メッシュはランタイムに存在しない |
| INV-2 | コア層フィールドはEntity存続中は常に有効。拡張層の有無に関わらずレンダリング・物理が機能する |
| INV-3 | topology_version変更時に全導出キャッシュが即時再計算される。サブシステム間で形状の不整合が1フレームも存在しない |
| INV-4 | EntityIdのgeneration checkが全アクセスパスで実行される。破壊分裂後の古い参照が無効データに到達しない |
| INV-5 | ブリックヘッダーのowner_idがEntity↔ブリック関係の権威的データ。per-EntityのBrickSetは導出キャッシュであり、owner_idから再構築可能 |

---

## 決定 1a: スパースボリューム構造

> **カスタム Sparse Brick Pool（4フェーズ段階的実装）**

### 決定理由

1. 変更ブリックのみの差分転送により、編集→描画レイテンシが最小
2. ブリック単位の O(1) 追加/削除が適応的細分化と直結
3. 段階的に GPU 直接マップへ移行可能（NanoVDB 依存の排除はオプション）
4. per-voxel レイアウト・キャッシュ最適化を本プロジェクトに完全特化可能
5. Phase 1 で NanoVDB をレンダリングに利用するため、初期から描画品質を確保

### 4フェーズ ロードマップ

| Phase | 内容 | GPU レンダリング |
|-------|------|----------------|
| 1 | CPU 側にハッシュベースブリックプール + NanoVDB 変換 | NanoVDB 経由 |
| 2 | dirty flag による差分転送最適化 | NanoVDB 経由（差分反映） |
| 3 | 一部パス（衝突判定等）で GPU バッファ直接参照（SSBO） | ハイブリッド |
| 4 | **（オプション）** 全パスで GPU 直接マップ。自前 HDDA + Morton/Z-order | 完全自前 |

### 受容したリスク

| リスク | 緩和策 |
|--------|--------|
| 開発工数 | Phase 1 で早期に動作状態を確保 |
| Phase 4 不要の可能性 | Phase 3 で十分な性能が得られれば Phase 4 は実施しない。ベンチマーク判断 |
| VFX 互換性なし | VDB ↔ Brick Pool 変換レイヤー（インポート/エクスポート時のみ） |
| NeuralVDB 非適用 | 標準圧縮（zstd/lz4）で対応（決定 1c） |

---

## 決定 1b: 解像度戦略

> **適応的細分化（マルチ解像度ブリック）**

### 決定理由

1. 「単一権威状態」の設計原則に最も忠実 — 全データが単一の権威的グリッド
2. 破壊断面の品質が構造的に保証 — 高解像度化済み領域の断面がそのまま権威的データ
3. 「それっぽい」リスクの構造的排除 — 一度正しく生成したデータを永続化
4. Brick Pool の O(1) 追加/削除が細分化/粗化と直結
5. 高解像度領域では物理シミュレーションも高精度に実行可能

### メモリ管理

- LRU 戦略: メモリバジェット上限を設定し、遠距離/古い高解像度ブリックから粗化
- 高解像度データの生成コストは細分化イベント時のみ（毎フレームコストに乗らない）
- シェーダーレベルのマイクロディテール（法線擾乱程度）のみプロシージャル評価

### 受容したリスク

| リスク | 緩和策 |
|--------|--------|
| メモリ消費の変動 | LRU ベースのバジェット管理 |
| 細分化時の生成コスト | 非同期生成 + 粗い解像度で暫定表示 |
| LOD 遷移のポッピング | 時間的ブレンディング |

---

## 決定 1c: ブリック単位圧縮

> **チャネル別量子化（Phase 1 常時有効）+ 標準圧縮（Phase 3+ ストレージ用）**

### 決定理由

1. SBP のアクティブボクセルは既にスパース性が除去済みで凝集率が高く、追加圧縮の効果は限定的 — 量子化（~1.7x）が最も費用対効果が高い
2. BC7/ASTC はテクスチャサンプリングハードウェア経由のデコードを前提とし、SBP の SSBO 直接アクセス設計と根本的に非互換 — **除外**
3. 適応的細分化（決定 1b）そのものが最大の「圧縮」（遠距離で桁違いのボクセル削減）
4. ストレージ圧縮はディスク↔VRAM のストリーミング層に限定し、VRAM 上のアクティブブリックには影響を与えない

### ランタイム圧縮（VRAM 上、Phase 1 から常時有効）

| チャネル | 元の型 | 量子化後 | 備考 |
|---------|--------|---------|------|
| SDF | float16 | int8 + ブリックヘッダー（scale/offset） | 高精度を維持 |
| 法線 | — | snorm8x2 (2B) | 決定2で確定。SDF int8量子化のため必須 |
| mineral_id | uint8 | uint8 | 離散値、変更なし |
| porosity | float16 | uint8 | |
| weathering_degree | float16 | uint8 | 静的（ランタイム風化なし） |
| microcrack_density | float16 | uint8 | 構造専用、レンダリング非参照 |
| crystal_orientation | — | snorm8x2 (2B) | 決定2で確定。生成時に計算し格納 |

**決定2により削除:** volume_fraction（排除）、grain_size（LUT導出）、damage_d（拡張層に統合）

**量子化後コア層: 9 bytes/voxel**（決定2の結果を反映）

### ストレージ圧縮（Phase 3+ で導入）

- **標準圧縮（zstd / lz4）**: ブリック単位で圧縮。実績のあるアルゴリズムで十分な圧縮率（2x〜5x）を確保
- ディスク→VRAM 転送時にのみデコード。VRAM 上は常に非圧縮（量子化済み）
- Neural Brick Codec は将来の研究オプションとして位置付け。標準圧縮で不十分な場合のみ検討

### メモリ見積もり（量子化後）

| シナリオ | ボクセル数 | VRAM (9B/voxel) |
|---------|----------|-----------------|
| 平均的な風景（石 ~300個 + 地形の岩） | ~28M | ~252 MB |
| 大規模シーン（石 1000個 + 広域地形） | ~200M | ~1.8 GB |

### 決定 2 への依存事項（解決済み）

SDF を int8 に量子化するため、隣接ボクセル差分による法線計算の精度が低下する。**決定 2 により、法線を snorm8x2（2B/voxel）で別チャネル格納することが確定。**

---

## 決定 2: Per-Voxel マテリアルフィールド

> **候補C（不可逆/可逆分離型）をベースに、ROI分析で最適化した9B/voxelコア層を採用**

詳細: `engine/docs/design/02_material_field_design.md`

### 概要

- コア層 9B/voxel: SDF(1) + 法線(2) + mineral_id(1) + porosity(1) + weathering_degree(1) + microcrack_density(1) + crystal_orientation(2)
- 拡張層 20B/voxel: シミュレーション対象領域のみ、解放可能
- GPUメモリレイアウト: ブリックレベルSoA
- 風化シミュレーション: 省略（weathering_degreeは生成時の静的パラメータ）

---

## 決定 3: 形状表現とメッシュ抽出

> **SDF直接レイマーチング + SDF直接物理。ランタイムメッシュ生成なし。**

詳細: `engine/docs/design/03_shape_representation_design.md`

### 概要

- レンダリング: PTがSBP上のSDFをsphere tracingで直接歩く（2段階: ブリックDDA → ブリック内sphere tracing）
- 衝突判定: 自前物理エンジンがSDF直接クエリ。メッシュ不要
- ブリックスキップ: ヘッダーにsdf_min/max（2B）追加。表面から離れたブリックを丸ごとスキップ
- int8量子化安全性: 量子化ポリシー（正→floor, 負→ceil）で過大評価を排除
- DC: オフラインエクスポート用ユーティリティとして位置付け。ランタイムパイプライン外

---

## 決定 4: Entity管理 + ID体系 + Version Parity

> **汎用ECS不採用。均質Entity構造に特化したシンプル管理 + Generational Index + 即時再計算。**

詳細: `engine/docs/design/04_ecs_id_version_design.md`

### 概要

- 汎用ECSフレームワーク不採用。全EntityがStone（同一構造）のため密配列 + 線形走査で十分
- Entity ID: Generational Index（24bit slot + 8bit generation = 32bit）
- Entity ↔ ブリック: ブリックヘッダーにowner_id（権威的）+ per-EntityのBrickSet（導出キャッシュ）
- Version Parity: topology_version変更時に即時再計算（導出キャッシュ、BrickSet、LOD）
- 破壊分裂: 最大破片が親ID継承、残りは新規ID発行
- 消滅: 残存ボクセル数が閾値以下で粉塵パーティクルに変換して消滅

---

## 決定 5: 表面レンダリング設計

> **Anisotropic GGX 単一モデル + Implicit Function Detail + Ray Footprint LOD。**

詳細: `engine/docs/design/05_surface_rendering_design.md`

### 概要

- BRDF: Anisotropic GGX (Cook-Torrance) + Energy Compensation。単一モデル、mineral_id LUT パラメタライズ。Per-mineral BRDF 分岐なし
- SSS: Volumetric transport（BRDF 外）。extinction 値で不透明〜半透明を連続的に制御
- 薄膜干渉: Belcour RGB 近似、Fresnel 項修正。weathering_degree 駆動
- Glint: 特別なモデル不要。Implicit function geometry + GGX を PT が直接サンプリング
- サブボクセルディテール: Implicit function perturbation（段階的: Generic FBM → 劈開鉱物に mineral-specific）
- Albedo variation: Procedural noise で十分。per-voxel 格納なし（テスト後に再評価）
- LOD: Ray footprint 基準の連続的フェードアウト + NDF filtering 補償

---

## 決定 6: 生成パイプライン設計

> **位置ベース決定論的関数 + Voronoi/Layered プリミティブ合成 + Voronoi Fracture 形状生成。**

詳細: `engine/docs/design/06_generation_pipeline_design.md`

### 概要

- パイプライン: Shape(SDF) → Mineral Distribution → Crystal Orientation → Porosity → Weathering → Normal → Metadata
- 全ステージが f(position, seed, params) の決定論的関数。解像度を意識しない
- 鉱物分布: Voronoi + Layered の2プリミティブの合成で全岩石カテゴリを表現
- 層構造: 変形場（fold_field）で褶曲・層厚変動を表現
- 形状: Voronoi Fracture Fragment + 曲率選択的風化 + 差別侵食
- 細粒岩: 格納 mineral_id と視覚 mineral_id の分離（render-time procedural evaluation）
- 適応的細分化: 同じ seed + 同じパイプラインを細かい解像度で再実行。特別なロジック不要
- Phase 1: CPU。GPU compute への移行パスは自明
