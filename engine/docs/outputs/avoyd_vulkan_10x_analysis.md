# Avoyd (enkisoftware) — Vulkan が OpenGL 比 10x 高速だった理由の技術分析

**調査日**: 2026-03-04
**対象**: Avoyd 0.21（2024年4月リリース）— GPU 加速ボクセルパストレーサー
**目的**: 性能差の根本原因を特定し、oRoNOiDE のカスタム Sparse Brick Pool 開発に活かせる知見を抽出する

---

## 1. 何が 10x だったのか — 正確な定義

enkisoftware の Doug Binks は、Avoyd 0.21 の開発記事で以下のように述べている:

> "I first discovered that the GLSL code ran over ten times faster using Vulkan instead of OpenGL"
> — [Voxel GPU Rendering in Avoyd 0.21](https://www.enkisoftware.com/devlogpost-20240426-1-Voxel-GPU-Rendering-in-Avoyd-0.21)

これは **CPU vs GPU の比較ではない**。**同一 GPU 上で、同一の GLSL シェーダーコードを OpenGL API と Vulkan API でそれぞれ実行した際の差**である。

### Avoyd のアーキテクチャ概要

| 要素 | 詳細 |
|------|------|
| データ構造 | SVO-DAG（Sparse Voxel Octree - Directed Acyclic Graph） |
| レンダリング | GPU パストレーシング（ウェーブフロントアーキテクチャ） |
| シェーダー言語 | GLSL |
| GPU バッファ | SSBO（Shader Storage Buffer Object）にオクツリーノード配列を格納 |
| 対象スケール | 最大 33,696 x 256 x 13,312 ボクセルのシーン |

### CPU vs GPU の別途ベンチマーク（参考）

以下は API の 10x とは別の指標だが、Avoyd のシステム全体の性能特性を理解する上で参考になる:

| テスト | CPU (i9-7980XE, 35T) | GPU (RTX 4070) | 倍率 |
|--------|----------------------|----------------|------|
| レイキャスト（影なし） | 105 ms | 13 ms | 8.1x |
| レイキャスト（影あり） | 161 ms | 25 ms | 6.4x |
| PT（影のみ） | 175 ms | 72 ms | 2.4x |
| PT（全効果） | 370 ms | 447 ms | 0.83x |

PT（全効果）で GPU が CPU より遅くなる理由は後述する（セクション 3.2）。

---

## 2. 10x の原因分析

### 2.1 OpenGL のグローバルステートマシンオーバーヘッド

OpenGL はグローバルステートマシンとして設計されている。`glClearColor()`, `glUseShader()`, `glBindVertexArray()` 等の各呼び出しが中央集約的な状態を変更し、ドライバはその都度、整合性バリデーションを実行する。

Vulkan はこれを Pipeline State Object (PSO) で置き換える。全てのレンダリングステートをパイプライン作成時に一度だけコンパイルし、ランタイムでは `vkCmdBindPipeline()` で切り替えるだけ。バリデーションはゼロ。

**Avoyd のケースでの影響度: 中〜高**

ボクセルパストレーサーは計算シェーダー（or フラグメントシェーダー）を繰り返しディスパッチする。OpenGL ではディスパッチごとに状態チェックが発生するが、Vulkan ではコマンドバッファに記録済みのコマンドを再生するだけ。ウェーブフロント PT では複数のカーネルを連続ディスパッチするため、この差が蓄積する。

### 2.2 SPIR-V コンパイラの品質差

OpenGL と Vulkan ではシェーダーのコンパイルパスが異なる:

```
OpenGL:
  GLSL ソース → ドライバ内蔵 GLSL コンパイラ → ドライバ固有 IR → GPU マシンコード

Vulkan:
  GLSL ソース → glslang → SPIR-V → ドライバの SPIR-V コンパイラ → ドライバ固有 IR → GPU マシンコード
```

一見 Vulkan の方がパスが長いが、実際にはドライバの SPIR-V コンパイラの方が新しく最適化されている。

NVIDIA は 2018 年のドライバ 396.18 で、SPIR-V を NVVM（NVIDIA の内部 IR）に直接コンパイルする新しいパスを導入した。それ以前は SPIR-V を一旦 "GLSL スタイル IR" に変換してから OpenGL コンパイラに渡していた。直接コンパイルへの切り替えにより:

- シェーダーコンパイル速度: **3x 高速化**
- メモリ使用量: **50% 削減**

つまり、Vulkan のドライバコンパイラは OpenGL のレガシーコンパイラより積極的に最適化を行える。特に SVO-DAG トラバーサルのような複雑な制御フロー（深いループネスト、条件分岐の多いツリー探索）では、コンパイラの品質差がそのまま実行性能の差になる。

**Avoyd のケースでの影響度: 高**

SVO-DAG のレイトラバーサルは、オクツリーの階層を再帰的に降下しながら空領域をスキップする複雑なシェーダー。ループ展開、分岐予測、レジスタ割り当ての最適化品質がそのまま性能を支配する。

### 2.3 暗黙的同期の排除

OpenGL は SSBO への読み書きに対して暗黙的なメモリバリアを挿入する場合がある。ドライバが「安全側」に倒して不要な同期を挿入すると、GPU パイプラインがストールする。

Vulkan では同期は完全に明示的。`vkCmdPipelineBarrier()` を必要な箇所にのみ配置することで、不要なストールを完全に排除できる。

**Avoyd のケースでの影響度: 中**

Avoyd は SVO-DAG のノードバッファ（uint32 配列）を SSBO に格納している。パストレーシング中はリードオンリーだが、OpenGL ドライバがこれを正しく認識せず、暗黙的バリアを挿入した可能性がある。

### 2.4 API ローダーディスパッチコスト

OpenGL の各 API コールは以下のパスを辿る:

```
アプリケーション → GL ローダー（関数ポインタテーブル参照）→ ドライバ → GPU コマンド
```

ディスパッチテーブルのルックアップは 1 回あたりは軽微だが、フレームあたり数千〜数万回の API コールで蓄積する。

Vulkan のコマンドバッファは一度記録すれば、以降の再生はほぼゼロコスト。

**Avoyd のケースでの影響度: 低〜中**

パストレーシングのディスパッチ回数はラスタライズほど多くないが、ウェーブフロント PT では bounce ごとにディスパッチが発生するため、無視できないコストになりうる。

### 2.5 SPIRV-opt のバグ（特定 GPU での問題）

Avoyd は特定の GPU で SPIRV-opt（SPIR-V シェーダー最適化ツール）のバグに遭遇した。複雑な SVO-DAG トラバーサルシェーダーが正しく最適化されず、性能低下やレンダリングエラーが発生。

これは Vulkan 固有の問題（OpenGL にはこのパスがない）だが、バグを回避した後の Vulkan 性能が OpenGL を大幅に上回ったことから、OpenGL 側の最適化品質の低さが相対的に浮き彫りになった。

**Avoyd のケースでの影響度: 直接的な原因ではないが、調査過程で OpenGL との差が発覚した契機**

### 2.6 原因の重み付け推定

enkisoftware は 10x の内訳を公式には分解していない。しかし、上記の情報と一般的な知見から以下の推定が可能:

| 要因 | 推定寄与度 | 根拠 |
|------|----------|------|
| SPIR-V コンパイラの品質差（コード生成最適化） | **40-50%** | 複雑なトラバーサルシェーダーはコンパイラ品質に最も敏感。NVIDIA の直接 SPIR-V→NVVM パスの導入で公式に 3x のコンパイル効率化が確認されており、生成コードの品質向上は実行性能にも直結 |
| ステートバリデーション + ローダーオーバーヘッド | **20-30%** | ウェーブフロント PT の複数ディスパッチで蓄積。API コールの頻度に比例 |
| 暗黙的同期の排除 | **15-25%** | 大きな SSBO（SVO-DAG ノードバッファは数百 MB 〜 2GB）へのアクセスパターンで顕在化 |
| レンダーパス最適化 | **5-10%** | Vulkan の明示的レンダーパスによるメモリアクセスパターンの最適化 |

---

## 3. Avoyd が採用した追加最適化

### 3.1 メガカーネル → ウェーブフロント パストレーシング

初期実装ではパストレーシング全体を1つの巨大シェーダー（メガカーネル）で実行していた。これを NVIDIA の Laine & Karras (2013) が提唱した**ウェーブフロント パストレーシング**に移行:

```
メガカーネル（全処理を1シェーダーで実行）:
  for each pixel:
    for each bounce:
      intersect → shade → generate next ray
  問題: bounce ごとにスレッドの挙動が分岐（SIMD divergence）

ウェーブフロント（処理を段階別カーネルに分離）:
  Kernel 1: 全レイの交差判定 → 結果をバッファに書き出し
  Kernel 2: 交差結果の分類（反射・屈折・吸収・終了）
  Kernel 3: 各マテリアルタイプ別にシェーディング
  Kernel 4: 次のレイを生成
  利点: 各カーネル内のスレッドが同種の処理を行い、divergence が最小化
```

**なぜメガカーネルが GPU で遅いのか:**

GPU の SIMT（Single Instruction, Multiple Threads）実行モデルでは、ワープ（32スレッド）が同一命令を実行する。パストレーシングのメガカーネルでは:

1. **分岐 divergence**: 石に当たったスレッドと空に飛んだスレッドが同じワープ内にいると、両方の分岐を実行し、片方はマスクアウトされて計算能力を浪費する
2. **反復回数の不均一**: bounce 回数がスレッドごとに異なる（1 bounce で終了 vs 8 bounce まで続く）。ワープ全体が最も遅いスレッドの速度で進行する
3. **レジスタ圧迫**: 全処理を1つのカーネルに詰め込むとレジスタ使用量が増大し、GPU 上の同時実行スレッド数（occupancy）が低下。レイテンシ隠蔽能力が減少する

Avoyd のベンチマークがこれを如実に示している:

| テスト | CPU (ms) | GPU (ms) | GPU/CPU |
|--------|----------|----------|---------|
| レイキャスト（影なし） | 105 | 13 | 8.1x faster |
| PT（全効果） | 370 | 447 | 0.83x (GPUが遅い) |

レイキャストは全スレッドが均一な処理を行うため GPU が得意。パストレーシングは分岐が多く divergence が深刻になるため、最適化前のメガカーネルでは GPU が CPU に負けた。ウェーブフロント化によりこれを改善し、最終リリースでは GPU が 5-10x 高速に。

### 3.2 SVO-DAG のメモリ効率とキャッシュ性能

Avoyd の SVO-DAG は通常の SVO に対してメモリを約 4x 削減する:

```
通常の SVO:
  各ノードが独自の子ノードを持つ → 同一パターンの領域でもノードが重複

SVO-DAG:
  同一の子サブツリーを共有（参照カウント + copy-on-modify）
  複数の親ノードが同じ子を指す → ノード重複が排除

実測例（Avoyd のデータセット）:
  SVO: ~4x のメモリ
  SVO-DAG: 1x（ベースライン）
  → 約 75% のメモリ削減
```

研究レベルではさらに圧縮が進んでおり:

| 構造 | 128K^3 ボクセル (19B voxels) | 256K^3 相当 |
|------|------|------|
| SVO（ポインタなし） | 5.1 GB | — |
| SVDAG | 945 MB | — |
| SSVDAG（対称性考慮） | — | 0.048 bits/voxel |

**GPU キャッシュとの関係**: DAG によるノード共有は、同一のノードが複数の親から参照されることを意味する。頻繁にアクセスされるノードが GPU の L1/L2 キャッシュに残りやすくなり、メモリ帯域の消費が減少する。特にボクセルシーンでは大量の「空」ノードが共有されるため、キャッシュヒット率の向上効果が大きい。

### 3.3 std430 レイアウトの重要性

Avoyd は開発中に `std140` と `std430` の SSBO レイアウトの違いにより、データが GPU に正しく転送されないバグに遭遇した。

```
std140: 各要素が 16 バイト境界にアラインされる
  → uint32 配列の場合、各要素が 16 バイトに膨張（12 バイトが無駄）
  → メモリ帯域が 4x 無駄に消費される

std430: 要素の自然なアラインメントに従う
  → uint32 配列は 4 バイト間隔で格納される
  → メモリ帯域の無駄がない
```

SVO-DAG のノードバッファは uint32 の大きな配列であるため、`std140` を使うとメモリ帯域が 4x 悪化し、レイトラバーサルの性能が壊滅的に低下する。`std430` への切り替えは必須。

---

## 4. oRoNOiDE への適用可能性

### 4.1 カスタム Sparse Brick Pool 設計への直接的知見

| Avoyd の知見 | oRoNOiDE への適用 | 影響するフェーズ |
|-------------|-----------------|---------------|
| **Vulkan の PSO + 明示的同期が OpenGL 比 10x** | oRoNOiDE は既に Vulkan を採用しており、この恩恵を自動的に享受する。逆に言えば、Vulkan の利点を最大化するために**暗黙的同期を模倣するような設計パターンを避ける**ことが重要 | 全フェーズ |
| **SPIR-V コンパイラの品質がトラバーサル性能を支配** | Brick Pool の GPU トラバーサルシェーダーは SPIR-V の品質に直結する。glslang の最適化オプションを積極的に活用し、複雑な制御フローを**コンパイラが最適化しやすい形**に書くことが重要 | Phase 3-4 |
| **std430 レイアウトの必須性** | Brick Pool を SSBO として GPU に公開する際、`std430` を使用すること。`std140` では uint32/float16 配列のメモリ帯域が壊滅的に悪化する | Phase 1 以降 |
| **ウェーブフロント化による divergence 対策** | SDF レイマーチング + マテリアル評価をメガカーネルにせず、段階別カーネルに分離することを検討。特にマテリアルタイプ別のシェーディングは divergence の主因になりうる | Phase 4 |
| **DAG のノード共有によるキャッシュ効率** | Brick Pool では DAG は不要だが、**ブリック内のボクセル配置を Morton/Z-order にすること**で同等のキャッシュ効率を達成できる。空間的に近いボクセルがメモリ上も近い配置を保証する | Phase 3-4 |

### 4.2 Vulkan 固有の最適化ポイント

Avoyd の経験から、oRoNOiDE の Brick Pool GPU パスで特に注意すべき点:

#### 4.2.1 コマンドバッファの再利用

ブリックプールのレンダリングパスが毎フレーム同じ構造であれば、コマンドバッファを毎フレーム再記録せず、一度記録したものを再利用する。変更があったブリックのみセカンダリコマンドバッファを更新する。

#### 4.2.2 パイプラインバリアの最小化

SDF 評価、マテリアル参照、レイマーチングの各パス間で、必要最小限のパイプラインバリアのみを挿入する。特に:

- ブリックデータが read-only のパス間ではバリア不要
- 風化更新（write）→ レンダリング（read）の遷移のみバリアが必要
- `VK_ACCESS_SHADER_WRITE_BIT → VK_ACCESS_SHADER_READ_BIT` を正確に指定

#### 4.2.3 SSBO のメモリレイアウト設計

Avoyd の std140 バグから学ぶべきこと:

```glsl
// 悪い例: std140 では float16 配列が 16 バイト境界にアラインされる
layout(std140, binding = 0) buffer BrickPool {
    uint data[];  // 各要素が 16 バイトに膨張
};

// 良い例: std430 では自然なアラインメント
layout(std430, binding = 0) buffer BrickPool {
    uint data[];  // 各要素が 4 バイト
};
```

マルチチャネル（SDF + マテリアル）の格納方式:

| 方式 | キャッシュ効率 | 柔軟性 |
|------|-------------|--------|
| インターリーブ（1ブリック内に全チャネル） | SDF + マテリアルを同時に読む場合に最適 | チャネル追加時にレイアウト変更が必要 |
| プレーナー（チャネル別バッファ） | 単一チャネルのみ読む場合に最適 | チャネルの独立追加が容易 |

推奨: Phase 1-2 ではプレーナー（柔軟性優先）、Phase 3-4 でプロファイリング結果に基づきインターリーブに移行する余地を残す。

### 4.3 ウェーブフロント化の設計指針

Avoyd が PT のメガカーネルで GPU が CPU に負けた事例は、oRoNOiDE のレイマーチング設計に直接的な教訓を与える:

```
oRoNOiDE のレイマーチング（メガカーネル設計 — 避けるべき）:
  for each ray:
    traverse brick pool hierarchy
    evaluate SDF at each step
    if hit: evaluate material, compute shading
    if miss: continue or sample environment
  問題: SDF の step 数がレイごとに異なる + マテリアルの分岐

oRoNOiDE のレイマーチング（ウェーブフロント設計 — 推奨）:
  Pass 1: 全レイの brick pool トラバーサル（空ブリックスキップ）
  Pass 2: ヒットしたレイのみ SDF 精密評価
  Pass 3: マテリアルタイプ別シェーディング
  Pass 4: 次のバウンスのレイ生成（反射・屈折のみ）
```

### 4.4 Phase 4（完全 GPU 直接マップ）への具体的示唆

Phase 4 ではブリックプール上で自前の HDDA を実装する必要がある。Avoyd の SVO-DAG トラバーサルの経験から:

1. **空領域スキップが性能の鍵**: SVO-DAG では空ノードを一括ジャンプすることで、レイが空間を高速に横断する。Brick Pool では「空ブリック」のスキップが同等の役割を果たす。空ブリックの索引（ハッシュテーブル or 階層インデックス）の効率が Phase 4 の性能を支配する

2. **シェーダーの複雑性とコンパイラ品質**: Avoyd の 10x の大部分はコンパイラ品質差に起因する。Phase 4 のトラバーサルシェーダーは SVO-DAG と同等以上に複雑になりうる。SPIR-V の生成品質を意識し、glslang のオプション（`--target-env vulkan1.2`, 最適化レベル）を慎重に選定する

3. **レジスタ圧迫の回避**: トラバーサル + SDF 評価 + マテリアル参照を1つのシェーダーに詰め込むとレジスタ使用量が爆発する。ウェーブフロント化で各パスのレジスタフットプリントを制御する

---

## 5. 参考文献

### enkisoftware / Avoyd

- Binks, D. (2024). [Voxel GPU Rendering in Avoyd 0.21](https://www.enkisoftware.com/devlogpost-20240426-1-Voxel-GPU-Rendering-in-Avoyd-0.21)
- Binks, D. (2023). [Implementing a GPU Voxel Octree Path Tracer](https://www.enkisoftware.com/devlogpost-20230823-1-Implementing-a-GPU-Voxel-Octree-Path-Tracer)

### ウェーブフロント パストレーシング

- Laine, S., Karras, T., & Aila, T. (2013). [Megakernels Considered Harmful: Wavefront Path Tracing on GPUs](https://research.nvidia.com/publication/2013-07_megakernels-considered-harmful-wavefront-path-tracing-gpus). HPG 2013.

### SVO-DAG

- Kämpe, V., Sintorn, E., & Assarsson, U. (2013). [High Resolution Sparse Voxel DAGs](https://jcgt.org/published/0006/02/01/). ACM Trans. Graphics.
- Villanueva, A. J., Marton, F., & Gobbetti, E. (2018). [SSVDAGs: Symmetry-aware Sparse Voxel DAGs](https://dl.acm.org/doi/10.1145/3130800.3130831). ACM Trans. Graphics.

### Vulkan vs OpenGL 性能

- NVIDIA (2018). [Driver 396.18 — New SPIR-V Compiler](https://www.phoronix.com/review/nvidia-396-nvvm). 直接 SPIR-V→NVVM コンパイルにより 3x 高速化、50% メモリ削減を達成。
- Heidelberg University. [Comparison: Vulkan vs OpenGL](https://pille.iwr.uni-heidelberg.de/~vulkan01/compare.html). アーキテクチャの構造的差異の解説。
- AMD GPUOpen. [Reducing Vulkan API Call Overhead](https://gpuopen.com/learn/reducing-vulkan-api-call-overhead). ローダーディスパッチコスト、PSO 設計の詳細。
