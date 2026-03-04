# Stone Report Gap Analysis

対象比較:
- `engine/docs/outputs/hyperreal_stone_technology_report.md`（以下 Hyperreal）
- `engine/docs/outputs/stone_structural_realism_report.md`（以下 Structural）

本レポートは、Structural を自己レビューする目的で、  
1) Structural で不足していた観点、2) Structural の方が優れていた観点、を分離して整理する。

---

## 1. 比較サマリー（結論）

- **網羅性・技術粒度は Hyperreal が優位**  
  具体技術名、実装候補、研究参照、適用条件まで踏み込んでいる。
- **設計判断の明瞭性は Structural が優位**  
  単一権威状態、責務分離、代替案不採用理由、実装順序が短く明快。
- **最適解は統合**  
  Structural の設計軸を中核に、Hyperreal の具体技術カタログを追補するのが最も強い。

---

## 2. Structural で不足していた観点（Hyperreal 比）

## 2.1 データ構造の実装具体化（不足）

Structural は「sparse volume / VDB 系」と記述しているが、Hyperreal は **NanoVDB を明示選定**し、二層構造（権威層 + Neural/Procedural ディテール層）まで具体化している。  
また Hyperreal は圧縮レイヤ（NeuralVDB, fVDB）まで記述しており、実装検討の入口が明確。

不足していた内容:
- 候補技術の比較表（長所/短所/採用理由）
- データ圧縮・転送・GPU実行の実装論点
- 「権威層とディテール層の分離」の設計図

## 2.2 外観技術の粒度とカバレッジ（不足）

Structural は MECE 分解は良いが、各カテゴリの技術が抽象的。  
Hyperreal は外観を細分化して、個別アルゴリズムを割り当てている。

Hyperreal 側の具体化例:
- 鉱物分布: 3D Worley + Voronoi + phase-field 結晶成長
- 層構造: reaction-diffusion（Liesegang）+ Gabor noise
- 微細形状: Nanite tessellation + procedural displacement
- 光学応答: glint, position-normal manifold, Belcour thin-film
- 色/BRDF生成: ReflectanceFusion, MatDecompSDF, NeuMaDiff
- 半透明: Hybrid ReSTIR-PT SSS, volumetric PT, SSS-GS

不足していた内容:
- 現象ごとの「専用技術」への割当て
- 適用条件（どの鉱物/状態で有効か）
- 手法選定の技術的理由（なぜ Perlin でなく Gabor か等）

## 2.3 レンダリングパイプライン定義（不足）

Structural は Hybrid 方針を提示しているが、フレーム内の処理分解は簡潔に留まる。  
Hyperreal は G-Buffer → DI → GI → 特殊効果 → ポストの段階で、機能配置を詳細化している。

不足していた内容:
- ReSTIR DI/GI/BDPT や NRC の位置づけ
- 石固有効果（glint, thin-film, SSS）のパス割り当て
- 参照品質モード（検証モード）の定義

## 2.4 植生統合の生物学的モデル（不足）

Structural は BioLayerEntity の枠組みはあるが、成長モデルは抽象度が高い。  
Hyperreal は Space Colonization / L-system / Riemannian L-system を明示し、成長条件の導出フローを具体化している。

不足していた内容:
- どの成長アルゴリズムを採用するか
- 石フィールドから植生条件へ変換する式/ルール
- 根浸透による石側フィールド更新の具体例

## 2.5 参照文献・研究裏付け（不足）

Structural には参考文献セクションがないため、調査トレーサビリティが弱い。  
Hyperreal は文献付録があり、探索再現性が高い。

不足していた内容:
- 主要論文/技術の参照リスト
- 「研究段階 / 製品レベル」の成熟度表示

---

## 3. Structural の方が優れていた観点

## 3.1 意思決定の明瞭さ（優位）

Structural は冒頭で三本柱を固定し、設計の判断基準を最短距離で提示している。  
Hyperreal は網羅的で強い一方、情報量が多く、読み手が主軸を掴むまで時間がかかる。

Structural の優位点:
- Executive Decision が明確
- 単一権威状態の主張が終始ぶれない
- 「何を採用しないか」が明示されている

## 3.2 アーキテクチャ抽象の整理（優位）

