# Stone Labeling Guide (v1)

## 1. なぜ石ラベリングが必要か

`stone` を単一クラスで扱うと、学習時に「どれも石」と見なされやすく、以下の問題が起きる。

- 生成結果の地域性/環境性が弱くなる
- 形状は石っぽいが材質感や破壊挙動が不自然になる
- ゲーム内条件（バイオーム、湿度、侵食履歴）と生成条件が結びつかない

本ガイドは、**本質的な切り分け軸**を先に定義し、次に**実務で回せるラベル仕様**に落とし込む。

---

## 2. 本質的な石の切り分け軸 (Conceptual Taxonomy)

石の「イデア」を 1 軸で定義するのは困難なので、複数軸の組み合わせとして扱う。

### 2.1 成因/環境軸 (Formation & Environment)

- どこで形成・輸送・堆積・露出したか
- 例: `riverbed`, `talus_slope`, `coastal`, `volcanic_field`, `glacial`, `quarry`
- 目的: 同じ鉱物でも摩耗・丸み・表面テクスチャの違いを表現する

### 2.2 材質軸 (Material)

- 鉱物群、粒度、結合性、空隙率、層理
- 例: `granite_like`, `basalt_like`, `sandstone_like`, `limestone_like`, `mixed`
- 目的: 反射特性や破壊しきい値など、見た目以外の挙動に効かせる

### 2.3 形状軸 (Geometry)

- 丸み、角張り、縦横比、局所粗さスケール、凹凸密度
- 目的: シルエットと接触判定の安定化

### 2.4 劣化/履歴軸 (Weathering & History)

- 風化、摩耗、割れ痕、浸食方向性
- 目的: 「新鮮な破断面」と「長期摩耗面」の差を出す

> 原則: 「地名」より「環境因子」を優先する。  
> 地名は補助メタとして保持し、モデル条件には環境因子へ変換して入力する。

---

## 3. Schema v1 (実務ラベル仕様)

本節は、収集チームと生成チームが共通で使う仕様。

## 3.1 必須ラベル

- `sample_id` (string): 一意 ID
- `environment_type` (enum): 生成/収集時の環境分類
- `material_family` (enum): 鉱物群の大分類
- `roundness` (float 0.0-1.0): 丸み指標
- `angularity` (float 0.0-1.0): 角張り指標
- `roughness_scale` (enum: `micro`, `meso`, `macro`, `multi`)
- `weathering_level` (float 0.0-1.0): 風化の進行度
- `label_confidence` (float 0.0-1.0): 総合ラベル信頼度
- `capture_source` (enum: `real_cross_pol`, `real_non_pol`, `synthetic`, `hybrid`)

## 3.2 任意ラベル

- `location_text` (string): 地名や採取地点の自由記述
- `moisture_state` (enum: `dry`, `damp`, `wet`)
- `porosity_proxy` (float 0.0-1.0): 空隙率の近似指標
- `layering_strength` (float 0.0-1.0): 層理の強さ
- `grain_size_class` (enum: `fine`, `medium`, `coarse`, `mixed`)
- `color_cluster` (string): 色群 ID
- `notes` (string): 注記

## 3.3 ラベル形式の基本

- 離散値と連続値を併用する
- 不確実な項目は `unknown` を許容する
- hard label のみでなく confidence を必須にする
- 判定不能は `unresolved` として明示し、無理に埋めない

---

## 4. 破壊/衝突ラベル (v1 初版)

実験設備なしでも付与可能な、近似ベースの物理ラベルを定義する。

## 4.1 必須 (物理)

- `impact_response_class` (enum)
  - `mostly_bounce`
  - `mixed_bounce_absorb`
  - `mostly_absorb`
- `fracture_susceptibility` (float 0.0-1.0): 割れやすさ
- `fragmentation_tendency` (enum: `low`, `medium`, `high`)
- `surface_friction_class` (enum: `low`, `medium`, `high`)
- `contact_stability_class` (enum: `stable`, `conditionally_stable`, `unstable`)

## 4.2 任意 (物理)

- `estimated_elasticity` (float 0.0-1.0)
- `estimated_energy_dissipation` (float 0.0-1.0)
- `crack_orientation_bias` (enum: `none`, `weak`, `strong`)
- `failure_mode_hint` (enum: `chipping`, `cleaving`, `crumbling`, `mixed`)

## 4.3 付与時の注意

- 物理ラベルは「真値」ではなく「運用可能な近似」
- 定義を固定し、チーム内で主観差を減らす
- 条件（落下高さ、接触材質など）をメタとして残す

---

## 5. 曖昧さへの対処 (専門家なし運用)

## 5.1 判定ルール

- 自信が低い場合は `unknown`/`unresolved` を選ぶ
- `label_confidence < 0.5` のサンプルは学習本流へ直接投入しない
- 専門判定が必要なものは `review_queue` に回す

## 5.2 最小運用セット (最初に集める)

初期段階は以下だけで開始可能。

- `environment_type`
- `material_family`
- `roundness`
- `angularity`
- `weathering_level`
- `impact_response_class`
- `fracture_susceptibility`
- `label_confidence`

---

## 6. データ運用ルール (Real + Synthetic)

## 6.1 収集戦略

- 少量高品質 real (`cross-pol` 含む) を seed にする
- synthetic を大規模生成して分布を広げる
- pseudo-label は段階的に採用する

## 6.2 品質ゲート

採用条件の例:

- トポロジ/形状破綻がない
- ラベル一貫性がある (例: `roundness` と `angularity` が矛盾しない)
- 物理ラベルが極端に矛盾しない (例: `mostly_bounce` かつ `high` 消散は要再確認)
- `label_confidence >= threshold`

棄却または保留条件の例:

- 見た目は石でも材質/環境軸が不整合
- 生成アーティファクトが強い
- ラベル付与者間で判断が割れる

## 6.3 データ分割方針

- train/val/test を環境軸と材質軸で層化分割する
- 同一個体由来の近縁サンプルは別 split に跨がせない
- test は real 比率を高め、合成過学習を監視する

---

## 7. ゲーム側条件との対応 (Condition Mapping)

ゲームの地名/バイオーム情報をそのままモデルに渡さず、ラベル空間へ写像する。

## 7.1 変換方針

- `biome` -> `environment_type`
- `humidity` -> `moisture_state`, `weathering_level`
- `geology_hint` -> `material_family`
- `slope/water_flow` -> `roundness`, `angularity` の prior

## 7.2 未指定時の推奨デフォルト

- `environment_type = talus_slope`
- `material_family = mixed`
- `roundness = 0.45`
- `angularity = 0.55`
- `weathering_level = 0.40`
- `impact_response_class = mixed_bounce_absorb`
- `fracture_susceptibility = 0.45`

---

## 8. v1 で決める範囲 / 後で拡張する範囲

## 8.1 v1 で固定する

- 必須ラベル一覧
- confidence 運用
- 品質ゲート最小セット
- ゲーム条件 -> ラベル変換ルール

## 8.2 将来拡張

- 破壊挙動の高精度ラベル (実測ベース)
- 詳細鉱物学ラベル (専門家監修)
- 高価な capture の追加モダリティ
- 生成モデルごとの最適ラベル重み
