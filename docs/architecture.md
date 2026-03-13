## アーキテクチャ: 境界面シミュレーション

### 設計思想

「単純なルール + 十分な変数の多様性」から複雑性を創発させる。
ルール自体を複雑にするのではなく、同じ単純なルールが異なるパラメータで
動作することで多様な構造が生まれる。

### シミュレーション: Multi-channel Lenia

連続セルオートマトン Lenia の多チャンネル拡張。

**更新ルール（各フィールド i, 各セル x,y）:**

```
potential_i(x,y) = Σ_j  weight[i,j] * convolve(field_j, kernel[i,j])
growth_i = 2 * exp(-(potential - gc_i)² / (2 * gw_i²)) - 1
field_i += dt * growth_i
field_i = clamp(field_i, 0, 1)
```

保存則なし、シャープニングなし、RPS なし。純粋な convolve → growth → update。

### Laws → Params 導出

9個のメタパラメータ（Laws）から全ランタイムパラメータ（Params）を決定論的に導出する。

**Laws（9変数）:**
- `gc[3]`: Growth center — 各フィールドが好む potential 密度
- `gw[3]`: Growth width — growth 関数の選択性
- `scale[3]`: Spatial scale — 自己相互作用カーネルの半径

**導出ルール:**
- 対角カーネル (i==j): radius = scale[i], width = scale[i] * 0.13, weight = 1.0
- 非対角カーネル (i!=j): radius = sqrt(scale[i] * scale[j]), weight = cross_weight(gc, scale)
- cross_weight: gc差分とscale差分から導出、反対称（weight(i,j) = -weight(j,i)）
- dt: min(gw) * 0.8（最も選択的なフィールドに合わせる）

**設計上の帰結:**
- 全フィールドが同じ性質 → coupling = 0 → 独立動作
- 性質の差がcouplingを生む（差が大きいほど強い相互作用）
- 反対称性により、一方向的な支配は起きない

### 発見されたパラメータ領域

**F1 プリセット（3フィールド非平衡共存）:**
```
gc=[0.077, 0.097, 0.064]  gw=[0.0098, 0.0123, 0.0058]  scale=[5.1, 4.0, 13.6]
```
R, G, B すべてが局所的な非平衡構造として真空上に存在できる。

### 未解決の課題

- **飽和問題**: 保存則がないため、構造は空間を埋め尽くすと平衡に達して変化が止まる
- **全フィールド同時ソリトン**: 個別フィールドのソリトン候補は見つかっているが、3フィールド同時は未達成
- **3フィールドの構造多様性**: ゲームに十分な多様性を生めるかは未検証

### 技術スタック

- Rust (edition 2024)
- wgpu 25 + winit 0.30 (自前レンダリング、Bevyは不使用)
- WGSL シェーダー (フルスクリーンクアッド)