Structural は `RockEntity` / `BioLayerEntity`、ID体系、version parity、不変条件を短く接続し、設計上の最重要制約が明確。  
Hyperreal は要素が豊富だが、技術カタログに比重があり、統治ルールの簡潔さでは Structural が勝る。

## 3.3 実装ロードマップの実行性（優位）

Structural には「高確度シーケンス」があり、実装順が明確。  
Hyperreal は技術選定の厚みは高いが、実装順序の優先度は相対的に弱い。

Structural の優位点:
- 先に ID/picking/proxy を成立させる現実的順序
- 破壊統合を event-driven に限定する段階設計
- 最終的に path 寄与を拡張するスケール計画

## 3.4 設計原則の一貫性（優位）

Structural は「見た目と構造の分離を避ける」という原則に忠実で、記述が過不足なくコンパクト。  
Hyperreal は優秀だが、詳細技術が多いため運用時に「どれを必須にするか」の取捨が別途必要になる。

---

## 4. 統合改訂ガイド（次版で最強化するには）

優先度順で、Structural に追補すべき項目を定義する。

### Priority 1（必須）
- データ構造の実名化: NanoVDB（二層構造）を本文に明記
- 外観マップの具体化: 各カテゴリに最低2つの具体技術を割当て
- レンダリング段階表: Hybrid 内のパス分担（DI/GI/SSS/特殊効果）を表形式で追加

### Priority 2（強く推奨）
- 植生モデルの具体化: Space Colonization + L-system 系を明示
- 適用条件表: mineral/state によるシェーディング分岐条件を追加
- 技術成熟度タグ: `production` / `promising-research` を併記

### Priority 3（補強）
- 参考文献セクション（主要20件程度）
- 実装候補の比較表（候補/利点/欠点/採用理由）
- 参照品質モード定義（キャリブレーション用）

---

## 5. 追補章案（Structural へ追加するなら）

1. `Appendix A: Technology Candidate Matrix`  
2. `Appendix B: Rendering Pass Breakdown`  
3. `Appendix C: Vegetation Growth Coupling Rules`  
4. `Appendix D: References and Maturity Tags`

---

## 6. 最終評価

- **Structural 単体評価:** 設計判断の骨格として非常に強い（短く明快で実装順がある）
- **Hyperreal 単体評価:** 技術辞書として非常に強い（現象カバレッジと研究追跡性が高い）
- **推奨:** Structural をベース文書、Hyperreal を技術付録群として統合する構成が最適

---

## 7. 第三者レビュー: Gap Analysis 自体の妥当性評価

本セクションは、Hyperreal レポートの著者が、Codex による gap analysis（Section 1-6）を公平に評価し、見落とされている論点を補完するものである。自レポート（Hyperreal）に対しても率直に弱点を指摘する。最終的な技術選定のための判断材料を提供することを目的とする。

---

### 7.1 Gap Analysis の評価: 正しく機能している部分

Gap analysis の全体構成と結論は概ね妥当である。特に以下の評価は正確かつ有用:

**正確な評価:**

- **Section 1 の「網羅性 vs 設計明瞭性」のトレードオフ認識** — これは両レポートの本質的な差異を正しく捉えている。Hyperreal は「何を使うか」に強く、Structural は「なぜそう設計するか」に強い。
- **Section 3.2 の ID 体系・version parity の優位性評価** — Structural の `instance_id` / `fragment_id` / `parent_fragment_id` / `bio_patch_id` の4層 ID 体系と `topology_version` による render/physics の同期保証は、ゲームとして動作させるための骨格であり、Hyperreal ではこのレベルの運用設計が薄い。これは正当な指摘。
- **Section 3.3 の実装ロードマップ評価** — 「先に ID/picking/proxy を成立させる」という Structural の順序設計は、エンジン開発において極めて実践的。Hyperreal にはこの段階的実装の視点がほぼ欠けている。
- **Section 6 の統合推奨** — 「Structural をベース、Hyperreal を技術付録」という結論は妥当な方向。

---

### 7.2 Gap Analysis が見落としている重要な論点

以下は、gap analysis が両レポートの比較において見逃している、あるいは過小評価している技術的・設計的論点である。これらは最終技術選定に直接影響する。

#### 7.2.1 NanoVDB の静的トポロジー問題（重大）

Gap analysis の Section 2.1 は「Hyperreal が NanoVDB を明示選定した点」を Structural の不足として記載している。しかし、ここには重要な見落としがある。

**NanoVDB は静的トポロジーを持つ。** 値（SDF 距離値、マテリアルパラメータ等）は変更可能だが、ツリー構造（どのボクセルがアクティブか）は変更できない。破壊操作ではジオメトリが劇的に変化し、新しい表面が露出し、フラグメントが分離する。これは本質的にトポロジー変更であり、NanoVDB の設計制約と正面から衝突する。

Structural が「sparse volume / VDB 系」と抽象的に記述していたのは、知識不足ではなく、**この制約を暗黙的に回避した賢明な非コミット**である可能性がある。Gap analysis はこの差異を「具体性の不足」と一方的に評価しているが、実際には Hyperreal 側に技術的リスクがある。

**最終選定への影響:** 破壊を中核要件とするエンジンでは、NanoVDB は「読み取り最適化された転送・レンダリング用フォーマット」として使い、権威的編集は OpenVDB（動的トポロジー対応）またはカスタム sparse brick pool で行う二段構成が必要。Hyperreal の二層構造はディテール生成の分離として設計されているが、権威層自体がさらに「編集用 + 転送用」に分かれる必要がある。

#### 7.2.2 BTF (Bidirectional Texture Function) の不在

Structural は Section 3.2 で BTF に言及しているが、Hyperreal はこれを扱っていない。Gap analysis もこの差を拾っていない。

BTF は視線方向と入射光方向の両方に依存する外観関数であり、SVBRDF では捉えきれないメソスケールの効果（粗い表面の自己遮蔽、自己相互反射、視差効果）を記録する。添付写真のような粗い表面を持つ石では、カメラや光源が低角度の時にこの効果が顕著に現れる。

SVBRDF は各点を独立に扱うため、隣接する凹凸同士の遮蔽関係を表現できない。BTF はこれをルックアップテーブルとして保持する。ストレージコストは高いが、2030 年以降の前提では制約ではない。

**最終選定への影響:** メソスケール（mm〜cm）の表面粗さ表現として、Hyperreal のプロシージャルディスプレイスメント + SVBRDF に加え、BTF ベースの表現を候補として残すべき。特に「ディスプレイスメントでは計算コストが重すぎるが、法線マップでは不十分」なスケール帯に対して有効。

#### 7.2.3 MPM / MLS-MPM の破壊手法からの欠落

Structural は Section 4.1 で CD-MPM / MLS-MPM を破壊 solver の一候補として明示している。Hyperreal はリサーチ段階で MPM の GPU 実装（JAX-MPM, CRESSim-MPM）を調査しているにもかかわらず、最終的な破壊手法の選定から除外している。Gap analysis もこの差を指摘していない。

MPM は Phase-field や Peridynamics とは異なる破壊モードに強い:
- **大変形・分離・接触**: フラグメントが完全に分離した後の衝突処理が自然
- **粒状崩壊**: 砂岩の粉砕、風化した花崗岩の粒状崩壊
- **マルチフェーズ**: 固体（石）+ 粒子（砕片）+ 粉塵を統一的に扱える

2025 年時点で、GPU 上でのリアルタイム MPM は 1M+ 粒子が達成されている。2030 年には桁が上がることが合理的に期待でき、破壊のインタラクティブな演出に十分。

**最終選定への影響:** 破壊 solver の「役割分担」に MPM を加え、Phase-field（準静的亀裂伝播）、Peridynamics（動的分岐）、MPM（粉砕・大変形・後続の粒状フロー）の三段構えにすべき。

#### 7.2.4 volume_fraction vs 離散 mineral_id

Structural は per-voxel フィールドに `volume_fraction[mineral]` を含めている。これは一つのボクセルが複数鉱物の混合（例: 石英 60% + 長石 40%）であることを表現できる。

Hyperreal は `mineral_id: uint8` のみであり、各ボクセルが単一の鉱物に属する離散表現を採用している。

この差は粒界の遷移帯の表現に影響する。離散表現では粒界は隣接ボクセル間の mineral_id の不連続として現れ、ボクセル解像度に依存する階段状のアーティファクトが生じうる。volume_fraction を使えば、グラデーショナルな遷移が可能になり、より低い解像度でも滑らかな粒界表現が得られる。

一方、volume_fraction はストレージコストが高い（鉱物種数 × float per voxel）。ここにはトレードオフがある。

**最終選定への影響:** 推奨は **ハイブリッド**: 大部分のボクセルには `mineral_id` (離散)、粒界近傍のボクセルにのみ `volume_fraction` を sparse に格納する。これにより、ストレージ効率と遷移帯の滑らかさを両立できる。

#### 7.2.5 明示的力学場 vs ランタイム導出

Structural は `E`（ヤング率）、`nu`（ポアソン比）、`Gc`（破壊靭性）、`sigma_t`（引張強度）を権威的フィールドとして明示的に格納する。Hyperreal はこれらを `mineral_id` + `porosity` から鉱物学的データベースを参照してランタイムに導出する。

Structural のアプローチの利点:
- 風化・損傷により鉱物学的ルックアップから乖離した状態を表現可能（例: 微小亀裂が蓄積して実効的な破壊靭性が低下した石英）
- `damage_d` フィールドが力学場を直接変調できる
- シミュレーション結果の永続化が容易

Hyperreal のアプローチの利点:
- ストレージが軽い（導出元の mineral_id + porosity のみ保持）
- 鉱物学的整合性が自動的に保たれる
- 新しい鉱物種の追加が容易

Gap analysis はこの設計判断の差を全く取り上げていない。

**最終選定への影響:** 推奨は **Structural のアプローチ（明示的格納）を基本とし、初期値は mineral_id から導出**する。これにより:
1. 初期状態では鉱物学的整合性が保たれ
2. 風化・損傷シミュレーションが力学場を直接更新でき
3. セーブ/ロード時に状態が完全に復元される

#### 7.2.6 Nanite のエンジン依存性

Hyperreal はレンダリングパイプラインの複数箇所で Nanite（仮想ジオメトリ、テッセレーション、Visibility Buffer）に言及している。これは UE5 固有の技術であり、自前エンジンでは直接使用できない。

Gap analysis はこの点を完全に見落としている。Structural は特定のエンジン技術に依存しない抽象的な記述（「hybrid renderer」「proxy generation」）をしており、自前エンジンの文脈ではより適切。

Hyperreal が参照している Nanite の**設計思想**（GPU 駆動レンダリング、Visibility Buffer、メッシュレットベースの自動 LOD）は自前エンジンでも実装可能だが、それは「Nanite を使う」のではなく「Nanite 方式のカスタム実装を行う」ことを意味する。この区別は実装計画において重要。

**最終選定への影響:** 技術選定テーブルで Nanite を参照する箇所は「Nanite 方式（GPU 駆動仮想ジオメトリ + Visibility Buffer + 動的テッセレーション）のカスタム実装」と明確に書き換えるべき。

---

### 7.3 Hyperreal レポート自身の弱点（自己批判）

公平性のため、Hyperreal レポートの弱点を Hyperreal 著者の視点から率直に記述する。

1. **設計原則の明文化が弱い**: Structural の「単一権威状態」「責務分離」「ID ベースの追跡」のような、全体を貫く設計原則が Hyperreal では暗黙的にしか存在しない。技術カタログとしては強いが、チームが判断に迷った時に立ち返る「北極星」が不明確。

2. **不採用理由の欠落**: Structural の Section 10（代替案の位置づけ）に相当するセクションが Hyperreal にはない。なぜ NeRF/Gaussian を権威状態にしないのか、なぜ純メッシュ + テクスチャ焼き込みを採用しないのか — これらの判断根拠が暗黙のまま。

3. **技術間の依存関係と優先順位の不足**: 30 以上の技術が列挙されているが、どれが他のどれに依存するか、どの順序で実装すべきか、どれが「必須」でどれが「あれば理想的」かの区分が欠けている。実装チームがこのレポートを受け取った場合、どこから手をつけるべきか判断できない。

4. **NanoVDB の制約の過小評価**: 前述の通り、静的トポロジーの問題を十分に検討せずに NanoVDB を推奨している。

5. **スケーラビリティの未検討**: per-voxel に 6 フィールド（mineral_id, crystal_orientation×3, porosity, microcrack_density, weathering_degree, grain_size）を格納した場合の、1000 個の石を含むシーンでのメモリフットプリントの見積もりがない。NeuralVDB による圧縮を言及しているが、ランタイムでの解凍コストは未検討。

---

### 7.4 Structural レポートの見落とされた強み

Gap analysis は Structural の強みを Section 3 で評価しているが、以下の点が過小評価されている:

#### 7.4.1 coupling field の概念

Structural は石と植生の接続を「接触面積」「付着強度」「水分輸送」の coupling field で定義している（Section 2.1）。Hyperreal は ECS の親子関係 + `RootPenetration` コンポーネントで扱っているが、coupling field の方がより物理的に根拠のあるモデルである。

例: 苔が石から剥離する条件を判定する場合:
- Structural: `adhesion_strength < applied_force` を coupling field 上で評価 — 物理的に自然
- Hyperreal: `RootPenetration` コンポーネントの閾値 — ECS 的にはクリーンだが物理モデルが薄い

#### 7.4.2 damage_d の永続的フィールド化

Structural は `damage_d`（Phase-field 損傷変数）を権威的フィールドの一部として明示的に格納している。これにより:
- 損傷の蓄積が永続化される（同じ箇所を繰り返し叩くと徐々に亀裂が進行する）
- セーブ/ロードで損傷状態が完全に復元される
- 損傷分布の可視化が直接的（デバッグモードで damage_d をカラーマップ表示）

Hyperreal の `FractureState` コンポーネントはこの機能を意図しているが、per-voxel の永続的場としての定義が曖昧。

#### 7.4.3 XFEM + CZM の参照・校正用途

Structural は XFEM + CZM を「高忠実度オフライン参照、校正用」として位置づけている。これはリアルタイムには使わないが、Phase-field や Peridynamics の結果を検証するための**グラウンドトゥルース生成手段**として価値がある。Hyperreal にはこの「校正パイプライン」の視点が欠けている。

---

### 7.5 両レポートに共通する欠落

以下は、Structural にも Hyperreal にも欠けている、しかし最終目標（現実と区別不可能な石）の達成に関わる論点である:

#### 7.5.1 オーディオフィードバック

石を叩いた音、石同士がぶつかる音、石の上を歩く音 — これらは「現実と区別がつかない」体験の重要な構成要素だが、両レポートとも完全に未言及。マテリアルテンソル場（mineral_id, porosity, grain_size）は音響応答のパラメータ化にも直結する。密度と弾性率から固有振動数を導出し、porosity から内部減衰を推定できる。

#### 7.5.2 動的な水のインタラクション

両レポートとも `moisture` パラメータを言及しているが、これは静的な状態値にとどまっている。実際には:
- 雨滴が石の表面を流れ、凹部に水溜りを形成する
- 水が亀裂に浸透し、凍結膨張で亀裂を進展させる（ランタイムの風化）
- 濡れた領域と乾いた領域の境界が時間的に変動する

これらの動的水文シミュレーションは、石のリアリズムの「最後の 10%」を担う要素であり、少なくとも設計段階でデータ構造に水分フロー場を予約しておくべき。

#### 7.5.3 アーティストのオーサリングワークフロー

全てがプロシージャルに生成される場合でも、アーティストが特定の石の外観を微調整したい場面は必ず発生する。「この石の色だけ少し赤みを足したい」「この亀裂のパターンは気に入らないので変えたい」といった要求に対して:
- マテリアル場への直接ペイント（3D テクスチャペイント相当）
- プロシージャルパラメータの局所オーバーライド
- 参照画像からの逆レンダリングによるパラメータフィッティング

これらのワークフローの設計がなければ、技術的に優れたシステムでも制作現場で使えない可能性がある。

#### 7.5.4 シーンスケーラビリティ

単一の石に対する品質は十分に議論されているが、1000+ の石が同時に存在するシーンでの:
- メモリフットプリント
- LOD 戦略（遠方の石はどこまで簡略化するか）
- ストリーミング（近づいた石だけ高解像度データをロードする）
- インスタンシング（同一の石種で形状だけ異なるケースの最適化）

これらの規模問題は、技術選定の段階で考慮しておかないと、後から設計変更が困難になる。

---

### 7.6 最終技術選定に向けた項目別推奨

各ドメインについて、両レポートのどちらの提案を優先すべきか、あるいはどう統合すべきかの推奨を示す。

| ドメイン | 推奨ベース | 統合方針 |
|---------|-----------|---------|
| **設計原則・アーキテクチャ** | Structural | 三本柱 + ID 体系 + version parity + 不変条件をそのまま採用 |
| **権威的データ構造** | 統合 | Structural のフィールド設計（volume_fraction, 明示的力学場, damage_d）+ Hyperreal の二層構造概念。ただし NanoVDB は転送/レンダリング用に限定し、編集用は OpenVDB またはカスタム sparse brick pool |
| **Per-voxel フィールド** | Structural 優先 | Structural の mechanical fields (E, nu, Gc, sigma_t) + damage_d を採用し、初期値は Hyperreal の mineral_id ベース導出で設定 |
| **メッシュ抽出** | Hyperreal | Dual Contouring + Neural DC の具体手法を採用 |
| **鉱物分布生成** | Hyperreal | 3D Worley + Voronoi + Phase-field 結晶成長の具体手法を採用 |
| **層構造・縞模様** | Hyperreal | Reaction-Diffusion + Gabor noise の具体手法を採用 |
| **風化シミュレーション** | Hyperreal | Arenite + Agent-based の具体手法を採用 |
| **マイクロジオメトリ** | 統合 | Hyperreal のプロシージャルディスプレイスメント方針 + Structural の BTF をメソスケール補完として追加 |
| **光学応答** | Hyperreal | Glint, thin-film, SSS の具体手法と適用条件表を採用 |
| **SVBRDF 生成** | Hyperreal | 条件付き拡散モデルの位置づけ（残差補完）を採用 |
| **破壊 solver** | 統合 | Structural の三分類（Phase-field / PD / MPM）+ XFEM 校正。Hyperreal の Voronoi フォールバックを追加し四段構え |
| **弾性・衝突** | 統合 | Hyperreal の三段階モデル（Tier 1-3）+ Structural の明示的力学場からのパラメータ導出 |
| **レンダリングパイプライン** | Hyperreal 優先 | フレームパイプライン詳細を採用。ただし「Nanite」は「カスタム GPU 駆動仮想ジオメトリ」に読み替え |
| **植生統合** | 統合 | Structural の coupling field 概念 + Hyperreal の具体アルゴリズム（Space Colonization, Riemannian L-system） |
| **ゲームオブジェクト** | Structural 優先 | ID 体系（instance_id, fragment_id, parent_fragment_id, bio_patch_id）+ lineage 追跡を採用 |
| **実装順序** | Structural | 高確度シーケンス（Section 11）をそのまま採用 |

---

### 7.7 Gap Analysis 自体への総合評価

**妥当な点:**
- 両レポートの特性差（網羅性 vs 設計明瞭性）の識別は正確
- Structural の不足点（技術具体化、文献、植生モデル）の指摘は正当
- 統合推奨の方向性は正しい

**不十分な点:**
- Hyperreal の技術的リスク（NanoVDB 静的トポロジー、Nanite 依存）を指摘できていない
- Structural 固有の強み（coupling field, damage_d 永続化, XFEM 校正, volume_fraction）を過小評価
- 両レポートの共通欠落（オーディオ、動的水、オーサリング、スケーラビリティ）に気づいていない
- 「Structural に何を追補すべきか」にフォーカスしすぎており、「Hyperreal 側に何が足りないか」の分析が不足

**全体として:** Gap analysis は Structural の自己改善ツールとしては有用だが、**公平な二者比較としては Hyperreal 寄りのバイアスがある**。これは gap analysis の目的が「Structural の自己レビュー」と明記されている（Section 冒頭）ため構造的に仕方がないが、最終技術選定に使う場合はこのバイアスを認識した上で読む必要がある。

本レビュー（Section 7）は、そのバイアスを補正し、両レポートの強みと弱みをより均衡のとれた形で提示することを意図している。

